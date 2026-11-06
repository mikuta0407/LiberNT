/*
 * PROJECT:     LiberNT Boot Loader
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Block device access
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

typedef struct _DK_VIRTUAL_DISK *PDK_VIRTUAL_DISK;

typedef struct _DK_DEVICE
{
    PVOID Firmware;
    PDK_VIRTUAL_DISK Virtual;
    PUCHAR Memory;
    ULONGLONG Offset;
    ULONGLONG Length;
    ULONG BlockSize;
    ULONG DiskIndex;
    ULONG PartitionNumber;
} DK_DEVICE, *PDK_DEVICE;

BOOLEAN
DkRead(
    _In_ PDK_DEVICE Device,
    _In_ ULONGLONG Offset,
    _In_ ULONG Length,
    _Out_writes_bytes_(Length) PVOID Buffer);

BOOLEAN
DkWrite(
    _In_ PDK_DEVICE Device,
    _In_ ULONGLONG Offset,
    _In_ ULONG Length,
    _In_reads_bytes_(Length) const VOID* Buffer);

BOOLEAN
DkFlush(
    _In_ PDK_DEVICE Device);

BOOLEAN
DkOpenBcdDevice(
    _In_ PBCD_DEVICE BcdDevice,
    _Out_ PDK_DEVICE Device);

BOOLEAN
GptFindPartition(
    _In_ PDK_DEVICE Disk,
    _In_reads_bytes_opt_(16) const UCHAR* DiskId,
    _In_reads_bytes_(16) const UCHAR* PartitionId,
    _Out_ PDK_DEVICE Partition);

PVOID
FsReadFile(
    _In_ PDK_DEVICE Device,
    _In_ PCWSTR Path,
    _Out_ PULONG Size);

PVOID
FsReadFileA(
    _In_ PDK_DEVICE Device,
    _In_ PCSTR Path,
    _Out_ PULONG Size);

PVOID
FsOpenFileA(
    _In_ PDK_DEVICE Device,
    _In_ PCSTR Path,
    _Out_ PULONGLONG Size);

BOOLEAN
FsReadFileAt(
    _In_ PVOID File,
    _In_ ULONGLONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length);

VOID
FsCloseFile(
    _In_ PVOID File);

BOOLEAN
GptGetPartition(
    _In_ PDK_DEVICE Disk,
    _In_ ULONG Number,
    _Out_ PDK_DEVICE Partition);

BOOLEAN
DkGetPartition(
    _In_ PDK_DEVICE Disk,
    _In_ ULONG Number,
    _Out_ PDK_DEVICE Partition);

BOOLEAN
DkLocatePartition(
    _In_ PDK_DEVICE Disk,
    _In_ PCWSTR Path,
    _Out_ PDK_DEVICE Partition);

BOOLEAN
DkOpenVirtualDisk(
    _In_ PBCD_DEVICE BcdDevice,
    _Out_ PDK_DEVICE Device);

VOID
VdGetDisk(
    _In_ PDK_DEVICE Partition,
    _Out_ PDK_DEVICE Disk);

BOOLEAN
VdAppendBootOption(
    _In_ PDK_DEVICE Partition,
    _Inout_updates_z_(Size) PSTR Options,
    _In_ SIZE_T Size);

BOOLEAN
VdReadBlocks(
    _In_ PDK_VIRTUAL_DISK Virtual,
    _In_ ULONGLONG Block,
    _In_ ULONG Count,
    _Out_ PVOID Buffer);

BOOLEAN
FsPathExists(
    _In_ PDK_DEVICE Device,
    _In_ PCWSTR Path);

BOOLEAN
FsHasDataStream(
    _In_ PDK_DEVICE Device,
    _In_ PCWSTR Path,
    _In_ PCWSTR Stream);

BOOLEAN
FsVolumeIsDirty(
    _In_ PDK_DEVICE Device);

PVOID
FsOpenFile(
    _In_ PDK_DEVICE Device,
    _In_ PCWSTR Path,
    _Out_ PULONGLONG Size);

BOOLEAN
GptGetDiskId(
    _In_ PDK_DEVICE Disk,
    _Out_writes_bytes_(16) PUCHAR DiskId);

BOOLEAN
DkLocateBootPartition(
    _Out_ PDK_DEVICE Partition);

PCWSTR
FsGetDriverName(
    _In_ PDK_DEVICE Device);

extern PVOID (*FsMemoryReader)(PCSTR Path, PULONG Size);

BOOLEAN
ExFatMount(
    _In_ PDK_DEVICE Device);

BOOLEAN
ExFatIsExFat(VOID);

PVOID
ExFatReadFile(
    _In_ PCSTR Path,
    _Out_ PULONG Size);

BOOLEAN
ExFatPathExists(
    _In_ PCSTR Path);
