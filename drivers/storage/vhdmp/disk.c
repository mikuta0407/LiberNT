/*
 * PROJECT:     LiberNT VHD Miniport
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     VHDX file access below the host file system
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include "vhdmp.h"

#define NDEBUG
#include <debug.h>

#define VHD_BOUNCE_SIZE  0x10000
#define VHD_ZERO_SIZE    0x100000
#define VHD_NAME_SIZE    1024

static
NTSTATUS
VhdDeviceIo(
    _In_ PDEVICE_OBJECT Target,
    _In_ UCHAR MajorFunction,
    _In_ ULONGLONG Offset,
    _Inout_updates_bytes_opt_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Position;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Position.QuadPart = (LONGLONG)Offset;
    Irp = IoBuildSynchronousFsdRequest(MajorFunction,
                                       Target,
                                       Buffer,
                                       Length,
                                       MajorFunction == IRP_MJ_FLUSH_BUFFERS ? NULL : &Position,
                                       &Event,
                                       &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (MajorFunction == IRP_MJ_WRITE)
        IoGetNextIrpStackLocation(Irp)->Flags |= SL_FORCE_DIRECT_WRITE;

    Status = IoCallDriver(Target, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    return Status;
}

static
NTSTATUS
NTAPI
VhdIoCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
VhdDeviceIoMdl(
    _In_ PDEVICE_OBJECT Target,
    _In_ UCHAR MajorFunction,
    _In_ ULONGLONG Offset,
    _In_ PMDL Source,
    _In_ PUCHAR Va,
    _In_ ULONG Length)
{
    PIO_STACK_LOCATION Stack;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;
    PMDL Mdl;

    Irp = IoAllocateIrp(Target->StackSize, FALSE);
    Mdl = IoAllocateMdl(Va, Length, FALSE, FALSE, NULL);
    if (!Irp || !Mdl)
    {
        if (Irp)
            IoFreeIrp(Irp);
        if (Mdl)
            IoFreeMdl(Mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IoBuildPartialMdl(Source, Mdl, Va, Length);
    Irp->MdlAddress = Mdl;
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = MajorFunction;
    Stack->Parameters.Read.Length = Length;
    Stack->Parameters.Read.ByteOffset.QuadPart = (LONGLONG)Offset;
    if (MajorFunction == IRP_MJ_WRITE)
        Stack->Flags |= SL_FORCE_DIRECT_WRITE;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoSetCompletionRoutine(Irp, VhdIoCompletion, &Event, TRUE, TRUE, TRUE);
    if (IoCallDriver(Target, Irp) == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    Status = Irp->IoStatus.Status;
    IoFreeMdl(Mdl);
    IoFreeIrp(Irp);
    return Status;
}

static
VHD_BUFFER
VhdPoolBuffer(
    _In_ PVOID Buffer)
{
    VHD_BUFFER Result = {Buffer, NULL, NULL};

    return Result;
}

static
VOID
VhdAdvance(
    _Inout_ PVHD_BUFFER Buffer,
    _In_ ULONG Length)
{
    Buffer->System += Length;
    if (Buffer->Mdl)
        Buffer->MdlVa += Length;
}

static
BOOLEAN
VhdMapFile(
    _In_ PVHD_DISK Disk,
    _In_ ULONGLONG FileOffset,
    _In_ ULONG Length,
    _Out_ PULONGLONG DeviceOffset,
    _Out_ PULONG Run)
{
    ULONGLONG Vcn = FileOffset / Disk->ClusterSize;
    ULONG Low = 0, High = Disk->ExtentCount, Middle;
    PVHD_EXTENT Extent;

    while (Low < High)
    {
        Middle = (Low + High) / 2;
        Extent = &Disk->Extents[Middle];
        if (Vcn < Extent->Vcn)
        {
            High = Middle;
        }
        else if (Vcn >= Extent->NextVcn)
        {
            Low = Middle + 1;
        }
        else
        {
            if (Extent->Lcn < 0)
                return FALSE;

            *DeviceOffset = ((ULONGLONG)Extent->Lcn + Vcn - Extent->Vcn) * Disk->ClusterSize +
                            FileOffset % Disk->ClusterSize;
            *Run = (ULONG)min((ULONGLONG)Length,
                              (Extent->NextVcn - Vcn) * Disk->ClusterSize - FileOffset % Disk->ClusterSize);
            return TRUE;
        }
    }

    return FALSE;
}

static
NTSTATUS
VhdFileIo(
    _In_ PVHD_DISK Disk,
    _In_ BOOLEAN Write,
    _In_ ULONGLONG FileOffset,
    _In_ VHD_BUFFER Buffer,
    _In_ ULONG Length)
{
    ULONGLONG DeviceOffset, Start;
    ULONG Run, Span, Skip;
    NTSTATUS Status;

    while (Length != 0)
    {
        if (!VhdMapFile(Disk, FileOffset, Length, &DeviceOffset, &Run))
            return STATUS_FILE_CORRUPT_ERROR;

        if (DeviceOffset % Disk->SectorSize == 0 && Run % Disk->SectorSize == 0)
        {
            Status = Buffer.Mdl ? VhdDeviceIoMdl(Disk->Target, Write ? IRP_MJ_WRITE : IRP_MJ_READ, DeviceOffset,
                                                 Buffer.Mdl, Buffer.MdlVa, Run) :
                                  VhdDeviceIo(Disk->Target, Write ? IRP_MJ_WRITE : IRP_MJ_READ, DeviceOffset,
                                              Buffer.System, Run);
        }
        else
        {
            Start = DeviceOffset - DeviceOffset % Disk->SectorSize;
            Skip = (ULONG)(DeviceOffset - Start);
            Run = min(Run, Disk->BounceSize - Skip);
            Span = ALIGN_UP_BY(Skip + Run, Disk->SectorSize);

            Status = VhdDeviceIo(Disk->Target, IRP_MJ_READ, Start, Disk->Bounce, Span);
            if (NT_SUCCESS(Status))
            {
                if (Write)
                {
                    RtlCopyMemory(Disk->Bounce + Skip, Buffer.System, Run);
                    Status = VhdDeviceIo(Disk->Target, IRP_MJ_WRITE, Start, Disk->Bounce, Span);
                }
                else
                {
                    RtlCopyMemory(Buffer.System, Disk->Bounce + Skip, Run);
                }
            }
        }

        if (!NT_SUCCESS(Status))
            return Status;

        FileOffset += Run;
        VhdAdvance(&Buffer, Run);
        Length -= Run;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
VhdAllocateBlock(
    _Inout_ PVHD_DISK Disk,
    _In_ ULONGLONG Offset)
{
    ULONG Block = (ULONG)(Offset / Disk->Image.BlockSize);
    ULONG Index = VhdxBatIndex(&Disk->Image, Block);
    ULONGLONG FileOffset = Disk->NextFree, Entry;
    ULONG Done, Chunk;
    NTSTATUS Status;

    if (FileOffset + Disk->Image.BlockSize > Disk->FileSize)
        return STATUS_DISK_FULL;

    for (Done = 0; Done < Disk->Image.BlockSize; Done += Chunk)
    {
        Chunk = min(Disk->Image.BlockSize - Done, VHD_ZERO_SIZE);
        Status = VhdFileIo(Disk, TRUE, FileOffset + Done, VhdPoolBuffer(Disk->Zero), Chunk);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* FIXME: the BAT entry is written in place without the VHDX log */
    Entry = (FileOffset / VHDX_MB) << VHDX_BAT_OFFSET_SHIFT | VHDX_BAT_FULLY_PRESENT;
    Status = VhdFileIo(Disk,
                       TRUE,
                       Disk->Image.BatOffset + (ULONGLONG)Index * sizeof(ULONGLONG),
                       VhdPoolBuffer(&Entry),
                       sizeof(Entry));
    if (!NT_SUCCESS(Status))
        return Status;

    Disk->Image.Bat[Index] = Entry;
    Disk->NextFree += Disk->Image.BlockSize;
    return STATUS_SUCCESS;
}

