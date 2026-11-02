/*
 * PROJECT:     LiberNT VHDX Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     VHDX image parsing and block translation ([MS-VHDX])
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#pragma once

#define VHDX_HEADER1_OFFSET      0x10000ULL
#define VHDX_HEADER2_OFFSET      0x20000ULL
#define VHDX_HEADER_SIZE         0x1000
#define VHDX_REGION_TABLE_SIZE   0x10000
#define VHDX_MB                  0x100000ULL

#define VHDX_BAT_STATE_MASK      7
#define VHDX_BAT_NOT_PRESENT     0
#define VHDX_BAT_UNDEFINED       1
#define VHDX_BAT_ZERO            2
#define VHDX_BAT_UNMAPPED        3
#define VHDX_BAT_FULLY_PRESENT   6
#define VHDX_BAT_PARTIALLY_PRESENT 7
#define VHDX_BAT_OFFSET_SHIFT    20

typedef BOOLEAN (*PVHDX_READ)(PVOID Context, ULONGLONG Offset, PVOID Buffer, ULONG Length);
typedef PVOID (*PVHDX_ALLOCATE)(SIZE_T Size);
typedef VOID (*PVHDX_FREE)(PVOID Buffer);

typedef struct _VHDX_IMAGE
{
    ULONGLONG SequenceNumber;
    ULONGLONG HeaderOffset;
    UCHAR FileWriteGuid[16];
    UCHAR DataWriteGuid[16];
    UCHAR LogGuid[16];
    ULONGLONG LogOffset;
    ULONG LogLength;
    ULONGLONG BatOffset;
    ULONG BatLength;
    ULONGLONG VirtualSize;
    ULONG BlockSize;
    ULONG LogicalSectorSize;
    ULONG PhysicalSectorSize;
    ULONG ChunkRatio;
    ULONG PayloadBlocks;
    ULONG BatEntries;
    UCHAR DiskId[16];
    BOOLEAN LeaveBlocksAllocated;
    BOOLEAN HasParent;
    PULONGLONG Bat;
} VHDX_IMAGE, *PVHDX_IMAGE;

ULONG
VhdxCrc32c(
    _In_ ULONG Crc,
    _In_reads_bytes_(Length) const VOID* Buffer,
    _In_ SIZE_T Length);

NTSTATUS
VhdxOpen(
    _Out_ PVHDX_IMAGE Image,
    _In_ PVHDX_READ Read,
    _In_ PVOID Context,
    _In_ ULONGLONG FileSize,
    _In_ PVHDX_ALLOCATE Allocate,
    _In_ PVHDX_FREE Free);

VOID
VhdxClose(
    _Inout_ PVHDX_IMAGE Image,
    _In_ PVHDX_FREE Free);

VOID
VhdxBuildHeader(
    _In_ PVHDX_IMAGE Image,
    _Out_writes_bytes_all_(VHDX_HEADER_SIZE) PUCHAR Header);

BOOLEAN
VhdxLogIsEmpty(
    _In_ PVHDX_IMAGE Image);

ULONG
VhdxBatIndex(
    _In_ PVHDX_IMAGE Image,
    _In_ ULONG Block);

ULONG
VhdxMap(
    _In_ PVHDX_IMAGE Image,
    _In_ ULONGLONG Offset,
    _In_ ULONG Length,
    _Out_ PULONGLONG FileOffset);
