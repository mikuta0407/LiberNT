/*
 * PROJECT:     LiberNT VHDX Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     VHDX image parsing and block translation ([MS-VHDX])
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include <ntddk.h>
#include <vhdx.h>

#define VHDX_FILE_SIGNATURE      0x656C696678646876ULL
#define VHDX_HEADER_SIGNATURE    0x64616568
#define VHDX_REGION_SIGNATURE    0x69676572
#define VHDX_METADATA_SIGNATURE  0x617461646174656DULL

#define VHDX_REGION_TABLE1_OFFSET 0x30000ULL
#define VHDX_REGION_TABLE2_OFFSET 0x40000ULL
#define VHDX_MAX_TABLE_ENTRIES   2047
#define VHDX_TABLE_ENTRY_SIZE    32
#define VHDX_TABLE_HEADER_SIZE   16
#define VHDX_METADATA_HEADER_SIZE 32
#define VHDX_METADATA_TABLE_SIZE 0x10000

#define VHDX_HEADER_CHECKSUM     4
#define VHDX_HEADER_SEQUENCE     8
#define VHDX_HEADER_FILE_WRITE   16
#define VHDX_HEADER_DATA_WRITE   32
#define VHDX_HEADER_LOG_GUID     48
#define VHDX_HEADER_LOG_VERSION  64
#define VHDX_HEADER_VERSION      66
#define VHDX_HEADER_LOG_LENGTH   68
#define VHDX_HEADER_LOG_OFFSET   72

#define VHDX_REGION_COUNT        8
#define VHDX_REGION_FILE_OFFSET  16
#define VHDX_REGION_LENGTH       24
#define VHDX_REGION_REQUIRED     28

#define VHDX_METADATA_COUNT      10
#define VHDX_METADATA_OFFSET     16
#define VHDX_METADATA_LENGTH     20
#define VHDX_METADATA_FLAGS      24
#define VHDX_METADATA_REQUIRED   4

#define VHDX_PARAMS_LEAVE_ALLOCATED 1
#define VHDX_PARAMS_HAS_PARENT      2

#define VHDX_MIN_BLOCK_SIZE      VHDX_MB
#define VHDX_MAX_BLOCK_SIZE      (256 * VHDX_MB)
#define VHDX_MAX_VIRTUAL_SIZE    (64ULL * 1024 * 1024 * VHDX_MB)
#define VHDX_SECTORS_PER_BITMAP  0x800000ULL

static const UCHAR VhdxBatRegion[16] =
    {0x66, 0x77, 0xC2, 0x2D, 0x23, 0xF6, 0x00, 0x42, 0x9D, 0x64, 0x11, 0x5E, 0x9B, 0xFD, 0x4A, 0x08};
static const UCHAR VhdxMetadataRegion[16] =
    {0x06, 0xA2, 0x7C, 0x8B, 0x90, 0x47, 0x9A, 0x4B, 0xB8, 0xFE, 0x57, 0x5F, 0x05, 0x0F, 0x88, 0x6E};
static const UCHAR VhdxFileParameters[16] =
    {0x37, 0x67, 0xA1, 0xCA, 0x36, 0xFA, 0x43, 0x4D, 0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B};
static const UCHAR VhdxVirtualDiskSize[16] =
    {0x24, 0x42, 0xA5, 0x2F, 0x1B, 0xCD, 0x76, 0x48, 0xB2, 0x11, 0x5D, 0xBE, 0xD8, 0x3B, 0xF4, 0xB8};
static const UCHAR VhdxVirtualDiskId[16] =
    {0xAB, 0x12, 0xCA, 0xBE, 0xE6, 0xB2, 0x23, 0x45, 0x93, 0xEF, 0xC3, 0x09, 0xE0, 0x00, 0xC7, 0x46};
static const UCHAR VhdxLogicalSectorSize[16] =
    {0x1D, 0xBF, 0x41, 0x81, 0x6F, 0xA9, 0x09, 0x47, 0xBA, 0x47, 0xF2, 0x33, 0xA8, 0xFA, 0xAB, 0x5F};
static const UCHAR VhdxPhysicalSectorSize[16] =
    {0xC7, 0x48, 0xA3, 0xCD, 0x5D, 0x44, 0x71, 0x44, 0x9C, 0xC9, 0xE9, 0x88, 0x52, 0x51, 0xC5, 0x56};
static const UCHAR VhdxParentLocator[16] =
    {0x2D, 0x5F, 0xD3, 0xA8, 0x0B, 0xB3, 0x4D, 0x45, 0xAB, 0xF7, 0xD3, 0xD8, 0x48, 0x34, 0xAB, 0x0C};

static
ULONG
VhdxUlong(
    _In_reads_bytes_(4) const UCHAR* Data)
{
    return Data[0] | (Data[1] << 8) | (Data[2] << 16) | ((ULONG)Data[3] << 24);
}

static
ULONGLONG
VhdxUlonglong(
    _In_reads_bytes_(8) const UCHAR* Data)
{
    return VhdxUlong(Data) | ((ULONGLONG)VhdxUlong(Data + 4) << 32);
}

static
BOOLEAN
VhdxEqualGuid(
    _In_reads_bytes_(16) const UCHAR* Left,
    _In_reads_bytes_(16) const UCHAR* Right)
{
    return RtlCompareMemory(Left, Right, 16) == 16;
}

ULONG
VhdxCrc32c(
    _In_ ULONG Crc,
    _In_reads_bytes_(Length) const VOID* Buffer,
    _In_ SIZE_T Length)
{
    const UCHAR* Data = Buffer;
    ULONG Bit;

    Crc = ~Crc;
    while (Length-- != 0)
    {
        Crc ^= *Data++;
        for (Bit = 0; Bit < 8; Bit++)
            Crc = (Crc >> 1) ^ (0x82F63B78 & (0 - (Crc & 1)));
    }

    return ~Crc;
}

static
BOOLEAN
VhdxChecksumValid(
    _Inout_updates_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    ULONG Checksum = VhdxUlong(Buffer + VHDX_HEADER_CHECKSUM);

    RtlZeroMemory(Buffer + VHDX_HEADER_CHECKSUM, sizeof(ULONG));
    return VhdxCrc32c(0, Buffer, Length) == Checksum;
}

static
BOOLEAN
VhdxIsPowerOfTwo(
    _In_ ULONGLONG Value)
{
    return Value != 0 && (Value & (Value - 1)) == 0;
}

static
NTSTATUS
VhdxReadHeaders(
    _Inout_ PVHDX_IMAGE Image,
    _In_ PVHDX_READ Read,
    _In_ PVOID Context,
    _Out_writes_bytes_(VHDX_HEADER_SIZE) PUCHAR Buffer)
{
    static const ULONGLONG Offsets[] = {VHDX_HEADER1_OFFSET, VHDX_HEADER2_OFFSET};
    NTSTATUS Status = STATUS_VHD_FORMAT_UNKNOWN;
    ULONGLONG Sequence;
    ULONG i;
    BOOLEAN Found = FALSE;

    for (i = 0; i < RTL_NUMBER_OF(Offsets); i++)
    {
        if (!Read(Context, Offsets[i], Buffer, VHDX_HEADER_SIZE) ||
            VhdxUlong(Buffer) != VHDX_HEADER_SIGNATURE ||
            !VhdxChecksumValid(Buffer, VHDX_HEADER_SIZE))
        {
            continue;
        }

        if (*(PUSHORT)(Buffer + VHDX_HEADER_VERSION) != 1 || *(PUSHORT)(Buffer + VHDX_HEADER_LOG_VERSION) != 0)
        {
            Status = STATUS_VHD_FORMAT_UNSUPPORTED_VERSION;
            continue;
        }

        Sequence = VhdxUlonglong(Buffer + VHDX_HEADER_SEQUENCE);
        if (Found && Sequence <= Image->SequenceNumber)
            continue;

        Found = TRUE;
        Image->SequenceNumber = Sequence;
        Image->HeaderOffset = Offsets[i];
        RtlCopyMemory(Image->FileWriteGuid, Buffer + VHDX_HEADER_FILE_WRITE, 16);
        RtlCopyMemory(Image->DataWriteGuid, Buffer + VHDX_HEADER_DATA_WRITE, 16);
        RtlCopyMemory(Image->LogGuid, Buffer + VHDX_HEADER_LOG_GUID, 16);
        Image->LogLength = VhdxUlong(Buffer + VHDX_HEADER_LOG_LENGTH);
        Image->LogOffset = VhdxUlonglong(Buffer + VHDX_HEADER_LOG_OFFSET);
    }

    return Found ? STATUS_SUCCESS : Status;
}

static
NTSTATUS
VhdxReadRegions(
    _Inout_ PVHDX_IMAGE Image,
    _In_ PVHDX_READ Read,
    _In_ PVOID Context,
    _In_ ULONGLONG FileSize,
    _Out_ PULONGLONG MetadataOffset,
    _Out_ PULONG MetadataLength,
    _Out_writes_bytes_(VHDX_REGION_TABLE_SIZE) PUCHAR Buffer)
{
    static const ULONGLONG Offsets[] = {VHDX_REGION_TABLE1_OFFSET, VHDX_REGION_TABLE2_OFFSET};
    const UCHAR* Entry;
    ULONGLONG Offset;
    ULONG Count, Length, i, j;

    for (i = 0; i < RTL_NUMBER_OF(Offsets); i++)
    {
        if (!Read(Context, Offsets[i], Buffer, VHDX_REGION_TABLE_SIZE) ||
            VhdxUlong(Buffer) != VHDX_REGION_SIGNATURE ||
            !VhdxChecksumValid(Buffer, VHDX_REGION_TABLE_SIZE))
        {
            continue;
        }

        Count = VhdxUlong(Buffer + VHDX_REGION_COUNT);
        if (Count > VHDX_MAX_TABLE_ENTRIES)
            continue;

        Image->BatOffset = 0;
        *MetadataOffset = 0;
        for (j = 0; j < Count; j++)
        {
            Entry = Buffer + VHDX_TABLE_HEADER_SIZE + j * VHDX_TABLE_ENTRY_SIZE;
            Offset = VhdxUlonglong(Entry + VHDX_REGION_FILE_OFFSET);
            Length = VhdxUlong(Entry + VHDX_REGION_LENGTH);

            if (Offset % VHDX_MB != 0 || Length % VHDX_MB != 0 || Offset > FileSize || Length > FileSize - Offset)
                return STATUS_FILE_CORRUPT_ERROR;

            if (VhdxEqualGuid(Entry, VhdxBatRegion))
            {
                Image->BatOffset = Offset;
                Image->BatLength = Length;
            }
            else if (VhdxEqualGuid(Entry, VhdxMetadataRegion))
            {
                *MetadataOffset = Offset;
                *MetadataLength = Length;
            }
            else if (VhdxUlong(Entry + VHDX_REGION_REQUIRED) & 1)
            {
                return STATUS_VHD_FORMAT_UNKNOWN;
            }
        }

        if (Image->BatOffset == 0 || *MetadataOffset == 0)
            return STATUS_FILE_CORRUPT_ERROR;

        return STATUS_SUCCESS;
    }

    return STATUS_FILE_CORRUPT_ERROR;
}

static
BOOLEAN
VhdxReadItem(
    _In_ PVHDX_READ Read,
    _In_ PVOID Context,
    _In_ ULONGLONG MetadataOffset,
    _In_ ULONG MetadataLength,
    _In_reads_bytes_(VHDX_TABLE_ENTRY_SIZE) const UCHAR* Entry,
    _In_ ULONG Length,
    _Out_writes_bytes_(Length) PVOID Value)
{
    ULONG Offset = VhdxUlong(Entry + VHDX_METADATA_OFFSET);

    return VhdxUlong(Entry + VHDX_METADATA_LENGTH) == Length &&
           Offset >= VHDX_METADATA_TABLE_SIZE &&
           Offset <= MetadataLength &&
           Length <= MetadataLength - Offset &&
           Read(Context, MetadataOffset + Offset, Value, Length);
}

static
NTSTATUS
VhdxReadMetadata(
    _Inout_ PVHDX_IMAGE Image,
    _In_ PVHDX_READ Read,
    _In_ PVOID Context,
    _In_ ULONGLONG MetadataOffset,
    _In_ ULONG MetadataLength,
    _Out_writes_bytes_(VHDX_METADATA_TABLE_SIZE) PUCHAR Buffer)
{
    const UCHAR* Entry;
    UCHAR Value[16];
    ULONG Count, Found = 0, i;
    BOOLEAN Ok;

    if (MetadataLength < VHDX_METADATA_TABLE_SIZE ||
        !Read(Context, MetadataOffset, Buffer, VHDX_METADATA_TABLE_SIZE) ||
        VhdxUlonglong(Buffer) != VHDX_METADATA_SIGNATURE)
    {
        return STATUS_VHD_METADATA_READ_FAILURE;
    }

    Count = *(PUSHORT)(Buffer + VHDX_METADATA_COUNT);
    if (Count > VHDX_MAX_TABLE_ENTRIES)
        return STATUS_VHD_METADATA_READ_FAILURE;

    for (i = 0; i < Count; i++)
    {
        Entry = Buffer + VHDX_METADATA_HEADER_SIZE + i * VHDX_TABLE_ENTRY_SIZE;
        Ok = TRUE;

        if (VhdxEqualGuid(Entry, VhdxFileParameters))
        {
            Ok = VhdxReadItem(Read, Context, MetadataOffset, MetadataLength, Entry, 8, Value);
            Image->BlockSize = VhdxUlong(Value);
            Image->LeaveBlocksAllocated = (VhdxUlong(Value + 4) & VHDX_PARAMS_LEAVE_ALLOCATED) != 0;
            Image->HasParent = (VhdxUlong(Value + 4) & VHDX_PARAMS_HAS_PARENT) != 0;
            Found |= 1;
        }
        else if (VhdxEqualGuid(Entry, VhdxVirtualDiskSize))
        {
            Ok = VhdxReadItem(Read, Context, MetadataOffset, MetadataLength, Entry, 8, Value);
            Image->VirtualSize = VhdxUlonglong(Value);
            Found |= 2;
        }
        else if (VhdxEqualGuid(Entry, VhdxVirtualDiskId))
        {
            Ok = VhdxReadItem(Read, Context, MetadataOffset, MetadataLength, Entry, 16, Image->DiskId);
            Found |= 4;
        }
        else if (VhdxEqualGuid(Entry, VhdxLogicalSectorSize))
        {
            Ok = VhdxReadItem(Read, Context, MetadataOffset, MetadataLength, Entry, 4, Value);
            Image->LogicalSectorSize = VhdxUlong(Value);
            Found |= 8;
        }
        else if (VhdxEqualGuid(Entry, VhdxPhysicalSectorSize))
        {
            Ok = VhdxReadItem(Read, Context, MetadataOffset, MetadataLength, Entry, 4, Value);
            Image->PhysicalSectorSize = VhdxUlong(Value);
            Found |= 16;
        }
        else if (!VhdxEqualGuid(Entry, VhdxParentLocator) &&
                 (VhdxUlong(Entry + VHDX_METADATA_FLAGS) & VHDX_METADATA_REQUIRED))
        {
            return STATUS_VHD_FORMAT_UNKNOWN;
        }

        if (!Ok)
            return STATUS_VHD_METADATA_READ_FAILURE;
    }

    if (Found != 31)
        return STATUS_VHD_METADATA_READ_FAILURE;

    if (Image->BlockSize < VHDX_MIN_BLOCK_SIZE ||
        Image->BlockSize > VHDX_MAX_BLOCK_SIZE ||
        !VhdxIsPowerOfTwo(Image->BlockSize))
    {
        return STATUS_VHD_INVALID_BLOCK_SIZE;
    }

    if ((Image->LogicalSectorSize != 512 && Image->LogicalSectorSize != 4096) ||
        (Image->PhysicalSectorSize != 512 && Image->PhysicalSectorSize != 4096))
    {
        return STATUS_VIRTDISK_UNSUPPORTED_DISK_SECTOR_SIZE;
    }

    if (Image->VirtualSize == 0 ||
        Image->VirtualSize > VHDX_MAX_VIRTUAL_SIZE ||
        Image->VirtualSize % Image->LogicalSectorSize != 0)
    {
        return STATUS_VHD_INVALID_SIZE;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
VhdxOpen(
    _Out_ PVHDX_IMAGE Image,
    _In_ PVHDX_READ Read,
    _In_ PVOID Context,
    _In_ ULONGLONG FileSize,
    _In_ PVHDX_ALLOCATE Allocate,
    _In_ PVHDX_FREE Free)
{
    ULONGLONG MetadataOffset, Signature;
    ULONG MetadataLength;
    PUCHAR Buffer;
    NTSTATUS Status;

    RtlZeroMemory(Image, sizeof(*Image));

    if (FileSize < VHDX_REGION_TABLE2_OFFSET + VHDX_REGION_TABLE_SIZE ||
        !Read(Context, 0, &Signature, sizeof(Signature)) ||
        Signature != VHDX_FILE_SIGNATURE)
    {
        return STATUS_VHD_FORMAT_UNKNOWN;
    }

    Buffer = Allocate(VHDX_REGION_TABLE_SIZE);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = VhdxReadHeaders(Image, Read, Context, Buffer);
    if (NT_SUCCESS(Status))
        Status = VhdxReadRegions(Image, Read, Context, FileSize, &MetadataOffset, &MetadataLength, Buffer);
    if (NT_SUCCESS(Status))
        Status = VhdxReadMetadata(Image, Read, Context, MetadataOffset, MetadataLength, Buffer);
    Free(Buffer);

    if (!NT_SUCCESS(Status))
        return Status;

    /* TODO: differencing disks, which need the parent chain and the sector bitmaps */
    if (Image->HasParent)
        return STATUS_NOT_SUPPORTED;

    Image->ChunkRatio = (ULONG)(VHDX_SECTORS_PER_BITMAP * Image->LogicalSectorSize / Image->BlockSize);
    Image->PayloadBlocks = (ULONG)((Image->VirtualSize + Image->BlockSize - 1) / Image->BlockSize);
    Image->BatEntries = Image->PayloadBlocks + (Image->PayloadBlocks - 1) / Image->ChunkRatio;
    if ((ULONGLONG)Image->BatEntries * sizeof(ULONGLONG) > Image->BatLength)
        return STATUS_VHD_BLOCK_ALLOCATION_TABLE_CORRUPT;

    Image->Bat = Allocate((SIZE_T)Image->BatEntries * sizeof(ULONGLONG));
    if (!Image->Bat)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (!Read(Context, Image->BatOffset, Image->Bat, Image->BatEntries * sizeof(ULONGLONG)))
    {
        VhdxClose(Image, Free);
        return STATUS_VHD_BLOCK_ALLOCATION_TABLE_CORRUPT;
    }

    return STATUS_SUCCESS;
}