NTSTATUS
VhdReadWrite(
    _Inout_ PVHD_DISK Disk,
    _In_ BOOLEAN Write,
    _In_ ULONGLONG Offset,
    _In_ VHD_BUFFER Buffer,
    _In_ ULONG Length)
{
    ULONGLONG FileOffset;
    ULONG Chunk;
    NTSTATUS Status;

    if (Offset > Disk->Image.VirtualSize || Length > Disk->Image.VirtualSize - Offset)
        return STATUS_INVALID_PARAMETER;

    while (Length != 0)
    {
        Chunk = VhdxMap(&Disk->Image, Offset, Length, &FileOffset);
        if (FileOffset != 0)
        {
            Status = VhdFileIo(Disk, Write, FileOffset, Buffer, Chunk);
            if (!NT_SUCCESS(Status))
                return Status;
        }
        else if (!Write)
        {
            RtlZeroMemory(Buffer.System, Chunk);
        }
        else
        {
            Status = VhdAllocateBlock(Disk, Offset);
            if (!NT_SUCCESS(Status))
                return Status;
            continue;
        }

        Offset += Chunk;
        VhdAdvance(&Buffer, Chunk);
        Length -= Chunk;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
VhdFlush(
    _In_ PVHD_DISK Disk)
{
    return VhdDeviceIo(Disk->Target, IRP_MJ_FLUSH_BUFFERS, 0, NULL, 0);
}

static
BOOLEAN
VhdReadFile(
    _In_ PVOID Context,
    _In_ ULONGLONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Position;

    Position.QuadPart = (LONGLONG)Offset;
    return NT_SUCCESS(ZwReadFile(Context, NULL, NULL, NULL, &IoStatus, Buffer, Length, &Position, NULL)) &&
           IoStatus.Information == Length;
}

static
PVOID
VhdAllocate(
    _In_ SIZE_T Size)
{
    return ExAllocatePoolZero(NonPagedPoolNx, Size, VHD_TAG);
}

static
VOID
VhdFree(
    _In_ PVOID Buffer)
{
    ExFreePoolWithTag(Buffer, VHD_TAG);
}

static
NTSTATUS
VhdExpand(
    _Inout_ PVHD_DISK Disk)
{
    FILE_END_OF_FILE_INFORMATION EndOfFile;
    FILE_VALID_DATA_LENGTH_INFORMATION ValidData;
    IO_STATUS_BLOCK IoStatus;
    ULONG Block, Missing = 0;
    NTSTATUS Status;

    for (Block = 0; Block < Disk->Image.PayloadBlocks; Block++)
    {
        if ((Disk->Image.Bat[VhdxBatIndex(&Disk->Image, Block)] & VHDX_BAT_STATE_MASK) != VHDX_BAT_FULLY_PRESENT)
            Missing++;
    }

    Disk->NextFree = ALIGN_UP_BY(VhdxUsedEnd(&Disk->Image), VHDX_MB);
    EndOfFile.EndOfFile.QuadPart = (LONGLONG)(Disk->NextFree + (ULONGLONG)Missing * Disk->Image.BlockSize);
    if (Missing == 0 || (ULONGLONG)EndOfFile.EndOfFile.QuadPart <= Disk->FileSize)
        return STATUS_SUCCESS;

    Status = ZwSetInformationFile(Disk->File, &IoStatus, &EndOfFile, sizeof(EndOfFile), FileEndOfFileInformation);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("VHDMP: cannot expand the virtual disk file to %I64u bytes: 0x%08lx\n",
                EndOfFile.EndOfFile.QuadPart,
                Status);
        return Status;
    }

    /* FIXME: Windows keeps the valid data length at the end of the used blocks; data I/O here bypasses the host file system */
    ValidData.ValidDataLength = EndOfFile.EndOfFile;
    Status = ZwSetInformationFile(Disk->File, &IoStatus, &ValidData, sizeof(ValidData), FileValidDataLengthInformation);
    if (!NT_SUCCESS(Status))
        return Status;

    Disk->FileSize = (ULONGLONG)EndOfFile.EndOfFile.QuadPart;
    return STATUS_SUCCESS;
}

static
NTSTATUS
VhdReadExtents(
    _Inout_ PVHD_DISK Disk)
{
    STARTING_VCN_INPUT_BUFFER Start;
    PRETRIEVAL_POINTERS_BUFFER Pointers;
    PVHD_EXTENT Extents = NULL, Grown;
    IO_STATUS_BLOCK IoStatus;
    ULONG Count = 0, Capacity = 0, i;
    ULONGLONG Vcn;
    NTSTATUS Status;

    Pointers = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, VHD_TAG);
    if (!Pointers)
        return STATUS_INSUFFICIENT_RESOURCES;

    Start.StartingVcn.QuadPart = 0;
    do
    {
        Status = ZwFsControlFile(Disk->File, NULL, NULL, NULL, &IoStatus, FSCTL_GET_RETRIEVAL_POINTERS,
                                 &Start, sizeof(Start), Pointers, PAGE_SIZE);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
            break;

        Vcn = (ULONGLONG)Pointers->StartingVcn.QuadPart;
        for (i = 0; i < Pointers->ExtentCount; i++)
        {
            if (Count == Capacity)
            {
                Capacity = Capacity ? Capacity * 2 : 64;
                Grown = ExAllocatePoolWithTag(NonPagedPoolNx, Capacity * sizeof(*Grown), VHD_TAG);
                if (!Grown)
                {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    break;
                }
                if (Extents)
                {
                    RtlCopyMemory(Grown, Extents, Count * sizeof(*Grown));
                    ExFreePoolWithTag(Extents, VHD_TAG);
                }
                Extents = Grown;
            }

            Extents[Count].Vcn = Vcn;
            Extents[Count].NextVcn = (ULONGLONG)Pointers->Extents[i].NextVcn.QuadPart;
            Extents[Count].Lcn = Pointers->Extents[i].Lcn.QuadPart;
            Vcn = Extents[Count].NextVcn;
            Count++;
        }

        Start.StartingVcn.QuadPart = (LONGLONG)Vcn;
    } while (Status == STATUS_BUFFER_OVERFLOW);

    ExFreePoolWithTag(Pointers, VHD_TAG);

    if (NT_SUCCESS(Status))
    {
        for (i = 0; i < Count; i++)
        {
            if (Extents[i].Lcn < 0)
                Status = STATUS_NOT_SUPPORTED;
        }

        if (Count == 0 || Extents[Count - 1].NextVcn * Disk->ClusterSize < Disk->FileSize)
            Status = STATUS_FILE_CORRUPT_ERROR;
    }

    if (!NT_SUCCESS(Status))
    {
        if (Extents)
            ExFreePoolWithTag(Extents, VHD_TAG);
        return Status;
    }

    Disk->Extents = Extents;
    Disk->ExtentCount = Count;
    return STATUS_SUCCESS;
}

