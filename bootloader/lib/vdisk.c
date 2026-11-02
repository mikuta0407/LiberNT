/*
 * PROJECT:     LiberNT Boot Loader
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Virtual disks stored in VHDX files
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include <bl.h>
#include <vhdx.h>

#define VD_HIBERNATION_FILE L"\\hiberfil.sys"
#define VD_ZONE_STREAM      L"Zone.Identifier"
#define VD_BOOT_OPTION      "VHDBOOT="

typedef struct _DK_VIRTUAL_DISK
{
    VHDX_IMAGE Image;
    PVOID File;
    DK_DEVICE Host;
    WCHAR Path[BCD_PATH_CHARS];
} DK_VIRTUAL_DISK;

BOOLEAN
VdReadBlocks(
    _In_ PDK_VIRTUAL_DISK Virtual,
    _In_ ULONGLONG Block,
    _In_ ULONG Count,
    _Out_ PVOID Buffer)
{
    ULONGLONG Offset = Block * Virtual->Image.LogicalSectorSize;
    ULONGLONG FileOffset;
    ULONG Length = Count * Virtual->Image.LogicalSectorSize;
    ULONG Chunk;
    PUCHAR Output = Buffer;

    while (Length != 0)
    {
        if (Offset >= Virtual->Image.VirtualSize)
        {
            RtlZeroMemory(Output, Length);
            break;
        }

        Chunk = VhdxMap(&Virtual->Image, Offset, (ULONG)min(Length, Virtual->Image.VirtualSize - Offset), &FileOffset);
        if (FileOffset == 0)
            RtlZeroMemory(Output, Chunk);
        else if (!FsReadFileAt(Virtual->File, FileOffset, Output, Chunk))
            return FALSE;

        Output += Chunk;
        Offset += Chunk;
        Length -= Chunk;
    }

    return TRUE;
}

static
BOOLEAN
VdHostHibernated(
    _In_ PDK_DEVICE Host)
{
    ULONGLONG Size;
    CHAR Signature[4];
    PVOID File;
    BOOLEAN Hibernated = FALSE;

    File = FsOpenFile(Host, VD_HIBERNATION_FILE, &Size);
    if (!File)
        return FALSE;

    if (Size >= sizeof(Signature) && FsReadFileAt(File, 0, Signature, sizeof(Signature)))
        Hibernated = _strnicmp(Signature, "hibr", sizeof(Signature)) == 0;

    FsCloseFile(File);
    return Hibernated;
}

BOOLEAN
VdAppendBootOption(
    _In_ PDK_DEVICE Partition,
    _Inout_updates_z_(Size) PSTR Options,
    _In_ SIZE_T Size)
{
    PDK_VIRTUAL_DISK Virtual = Partition->Virtual;
    UCHAR Sector[512], DiskId[16];
    CHAR Disk[40], Path[BCD_PATH_CHARS * 3];
    DK_DEVICE HostDisk;
    PCHAR Output = Path;
    ULONG i;
    WCHAR Char;

    if (!FwGetDisk(Virtual->Host.DiskIndex, &HostDisk))
        return FALSE;

    if (GptGetDiskId(&HostDisk, DiskId))
    {
        RtlStringCbPrintfA(Disk, sizeof(Disk),
                           "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                           LeUlong(DiskId), DiskId[4] | (DiskId[5] << 8), DiskId[6] | (DiskId[7] << 8),
                           DiskId[8], DiskId[9], DiskId[10], DiskId[11],
                           DiskId[12], DiskId[13], DiskId[14], DiskId[15]);
    }
    else if (DkRead(&HostDisk, 0, sizeof(Sector), Sector))
    {
        RtlStringCbPrintfA(Disk, sizeof(Disk), "%08lX", LeUlong(Sector + 0x1B8));
    }
    else
    {
        return FALSE;
    }

    for (i = 0; (Char = Virtual->Path[i]) != UNICODE_NULL; i++)
    {
        if (Char > 0x7F)
            return FALSE;
        if (isalnum(Char) || Char == L'\\' || Char == L'.' || Char == L'_' || Char == L'-')
            *Output++ = (CHAR)Char;
        else
            Output += sprintf(Output, "%%%02X", Char);
    }
    *Output = ANSI_NULL;

    return NT_SUCCESS(RtlStringCbPrintfA(Options + strlen(Options),
                                         Size - strlen(Options),
                                         " %s%s:%I64X:%s",
                                         VD_BOOT_OPTION,
                                         Disk,
                                         Virtual->Host.Offset,
                                         Path));
}

VOID
VdGetDisk(
    _In_ PDK_DEVICE Partition,
    _Out_ PDK_DEVICE Disk)
{
    *Disk = *Partition;
    Disk->Offset = 0;
    Disk->Length = Partition->Virtual->Image.VirtualSize;
    Disk->PartitionNumber = 0;
}

BOOLEAN
DkOpenVirtualDisk(
    _In_ PBCD_DEVICE BcdDevice,
    _Out_ PDK_DEVICE Device)
{
    PDK_VIRTUAL_DISK Virtual;
    BCD_DEVICE HostDevice = *BcdDevice;
    DK_DEVICE Host, Disk;
    ULONGLONG Size;
    NTSTATUS Status;

    HostDevice.Vhd = FALSE;
    if (!DkOpenBcdDevice(&HostDevice, &Host))
    {
        FwPrint(L"The partition that holds %s was not found.\r\n", BcdDevice->VhdPath);
        return FALSE;
    }

    if (FsHasDataStream(&Host, BcdDevice->VhdPath, VD_ZONE_STREAM))
    {
        FwPrint(L"%s came from the Internet and cannot be started.\r\n", BcdDevice->VhdPath);
        return FALSE;
    }

    if (VdHostHibernated(&Host) || FsVolumeIsDirty(&Host))
    {
        FwPrint(L"The partition that holds %s is hibernated or was not shut down cleanly.\r\n"
                L"Shut down the operating system on it fully, then start again.\r\n",
                BcdDevice->VhdPath);
        return FALSE;
    }

    Virtual = FwAllocate(sizeof(*Virtual));
    if (!Virtual)
        return FALSE;

    Virtual->Host = Host;
    RtlStringCchCopyW(Virtual->Path, RTL_NUMBER_OF(Virtual->Path), BcdDevice->VhdPath);

    Virtual->File = FsOpenFile(&Host, BcdDevice->VhdPath, &Size);
    if (!Virtual->File)
    {
        FwPrint(L"%s was not found.\r\n", BcdDevice->VhdPath);
        FwFree(Virtual);
        return FALSE;
    }

    Status = VhdxOpen(&Virtual->Image, FsReadFileAt, Virtual->File, Size, FwAllocate, FwFree);
    if (NT_SUCCESS(Status) && !VhdxLogIsEmpty(&Virtual->Image))
    {
        /* TODO: replay the VHDX log; Windows replays it before the first read */
        VhdxClose(&Virtual->Image, FwFree);
        Status = STATUS_VHD_INVALID_STATE;
    }

    if (!NT_SUCCESS(Status))
    {
        FwPrint(L"%s cannot be opened as a VHDX virtual disk (0x%08lx).\r\n", BcdDevice->VhdPath, Status);
        FsCloseFile(Virtual->File);
        FwFree(Virtual);
        return FALSE;
    }

    RtlZeroMemory(&Disk, sizeof(Disk));
    Disk.Firmware = Virtual;
    Disk.Virtual = Virtual;
    Disk.Length = Virtual->Image.VirtualSize;
    Disk.BlockSize = Virtual->Image.LogicalSectorSize;

    if (!DkLocatePartition(&Disk, BcdDevice->VhdLocatePath, Device))
    {
        FwPrint(L"No partition in %s holds %s.\r\n", BcdDevice->VhdPath, BcdDevice->VhdLocatePath);
        return FALSE;
    }

    return TRUE;
}