VOID
VhdxClose(
    _Inout_ PVHDX_IMAGE Image,
    _In_ PVHDX_FREE Free)
{
    if (Image->Bat)
        Free(Image->Bat);
    Image->Bat = NULL;
}

VOID
VhdxBuildHeader(
    _In_ PVHDX_IMAGE Image,
    _Out_writes_bytes_all_(VHDX_HEADER_SIZE) PUCHAR Header)
{
    ULONG Checksum;

    RtlZeroMemory(Header, VHDX_HEADER_SIZE);
    *(PULONG)Header = VHDX_HEADER_SIGNATURE;
    *(PULONGLONG)(Header + VHDX_HEADER_SEQUENCE) = Image->SequenceNumber;
    RtlCopyMemory(Header + VHDX_HEADER_FILE_WRITE, Image->FileWriteGuid, 16);
    RtlCopyMemory(Header + VHDX_HEADER_DATA_WRITE, Image->DataWriteGuid, 16);
    RtlCopyMemory(Header + VHDX_HEADER_LOG_GUID, Image->LogGuid, 16);
    *(PUSHORT)(Header + VHDX_HEADER_VERSION) = 1;
    *(PULONG)(Header + VHDX_HEADER_LOG_LENGTH) = Image->LogLength;
    *(PULONGLONG)(Header + VHDX_HEADER_LOG_OFFSET) = Image->LogOffset;

    Checksum = VhdxCrc32c(0, Header, VHDX_HEADER_SIZE);
    *(PULONG)(Header + VHDX_HEADER_CHECKSUM) = Checksum;
}

BOOLEAN
VhdxLogIsEmpty(
    _In_ PVHDX_IMAGE Image)
{
    static const UCHAR Null[16] = {0};

    return VhdxEqualGuid(Image->LogGuid, Null);
}

ULONG
VhdxBatIndex(
    _In_ PVHDX_IMAGE Image,
    _In_ ULONG Block)
{
    return Block + Block / Image->ChunkRatio;
}

ULONG
VhdxMap(
    _In_ PVHDX_IMAGE Image,
    _In_ ULONGLONG Offset,
    _In_ ULONG Length,
    _Out_ PULONGLONG FileOffset)
{
    ULONG Block = (ULONG)(Offset / Image->BlockSize);
    ULONG Inside = (ULONG)(Offset % Image->BlockSize);
    ULONGLONG Entry = Image->Bat[VhdxBatIndex(Image, Block)];

    *FileOffset = (Entry & VHDX_BAT_STATE_MASK) == VHDX_BAT_FULLY_PRESENT ?
                  (Entry >> VHDX_BAT_OFFSET_SHIFT) * VHDX_MB + Inside : 0;
    return min(Length, Image->BlockSize - Inside);
}
