/*
 * PROJECT:     LiberNT VHD boot setup tool
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     BCD device elements of a partition inside a VHDX file
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include <windows.h>
#include <string.h>

#include "vhdbcd.h"

#define BCD_ELEMENT_DEVICE_OFFSET 0x10
#define BCD_HEADER_LENGTH         0x10
#define BCD_PARTITION_LENGTH      0x48
#define BCD_DEVICE_DISK           0
#define BCD_DEVICE_PARTITION      6
#define BCD_DEVICE_LOCATE         8
#define BCD_DISK_FILE             5
#define BCD_DISK_VIRTUAL          6
#define BCD_FILE_VERSION          1
#define BCD_FILE_PATH             5
#define BCD_PARTITION_GPT         0
#define BCD_PARTITION_MBR         1
#define BCD_LOCATE_ELEMENT        0
#define BCD_VIRTUAL_RESERVED      20

static
VOID
StoreUlong(
    _Out_writes_bytes_all_(4) PBYTE Data,
    _In_ DWORD Value)
{
    Data[0] = (BYTE)Value;
    Data[1] = (BYTE)(Value >> 8);
    Data[2] = (BYTE)(Value >> 16);
    Data[3] = (BYTE)(Value >> 24);
}

static
VOID
StoreHeader(
    _Out_writes_bytes_all_(BCD_HEADER_LENGTH) PBYTE Data,
    _In_ DWORD Type,
    _In_ DWORD Length)
{
    memset(Data, 0, BCD_HEADER_LENGTH);
    StoreUlong(Data, Type);
    StoreUlong(Data + 8, Length);
}

DWORD
VhdBuildBcdDevice(
    _In_ const VHD_BCD_HOST* Host,
    _In_ PCWSTR Path,
    _In_ DWORD LocateElement,
    _Out_writes_bytes_(Size) PBYTE Buffer,
    _In_ DWORD Size)
{
    DWORD PathBytes = (DWORD)(wcslen(Path) + 1) * sizeof(WCHAR);
    DWORD FileLength = 0x0C + BCD_PARTITION_LENGTH + PathBytes;
    DWORD FileDevice = 0x14 + FileLength;
    DWORD VirtualDevice = 0x28 + FileDevice;
    DWORD Locate = 0x1E + VirtualDevice;
    DWORD Total = BCD_ELEMENT_DEVICE_OFFSET + Locate;
    PBYTE Data;

    if (Size < Total)
        return 0;

    memset(Buffer, 0, Total);
    Data = Buffer + BCD_ELEMENT_DEVICE_OFFSET;

    StoreHeader(Data, BCD_DEVICE_LOCATE, Locate);
    StoreUlong(Data + 0x10, BCD_LOCATE_ELEMENT);
    StoreUlong(Data + 0x14, LocateElement);
    StoreUlong(Data + 0x18, 0x1E);
    Data += 0x1E;

    StoreHeader(Data, BCD_DEVICE_DISK, VirtualDevice);
    StoreUlong(Data + 0x10, BCD_DISK_VIRTUAL);
    Data += 0x14 + BCD_VIRTUAL_RESERVED;

    StoreHeader(Data, BCD_DEVICE_DISK, FileDevice);
    StoreUlong(Data + 0x10, BCD_DISK_FILE);
    StoreUlong(Data + 0x14, BCD_FILE_VERSION);
    StoreUlong(Data + 0x18, FileLength);
    StoreUlong(Data + 0x1C, BCD_FILE_PATH);
    Data += 0x20;

    StoreHeader(Data, BCD_DEVICE_PARTITION, BCD_PARTITION_LENGTH);
    if (Host->IsGpt)
    {
        memcpy(Data + 0x10, &Host->PartitionId, sizeof(GUID));
        StoreUlong(Data + 0x24, BCD_PARTITION_GPT);
        memcpy(Data + 0x28, &Host->DiskId, sizeof(GUID));
    }
    else
    {
        StoreUlong(Data + 0x10, (DWORD)Host->Offset);
        StoreUlong(Data + 0x14, (DWORD)(Host->Offset >> 32));
        StoreUlong(Data + 0x24, BCD_PARTITION_MBR);
        StoreUlong(Data + 0x28, Host->Signature);
    }
    Data += BCD_PARTITION_LENGTH;

    memcpy(Data, Path, PathBytes);
    return Total;
}
