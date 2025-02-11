/*
 * Copyright (C) 2022 Cloudbase Solutions
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include "pch.h"

#include "mock_wnbd_daemon.h"
#include "utils.h"

MockWnbdDaemon::~MockWnbdDaemon()
{
    if (Started && WnbdDisk) {
        Shutdown();

        WnbdClose(WnbdDisk);

        Started = false;
    }
}

void MockWnbdDaemon::Start()
{
    WNBD_PROPERTIES WnbdProps = {0};

    InstanceName.copy(WnbdProps.InstanceName, sizeof(WnbdProps.InstanceName));
    ASSERT_TRUE(strlen(WNBD_OWNER_NAME) < WNBD_MAX_OWNER_LENGTH)
        << "WnbdOwnerName too long";
    strncpy_s(
        WnbdProps.Owner, WNBD_MAX_OWNER_LENGTH,
        WNBD_OWNER_NAME, strlen(WNBD_OWNER_NAME));

    WnbdProps.BlockCount = BlockCount;
    WnbdProps.BlockSize = BlockSize;
    WnbdProps.MaxUnmapDescCount = 1;

    WnbdProps.Flags.ReadOnly = ReadOnly;
    WnbdProps.Flags.UnmapSupported = 1;
    if (CacheEnabled) {
        WnbdProps.Flags.FUASupported = 1;
        WnbdProps.Flags.FlushSupported = 1;
    }

    if (UseCustomNaaIdentifier) {
        WnbdProps.Flags.NaaIdSpecified = 1;
        WnbdProps.NaaIdentifier.data[0] = 0x60;
        for (int i = 1; i < sizeof(WnbdProps.NaaIdentifier.data); i++)
            WnbdProps.NaaIdentifier.data[i] = (BYTE)rand();
    }

    if (UseCustomDeviceSerial)
        strcpy(WnbdProps.SerialNumber,(std::to_string(rand()) + "-"
               + std::to_string(rand())).c_str());

    DWORD err = WnbdCreate(
        &WnbdProps, (const PWNBD_INTERFACE) &MockWnbdInterface,
        this, &WnbdDisk);
    ASSERT_FALSE(err) << "WnbdCreate failed";

    Started = true;

    err = WnbdStartDispatcher(WnbdDisk, IO_REQ_WORKERS);
    ASSERT_FALSE(err) << "WnbdStartDispatcher failed";

    if (!ReadOnly) {
        SetDiskWritable(InstanceName);
    }
}

void MockWnbdDaemon::Shutdown()
{
    std::unique_lock<std::mutex> Lock(ShutdownLock);
    if (!Terminated && WnbdDisk) {
        TerminateInProgress = true;
        // We're requesting the disk to be removed but continue serving IO
        // requests until the driver sends us the "Disconnect" event.
        DWORD Ret = WnbdRemove(WnbdDisk, NULL);
        if (Ret && Ret != ERROR_FILE_NOT_FOUND)
            ASSERT_FALSE(Ret) << "couldn't stop the wnbd dispatcher, err: " << Ret;
        Wait();
        Terminated = true;
    }
}

void MockWnbdDaemon::Wait()
{
    if (Started && WnbdDisk) {
        DWORD err = WnbdWaitDispatcher(WnbdDisk);
        ASSERT_FALSE(err) << "failed waiting for the dispatcher to stop";
    }
}

void MockWnbdDaemon::Read(
    PWNBD_DISK Disk,
    UINT64 RequestHandle,
    PVOID Buffer,
    UINT64 BlockAddress,
    UINT32 BlockCount,
    BOOLEAN ForceUnitAccess)
{
    MockWnbdDaemon* handler = nullptr;
    ASSERT_FALSE(WnbdGetUserContext(Disk, (PVOID*)&handler));

    ASSERT_TRUE(handler->BlockSize);
    ASSERT_TRUE(handler->BlockSize * BlockCount
        <= WNBD_DEFAULT_MAX_TRANSFER_LENGTH);

    WnbdRequestType RequestType = WnbdReqTypeRead;
    WNBD_IO_REQUEST WnbdReq = { 0 };
    WnbdReq.RequestType = RequestType;
    WnbdReq.RequestHandle = RequestHandle;
    WnbdReq.Cmd.Read.BlockAddress = BlockAddress;
    WnbdReq.Cmd.Read.BlockCount = BlockCount;
    WnbdReq.Cmd.Read.ForceUnitAccess = ForceUnitAccess;

    handler->ReqLog.AddEntry(WnbdReq);

    if (Disk->Properties.BlockCount < BlockAddress + BlockCount) {
        // Overflow
        // TODO: consider moving this check to the driver.
        WnbdSetSense(
            &handler->MockStatus,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_VOLUME_OVERFLOW);
    } else {
        memset(Buffer, READ_BYTE_CONTENT, BlockCount * handler->BlockSize);
    }

    handler->SendIoResponse(
        RequestHandle, RequestType,
        handler->MockStatus,
        Buffer, handler->BlockSize * BlockCount);
}

void MockWnbdDaemon::Write(
    PWNBD_DISK Disk,
    UINT64 RequestHandle,
    PVOID Buffer,
    UINT64 BlockAddress,
    UINT32 BlockCount,
    BOOLEAN ForceUnitAccess)
{
    MockWnbdDaemon* handler = nullptr;
    ASSERT_FALSE(WnbdGetUserContext(Disk, (PVOID*)&handler));

    ASSERT_TRUE(handler->BlockSize);
    ASSERT_TRUE(handler->BlockSize * BlockCount
        <= WNBD_DEFAULT_MAX_TRANSFER_LENGTH);

    WnbdRequestType RequestType = WnbdReqTypeWrite;
    WNBD_IO_REQUEST WnbdReq = { 0 };
    WnbdReq.RequestType = RequestType;
    WnbdReq.RequestHandle = RequestHandle;
    WnbdReq.Cmd.Write.BlockAddress = BlockAddress;
    WnbdReq.Cmd.Write.BlockCount = BlockCount;
    WnbdReq.Cmd.Write.ForceUnitAccess = ForceUnitAccess;

    handler->ReqLog.AddEntry(WnbdReq, Buffer, BlockCount * handler->BlockSize);

    WNBD_STATUS Status = handler->MockStatus;

    if (Disk->Properties.BlockCount < BlockAddress + BlockCount) {
        // Overflow
        // TODO: consider moving this check to the driver.
        WnbdSetSense(
            &Status,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_VOLUME_OVERFLOW);
    }

    handler->SendIoResponse(
        RequestHandle, RequestType,
        Status,
        Buffer, handler->BlockSize * BlockCount);
}

void MockWnbdDaemon::Flush(
    PWNBD_DISK Disk,
    UINT64 RequestHandle,
    UINT64 BlockAddress,
    UINT32 BlockCount)
{
    MockWnbdDaemon* handler = nullptr;
    ASSERT_FALSE(WnbdGetUserContext(Disk, (PVOID*)&handler));

    WnbdRequestType RequestType = WnbdReqTypeFlush;
    WNBD_IO_REQUEST WnbdReq = { 0 };
    WnbdReq.RequestType = RequestType;
    WnbdReq.RequestHandle = RequestHandle;
    WnbdReq.Cmd.Flush.BlockAddress = BlockAddress;
    WnbdReq.Cmd.Flush.BlockCount = BlockCount;

    handler->ReqLog.AddEntry(WnbdReq);

    if (Disk->Properties.BlockCount < BlockAddress + BlockCount) {
        // Overflow
        // TODO: consider moving this check to the driver.
        WnbdSetSense(
            &handler->MockStatus,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_VOLUME_OVERFLOW);
    }

    handler->SendIoResponse(
        RequestHandle, RequestType,
        handler->MockStatus, NULL, 0);
}

void MockWnbdDaemon::Unmap(
    PWNBD_DISK Disk,
    UINT64 RequestHandle,
    PWNBD_UNMAP_DESCRIPTOR Descriptors,
    UINT32 Count)
{
    MockWnbdDaemon* handler = nullptr;
    ASSERT_FALSE(WnbdGetUserContext(Disk, (PVOID*)&handler));

    WnbdRequestType RequestType = WnbdReqTypeUnmap;
    WNBD_IO_REQUEST WnbdReq = { 0 };
    WnbdReq.RequestType = RequestType;
    WnbdReq.RequestHandle = RequestHandle;
    WnbdReq.Cmd.Unmap.Count = Count;

    handler->ReqLog.AddEntry(
        WnbdReq,
        (void*)Descriptors,
        sizeof(WNBD_UNMAP_DESCRIPTOR) * Count);

    // TODO: validate unmap descriptors

    handler->SendIoResponse(
        RequestHandle, RequestType,
        handler->MockStatus, NULL, 0);
}

void MockWnbdDaemon::SendIoResponse(
    UINT64 RequestHandle,
    WnbdRequestType RequestType,
    WNBD_STATUS& Status,
    PVOID DataBuffer,
    UINT32 DataBufferSize
) {
    ASSERT_TRUE(WNBD_DEFAULT_MAX_TRANSFER_LENGTH >= DataBufferSize)
        << "wnbd response too large";

    WNBD_IO_RESPONSE Resp = { 0 };
    Resp.RequestHandle = RequestHandle;
    Resp.RequestType = RequestType;
    Resp.Status = Status;

    int err = WnbdSendResponse(
        WnbdDisk,
        &Resp,
        DataBuffer,
        DataBufferSize);

    if (err && TerminateInProgress) {
        // Suppress errors that might occur because of pending disk removals.
        err = 0;
    }

    ASSERT_FALSE(err) << "unable to send wnbd response, error: "
                      << GetLastError();
}

PWNBD_DISK MockWnbdDaemon::GetDisk() {
    return WnbdDisk;
}

void MockWnbdDaemon::TerminatingInProgress() {
    TerminateInProgress = true;
}
