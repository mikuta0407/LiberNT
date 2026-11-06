/*
 * PROJECT:     LiberNT VHD Miniport
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Virtual disk driver for booting from a VHDX file
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#pragma once

#include <ntifs.h>
#include <ntstrsafe.h>
#include <ntpoapi.h>
#include <ntddscsi.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <scsi.h>
#include <vhdx.h>

#define VHD_TAG 'mdhV'
#define VHD_LUN 1

#define VIRTUAL_STORAGE_TYPE_DEVICE_VHDX        3
#define DEPENDENT_DISK_FLAG_SYSTEM_VOLUME       0x00000010
#define DEPENDENT_DISK_FLAG_REMOVABLE           0x00000040
#define DEPENDENT_DISK_FLAG_PERMANENT_LIFETIME  0x00000400

DEFINE_GUID(VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT, 0xec984aec, 0xa0f9, 0x47e9, 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b);

#define VHD_VENDOR   "Msft    "
#define VHD_PRODUCT  "Virtual Disk    "
#define VHD_REVISION "1.0 "

typedef struct _VHD_EXTENT
{
    ULONGLONG Vcn;
    ULONGLONG NextVcn;
    LONGLONG Lcn;
} VHD_EXTENT, *PVHD_EXTENT;

typedef struct _VHD_BUFFER
{
    PUCHAR System;
    PMDL Mdl;
    PUCHAR MdlVa;
} VHD_BUFFER, *PVHD_BUFFER;

typedef struct _VHD_DISK
{
    VHDX_IMAGE Image;
    HANDLE File;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT Target;
    ULONG ClusterSize;
    ULONG SectorSize;
    PVHD_EXTENT Extents;
    ULONG ExtentCount;
    ULONGLONG FileSize;
    ULONGLONG NextFree;
    PUCHAR Bounce;
    ULONG BounceSize;
    PUCHAR Zero;
    UNICODE_STRING HostVolume;
    UNICODE_STRING RelativePath;
} VHD_DISK, *PVHD_DISK;

typedef struct _VHD_EXTENSION
{
    BOOLEAN IsFdo;
    PDEVICE_OBJECT Self;
} VHD_EXTENSION, *PVHD_EXTENSION;

typedef struct _VHD_FDO_EXTENSION
{
    VHD_EXTENSION Common;
    PDEVICE_OBJECT Pdo;
    PDEVICE_OBJECT Lower;
    PDEVICE_OBJECT Child;
    UNICODE_STRING HostPath;
} VHD_FDO_EXTENSION, *PVHD_FDO_EXTENSION;

typedef struct _VHD_PDO_EXTENSION
{
    VHD_EXTENSION Common;
    PDEVICE_OBJECT Fdo;
    VHD_DISK Disk;
    LIST_ENTRY Queue;
    KSPIN_LOCK QueueLock;
    KEVENT QueueEvent;
    PKTHREAD Worker;
    PVOID SleepToken;
    BOOLEAN Present;
} VHD_PDO_EXTENSION, *PVHD_PDO_EXTENSION;

NTSTATUS
VhdDeviceIoControl(
    _In_ PDEVICE_OBJECT Device,
    _In_ ULONG IoControlCode,
    _Out_writes_bytes_(Length) PVOID Output,
    _In_ ULONG Length);

NTSTATUS
VhdOpenDisk(
    _Out_ PVHD_DISK Disk,
    _In_ PCUNICODE_STRING Path);

NTSTATUS
VhdReadWrite(
    _Inout_ PVHD_DISK Disk,
    _In_ BOOLEAN Write,
    _In_ ULONGLONG Offset,
    _In_ VHD_BUFFER Buffer,
    _In_ ULONG Length);

NTSTATUS
VhdFlush(
    _In_ PVHD_DISK Disk);

NTSTATUS
VhdExecuteSrb(
    _In_ PVHD_PDO_EXTENSION Extension,
    _Inout_ PIRP Irp);

NTSTATUS
VhdPdoDeviceControl(
    _In_ PVHD_PDO_EXTENSION Extension,
    _Inout_ PIRP Irp);
