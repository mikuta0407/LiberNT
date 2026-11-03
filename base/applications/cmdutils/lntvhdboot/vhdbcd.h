/*
 * PROJECT:     LiberNT VHD boot setup tool
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     BCD device elements of a partition inside a VHDX file
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#pragma once

#define VHD_BCD_APPLICATION_PATH 0x12000002
#define VHD_BCD_SYSTEM_ROOT      0x22000002

typedef struct _VHD_BCD_HOST
{
    BOOL IsGpt;
    DWORD Signature;
    ULONGLONG Offset;
    GUID DiskId;
    GUID PartitionId;
} VHD_BCD_HOST, *PVHD_BCD_HOST;

DWORD
VhdBuildBcdDevice(
    _In_ const VHD_BCD_HOST* Host,
    _In_ PCWSTR Path,
    _In_ DWORD LocateElement,
    _Out_writes_bytes_(Size) PBYTE Buffer,
    _In_ DWORD Size);
