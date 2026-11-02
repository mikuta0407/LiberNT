/*
 * PROJECT:     LiberNT Boot Loader
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     File access on a block device
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <bl.h>
#include "ntfs.h"

static DK_DEVICE FsMountedDevice;
static PNTFS_VOLUME_INFO FsMountedVolume;
static BOOLEAN FsMountedExFat;

PVOID (*FsMemoryReader)(PCSTR Path, PULONG Size);

static
BOOLEAN
FsMount(
    _In_ PDK_DEVICE Device)
{
    if ((!FsMountedVolume && !FsMountedExFat) ||
        FsMountedDevice.Firmware != Device->Firmware ||
        FsMountedDevice.Offset != Device->Offset)
    {
        FsMountedVolume = NtfsMount(Device);
        FsMountedExFat = !FsMountedVolume && ExFatMount(Device);
        if (!FsMountedVolume && !FsMountedExFat)
            return FALSE;

        FsMountedDevice = *Device;
    }

    return TRUE;
}

PVOID
FsReadFileA(
    _In_ PDK_DEVICE Device,
    _In_ PCSTR Path,
    _Out_ PULONG Size)
{
    *Size = 0;

    if (Device->Memory && FsMemoryReader)
        return FsMemoryReader(Path, Size);

    if (!FsMount(Device))
        return NULL;

    return FsMountedVolume ? NtfsReadFile(FsMountedVolume, Path, Size) : ExFatReadFile(Path, Size);
}

PVOID
FsOpenFileA(
    _In_ PDK_DEVICE Device,
    _In_ PCSTR Path,
    _Out_ PULONGLONG Size)
{
    *Size = 0;

    if ((Device->Memory && FsMemoryReader) || !FsMount(Device) || !FsMountedVolume)
        return NULL;

    return NtfsOpenFile(FsMountedVolume, Path, Size);
}

BOOLEAN
FsReadFileAt(
    _In_ PVOID File,
    _In_ ULONGLONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    return NtfsReadFileAt(File, Offset, Buffer, Length);
}

VOID
FsCloseFile(
    _In_ PVOID File)
{
    NtfsCloseFile(File);
}

static
BOOLEAN
FsNarrowPath(
    _In_ PCWSTR Path,
    _Out_writes_z_(BM_PATH_CHARS) PCHAR AnsiPath)
{
    ULONG i;

    for (i = 0; Path[i] != UNICODE_NULL; i++)
    {
        if (i >= BM_PATH_CHARS - 1 || Path[i] > 0x7F)
            return FALSE;
        AnsiPath[i] = (CHAR)Path[i];
    }
    AnsiPath[i] = ANSI_NULL;

    return TRUE;
}

PVOID
FsReadFile(
    _In_ PDK_DEVICE Device,
    _In_ PCWSTR Path,
    _Out_ PULONG Size)
{
    CHAR AnsiPath[BM_PATH_CHARS];

    *Size = 0;

    if (!FsNarrowPath(Path, AnsiPath))
        return NULL;

    return FsReadFileA(Device, AnsiPath, Size);
}

PVOID
FsOpenFile(
    _In_ PDK_DEVICE Device,
    _In_ PCWSTR Path,
    _Out_ PULONGLONG Size)
{
    CHAR AnsiPath[BM_PATH_CHARS];

    *Size = 0;

    if (!FsNarrowPath(Path, AnsiPath))
        return NULL;

    return FsOpenFileA(Device, AnsiPath, Size);
}

BOOLEAN
FsPathExists(
    _In_ PDK_DEVICE Device,
    _In_ PCWSTR Path)
{
    CHAR AnsiPath[BM_PATH_CHARS];

    if (!FsNarrowPath(Path, AnsiPath) || (Device->Memory && FsMemoryReader) || !FsMount(Device))
        return FALSE;

    return FsMountedVolume ? NtfsPathExists(FsMountedVolume, AnsiPath) : ExFatPathExists(AnsiPath);
}

BOOLEAN
FsHasDataStream(
    _In_ PDK_DEVICE Device,
    _In_ PCWSTR Path,
    _In_ PCWSTR Stream)
{
    CHAR AnsiPath[BM_PATH_CHARS];

    return FsNarrowPath(Path, AnsiPath) &&
           !(Device->Memory && FsMemoryReader) &&
           FsMount(Device) &&
           FsMountedVolume &&
           NtfsHasDataStream(FsMountedVolume, AnsiPath, Stream);
}

BOOLEAN
FsVolumeIsDirty(
    _In_ PDK_DEVICE Device)
{
    return !(Device->Memory && FsMemoryReader) &&
           FsMount(Device) &&
           FsMountedVolume &&
           NtfsVolumeIsDirty(FsMountedVolume);
}

PCWSTR
FsGetDriverName(
    _In_ PDK_DEVICE Device)
{
    if (Device->Memory && FsMemoryReader)
        return L"fastfat";

    if (FsMountedExFat)
        return ExFatIsExFat() ? L"exfat" : L"fastfat";

    return L"ntfs";
}