static
NTSTATUS
VhdWriteHeaders(
    _Inout_ PVHD_DISK Disk)
{
    UCHAR Header[VHDX_HEADER_SIZE];
    ULONG i;
    NTSTATUS Status;

    Status = ExUuidCreate((UUID*)Disk->Image.FileWriteGuid);
    if (NT_SUCCESS(Status))
        Status = ExUuidCreate((UUID*)Disk->Image.DataWriteGuid);
    if (!NT_SUCCESS(Status))
        return Status;

    for (i = 0; i < 2; i++)
    {
        Disk->Image.SequenceNumber++;
        Disk->Image.HeaderOffset = Disk->Image.HeaderOffset == VHDX_HEADER1_OFFSET ? VHDX_HEADER2_OFFSET :
                                                                                     VHDX_HEADER1_OFFSET;
        VhdxBuildHeader(&Disk->Image, Header);
        Status = VhdFileIo(Disk, TRUE, Disk->Image.HeaderOffset, VhdPoolBuffer(Header), sizeof(Header));
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return VhdFlush(Disk);
}

static
NTSTATUS
VhdQueryNames(
    _Inout_ PVHD_DISK Disk,
    _In_ PDEVICE_OBJECT Volume)
{
    POBJECT_NAME_INFORMATION Name;
    PFILE_NAME_INFORMATION File;
    IO_STATUS_BLOCK IoStatus;
    ULONG Length = VHD_NAME_SIZE;
    NTSTATUS Status;

    Name = ExAllocatePoolWithTag(PagedPool, VHD_NAME_SIZE, VHD_TAG);
    File = ExAllocatePoolWithTag(PagedPool, VHD_NAME_SIZE, VHD_TAG);
    if (!Name || !File)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    Status = ObQueryNameString(Volume, Name, Length, &Length);
    if (NT_SUCCESS(Status))
        Status = ZwQueryInformationFile(Disk->File, &IoStatus, File, VHD_NAME_SIZE, FileNameInformation);
    if (!NT_SUCCESS(Status))
        goto Done;

    Disk->RelativePath.Buffer = ExAllocatePoolWithTag(PagedPool, File->FileNameLength, VHD_TAG);
    if (!Disk->RelativePath.Buffer)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    RtlCopyMemory(Disk->RelativePath.Buffer, File->FileName, File->FileNameLength);
    Disk->RelativePath.Length = Disk->RelativePath.MaximumLength = (USHORT)File->FileNameLength;
    Disk->HostVolume = Name->Name;
    Name = NULL;

Done:
    if (Name)
        ExFreePoolWithTag(Name, VHD_TAG);
    if (File)
        ExFreePoolWithTag(File, VHD_TAG);
    return Status;
}

NTSTATUS
VhdOpenDisk(
    _Out_ PVHD_DISK Disk,
    _In_ PCUNICODE_STRING Path)
{
    FILE_STANDARD_INFORMATION Standard;
    FILE_BASIC_INFORMATION Basic;
    FILE_FS_SIZE_INFORMATION Size;
    FILE_SET_SPARSE_BUFFER Sparse;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK IoStatus;
    PDEVICE_OBJECT Device;
    NTSTATUS Status;

    RtlZeroMemory(Disk, sizeof(*Disk));

    InitializeObjectAttributes(&Attributes, (PUNICODE_STRING)Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateFile(&Disk->File,
                          GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                          &Attributes,
                          &IoStatus,
                          NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ,
                          FILE_OPEN,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
                          NULL,
                          0);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("VHDMP: cannot open %wZ: 0x%08lx\n", Path, Status);
        return Status;
    }

    Status = ZwQueryInformationFile(Disk->File, &IoStatus, &Basic, sizeof(Basic), FileBasicInformation);
    if (NT_SUCCESS(Status))
        Status = ZwQueryInformationFile(Disk->File, &IoStatus, &Standard, sizeof(Standard), FileStandardInformation);
    if (NT_SUCCESS(Status) && (Basic.FileAttributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED)))
        Status = STATUS_NOT_SUPPORTED;
    if (NT_SUCCESS(Status) && (Basic.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE))
    {
        Sparse.SetSparse = FALSE;
        Status = ZwFsControlFile(Disk->File, NULL, NULL, NULL, &IoStatus, FSCTL_SET_SPARSE,
                                 &Sparse, sizeof(Sparse), NULL, 0);
    }
    if (!NT_SUCCESS(Status))
        goto Fail;

    Disk->FileSize = (ULONGLONG)Standard.EndOfFile.QuadPart;
    Status = VhdxOpen(&Disk->Image, VhdReadFile, Disk->File, Disk->FileSize, VhdAllocate, VhdFree);
    if (NT_SUCCESS(Status) && !VhdxLogIsEmpty(&Disk->Image))
    {
        /* TODO: replay the VHDX log before the first read */
        Status = STATUS_VHD_INVALID_STATE;
    }
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("VHDMP: %wZ is not a usable VHDX file: 0x%08lx\n", Path, Status);
        goto Fail;
    }

    Status = VhdExpand(Disk);
    if (NT_SUCCESS(Status))
        Status = ZwFlushBuffersFile(Disk->File, &IoStatus);
    if (NT_SUCCESS(Status))
        Status = ZwQueryVolumeInformationFile(Disk->File, &IoStatus, &Size, sizeof(Size), FileFsSizeInformation);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Disk->SectorSize = Size.BytesPerSector;
    Disk->ClusterSize = Size.SectorsPerAllocationUnit * Size.BytesPerSector;
    Status = VhdReadExtents(Disk);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("VHDMP: cannot map %wZ on the host volume: 0x%08lx\n", Path, Status);
        goto Fail;
    }

    Status = ObReferenceObjectByHandle(Disk->File, 0, *IoFileObjectType, KernelMode, (PVOID*)&Disk->FileObject, NULL);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Device = Disk->FileObject->Vpb ? Disk->FileObject->Vpb->RealDevice : Disk->FileObject->DeviceObject;
    Disk->Target = IoGetAttachedDeviceReference(Device);
    Status = VhdQueryNames(Disk, Device);
    if (!NT_SUCCESS(Status))
        goto Fail;

    Disk->BounceSize = VHD_BOUNCE_SIZE;
    Disk->Bounce = ExAllocatePoolWithTag(NonPagedPoolNx, VHD_BOUNCE_SIZE, VHD_TAG);
    Disk->Zero = ExAllocatePoolZero(NonPagedPoolNx, VHD_ZERO_SIZE, VHD_TAG);
    if (!Disk->Bounce || !Disk->Zero)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }

    Status = VhdWriteHeaders(Disk);
    if (NT_SUCCESS(Status))
        return STATUS_SUCCESS;

Fail:
    if (Disk->Target)
        ObDereferenceObject(Disk->Target);
    if (Disk->FileObject)
        ObDereferenceObject(Disk->FileObject);
    if (Disk->Bounce)
        ExFreePoolWithTag(Disk->Bounce, VHD_TAG);
    if (Disk->Zero)
        ExFreePoolWithTag(Disk->Zero, VHD_TAG);
    if (Disk->Extents)
        ExFreePoolWithTag(Disk->Extents, VHD_TAG);
    VhdxClose(&Disk->Image, VhdFree);
    ZwClose(Disk->File);
    RtlZeroMemory(Disk, sizeof(*Disk));
    return Status;
}
