/*
 * PROJECT:     LiberNT VHD Miniport
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SCSI commands and storage queries of the virtual disk
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include "vhdmp.h"

#define NDEBUG
#include <debug.h>

#define VHD_MAX_TRANSFER 0x100000

static
ULONG
VhdBigEndian32(
    _In_reads_bytes_(4) const UCHAR* Data)
{
    return ((ULONG)Data[0] << 24) | ((ULONG)Data[1] << 16) | ((ULONG)Data[2] << 8) | Data[3];
}

static
ULONGLONG
VhdBigEndian64(
    _In_reads_bytes_(8) const UCHAR* Data)
{
    return ((ULONGLONG)VhdBigEndian32(Data) << 32) | VhdBigEndian32(Data + 4);
}

static
VOID
VhdStoreBigEndian32(
    _Out_writes_bytes_all_(4) PUCHAR Data,
    _In_ ULONG Value)
{
    Data[0] = (UCHAR)(Value >> 24);
    Data[1] = (UCHAR)(Value >> 16);
    Data[2] = (UCHAR)(Value >> 8);
    Data[3] = (UCHAR)Value;
}

static
VOID
VhdStoreBigEndian64(
    _Out_writes_bytes_all_(8) PUCHAR Data,
    _In_ ULONGLONG Value)
{
    VhdStoreBigEndian32(Data, (ULONG)(Value >> 32));
    VhdStoreBigEndian32(Data + 4, (ULONG)Value);
}

static
NTSTATUS
VhdCheckCondition(
    _Inout_ PSCSI_REQUEST_BLOCK Srb,
    _In_ UCHAR SenseKey,
    _In_ UCHAR AdditionalSenseCode)
{
    PSENSE_DATA Sense = Srb->SenseInfoBuffer;

    Srb->SrbStatus = SRB_STATUS_ERROR;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
    Srb->DataTransferLength = 0;

    if (Sense && Srb->SenseInfoBufferLength >= sizeof(SENSE_DATA) && !(Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE))
    {
        RtlZeroMemory(Sense, sizeof(SENSE_DATA));
        Sense->ErrorCode = SCSI_SENSE_ERRORCODE_FIXED_CURRENT;
        Sense->SenseKey = SenseKey;
        Sense->AdditionalSenseLength = sizeof(SENSE_DATA) - RTL_SIZEOF_THROUGH_FIELD(SENSE_DATA, AdditionalSenseLength);
        Sense->AdditionalSenseCode = AdditionalSenseCode;
        Srb->SenseInfoBufferLength = sizeof(SENSE_DATA);
        Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
    }

    return STATUS_IO_DEVICE_ERROR;
}

static
NTSTATUS
VhdReturnData(
    _Inout_ PSCSI_REQUEST_BLOCK Srb,
    _Out_writes_bytes_(Srb->DataTransferLength) PUCHAR Buffer,
    _In_reads_bytes_(Length) const VOID* Data,
    _In_ ULONG Length)
{
    Srb->DataTransferLength = min(Srb->DataTransferLength, Length);
    RtlCopyMemory(Buffer, Data, Srb->DataTransferLength);
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

static
NTSTATUS
VhdInquiry(
    _In_ PVHD_DISK Disk,
    _Inout_ PSCSI_REQUEST_BLOCK Srb,
    _Out_writes_bytes_(Srb->DataTransferLength) PUCHAR Buffer)
{
    static const UCHAR Pages[] = {DIRECT_ACCESS_DEVICE, VPD_SUPPORTED_PAGES, 0, 2,
                                  VPD_SUPPORTED_PAGES, VPD_SERIAL_NUMBER};
    UCHAR Serial[4 + 32];
    INQUIRYDATA Inquiry;
    PCDB Cdb = (PCDB)Srb->Cdb;
    ULONG i;

    if (Cdb->CDB6INQUIRY3.EnableVitalProductData)
    {
        switch (Cdb->CDB6INQUIRY3.PageCode)
        {
            case VPD_SUPPORTED_PAGES:
                return VhdReturnData(Srb, Buffer, Pages, sizeof(Pages));

            case VPD_SERIAL_NUMBER:
                Serial[0] = DIRECT_ACCESS_DEVICE;
                Serial[1] = VPD_SERIAL_NUMBER;
                Serial[2] = 0;
                Serial[3] = 32;
                for (i = 0; i < 16; i++)
                {
                    Serial[4 + i * 2] = "0123456789ABCDEF"[Disk->Image.DiskId[i] >> 4];
                    Serial[5 + i * 2] = "0123456789ABCDEF"[Disk->Image.DiskId[i] & 15];
                }
                return VhdReturnData(Srb, Buffer, Serial, sizeof(Serial));
        }

        return VhdCheckCondition(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB);
    }

    RtlZeroMemory(&Inquiry, sizeof(Inquiry));
    Inquiry.DeviceType = DIRECT_ACCESS_DEVICE;
    Inquiry.Versions = 5;
    Inquiry.ResponseDataFormat = 2;
    Inquiry.AdditionalLength = INQUIRYDATABUFFERSIZE - RTL_SIZEOF_THROUGH_FIELD(INQUIRYDATA, AdditionalLength);
    RtlCopyMemory(Inquiry.VendorId, VHD_VENDOR, sizeof(Inquiry.VendorId));
    RtlCopyMemory(Inquiry.ProductId, VHD_PRODUCT, sizeof(Inquiry.ProductId));
    RtlCopyMemory(Inquiry.ProductRevisionLevel, VHD_REVISION, sizeof(Inquiry.ProductRevisionLevel));
    return VhdReturnData(Srb, Buffer, &Inquiry, INQUIRYDATABUFFERSIZE);
}

static
NTSTATUS
VhdTransfer(
    _In_ PVHD_DISK Disk,
    _Inout_ PSCSI_REQUEST_BLOCK Srb,
    _In_ VHD_BUFFER Buffer,
    _In_ BOOLEAN Write,
    _In_ ULONGLONG Lba,
    _In_ ULONG Blocks)
{
    ULONG SectorSize = Disk->Image.LogicalSectorSize;
    ULONGLONG Sectors = Disk->Image.VirtualSize / SectorSize;
    ULONG Length;
    NTSTATUS Status;

    if (Lba > Sectors || Blocks > Sectors - Lba)
        return VhdCheckCondition(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_ILLEGAL_BLOCK);

    if ((ULONGLONG)Blocks * SectorSize > Srb->DataTransferLength)
        return VhdCheckCondition(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB);

    Length = Blocks * SectorSize;
    Status = VhdReadWrite(Disk, Write, Lba * SectorSize, Buffer, Length);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("VHDMP: %s of %lu sectors at %I64u failed: 0x%08lx\n", Write ? "write" : "read", Blocks, Lba, Status);
        return VhdCheckCondition(Srb, Write ? SCSI_SENSE_HARDWARE_ERROR : SCSI_SENSE_MEDIUM_ERROR, 0);
    }

    Srb->DataTransferLength = Length;
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

static
NTSTATUS
VhdModeSense(
    _Inout_ PSCSI_REQUEST_BLOCK Srb,
    _Out_writes_bytes_(Srb->DataTransferLength) PUCHAR Buffer)
{
    UCHAR Header[8] = {0};

    if (Srb->Cdb[0] == SCSIOP_MODE_SENSE)
    {
        Header[0] = sizeof(MODE_PARAMETER_HEADER) - 1;
        return VhdReturnData(Srb, Buffer, Header, sizeof(MODE_PARAMETER_HEADER));
    }

    Header[1] = sizeof(MODE_PARAMETER_HEADER10) - 2;
    return VhdReturnData(Srb, Buffer, Header, sizeof(MODE_PARAMETER_HEADER10));
}

static
NTSTATUS
VhdExecuteCdb(
    _In_ PVHD_DISK Disk,
    _Inout_ PSCSI_REQUEST_BLOCK Srb,
    _In_ VHD_BUFFER Data)
{
    PUCHAR Buffer = Data.System;
    READ_CAPACITY_DATA Capacity;
    READ_CAPACITY16_DATA Capacity16;
    PCDB Cdb = (PCDB)Srb->Cdb;
    ULONGLONG LastLba = Disk->Image.VirtualSize / Disk->Image.LogicalSectorSize - 1;
    ULONG Exponent;

    Srb->ScsiStatus = SCSISTAT_GOOD;

    if (!Buffer && Srb->DataTransferLength != 0)
        return VhdCheckCondition(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB);

    switch (Cdb->CDB6GENERIC.OperationCode)
    {
        case SCSIOP_TEST_UNIT_READY:
        case SCSIOP_START_STOP_UNIT:
        case SCSIOP_MEDIUM_REMOVAL:
        case SCSIOP_VERIFY:
        case SCSIOP_VERIFY16:
        case SCSIOP_RESERVE_UNIT:
        case SCSIOP_RELEASE_UNIT:
            Srb->DataTransferLength = 0;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return STATUS_SUCCESS;

        case SCSIOP_INQUIRY:
            return VhdInquiry(Disk, Srb, Buffer);

        case SCSIOP_READ_CAPACITY:
            VhdStoreBigEndian32((PUCHAR)&Capacity.LogicalBlockAddress, (ULONG)min(LastLba, MAXULONG));
            VhdStoreBigEndian32((PUCHAR)&Capacity.BytesPerBlock, Disk->Image.LogicalSectorSize);
            return VhdReturnData(Srb, Buffer, &Capacity, sizeof(Capacity));

        case SCSIOP_SERVICE_ACTION_IN16:
            if (Cdb->READ_CAPACITY16.ServiceAction != SERVICE_ACTION_READ_CAPACITY16)
                break;

            RtlZeroMemory(&Capacity16, sizeof(Capacity16));
            VhdStoreBigEndian64((PUCHAR)&Capacity16.LogicalBlockAddress, LastLba);
            VhdStoreBigEndian32((PUCHAR)&Capacity16.BytesPerBlock, Disk->Image.LogicalSectorSize);
            for (Exponent = 0;
                 (Disk->Image.LogicalSectorSize << Exponent) < Disk->Image.PhysicalSectorSize;
                 Exponent++)
                ;
            Capacity16.LogicalPerPhysicalExponent = (UCHAR)Exponent;
            return VhdReturnData(Srb, Buffer, &Capacity16, sizeof(Capacity16));

        case SCSIOP_READ6:
        case SCSIOP_WRITE6:
            return VhdTransfer(Disk, Srb, Data, Cdb->CDB6READWRITE.OperationCode == SCSIOP_WRITE6,
                               ((ULONG)(Cdb->CDB6READWRITE.LogicalBlockMsb1 & 0x1F) << 16) |
                               (Cdb->CDB6READWRITE.LogicalBlockMsb0 << 8) | Cdb->CDB6READWRITE.LogicalBlockLsb,
                               Cdb->CDB6READWRITE.TransferBlocks ? Cdb->CDB6READWRITE.TransferBlocks : 256);

        case SCSIOP_READ:
        case SCSIOP_WRITE:
            return VhdTransfer(Disk, Srb, Data, Cdb->CDB10.OperationCode == SCSIOP_WRITE,
                               VhdBigEndian32(&Cdb->CDB10.LogicalBlockByte0),
                               (Cdb->CDB10.TransferBlocksMsb << 8) | Cdb->CDB10.TransferBlocksLsb);

        case SCSIOP_READ12:
        case SCSIOP_WRITE12:
            return VhdTransfer(Disk, Srb, Data, Cdb->CDB12.OperationCode == SCSIOP_WRITE12,
                               VhdBigEndian32(Cdb->CDB12.LogicalBlock),
                               VhdBigEndian32(Cdb->CDB12.TransferLength));

        case SCSIOP_READ16:
        case SCSIOP_WRITE16:
            return VhdTransfer(Disk, Srb, Data, Cdb->CDB16.OperationCode == SCSIOP_WRITE16,
                               VhdBigEndian64(Cdb->CDB16.LogicalBlock),
                               VhdBigEndian32(Cdb->CDB16.TransferLength));

        case SCSIOP_SYNCHRONIZE_CACHE:
        case SCSIOP_SYNCHRONIZE_CACHE16:
            Srb->DataTransferLength = 0;
            if (!NT_SUCCESS(VhdFlush(Disk)))
                return VhdCheckCondition(Srb, SCSI_SENSE_HARDWARE_ERROR, 0);
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            return STATUS_SUCCESS;

        case SCSIOP_MODE_SENSE:
        case SCSIOP_MODE_SENSE10:
            return VhdModeSense(Srb, Buffer);
    }

    return VhdCheckCondition(Srb, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_ILLEGAL_COMMAND);
}

NTSTATUS
VhdExecuteSrb(
    _In_ PVHD_PDO_EXTENSION Extension,
    _Inout_ PIRP Irp)
{
    PSCSI_REQUEST_BLOCK Srb = IoGetCurrentIrpStackLocation(Irp)->Parameters.Scsi.Srb;
    VHD_BUFFER Buffer = {NULL, NULL, NULL};
    NTSTATUS Status = STATUS_SUCCESS;

    switch (Srb->Function)
    {
        case SRB_FUNCTION_EXECUTE_SCSI:
            if (Srb->DataTransferLength != 0)
            {
                if (Irp->MdlAddress)
                {
                    Buffer.Mdl = Irp->MdlAddress;
                    Buffer.MdlVa = Srb->DataBuffer;
                    Buffer.System = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
                    if (Buffer.System)
                        Buffer.System += (PUCHAR)Srb->DataBuffer - (PUCHAR)MmGetMdlVirtualAddress(Irp->MdlAddress);
                }
                else
                {
                    Buffer.System = Srb->DataBuffer;
                }
            }
            Status = VhdExecuteCdb(&Extension->Disk, Srb, Buffer);
            Irp->IoStatus.Information = NT_SUCCESS(Status) ? Srb->DataTransferLength : 0;
            break;

        case SRB_FUNCTION_CLAIM_DEVICE:
            Srb->DataBuffer = Extension->Common.Self;
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;

        case SRB_FUNCTION_FLUSH:
        case SRB_FUNCTION_SHUTDOWN:
            Status = VhdFlush(&Extension->Disk);
            Srb->SrbStatus = NT_SUCCESS(Status) ? SRB_STATUS_SUCCESS : SRB_STATUS_ERROR;
            break;

        default:
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            break;
    }

    Irp->IoStatus.Status = Status;
    return Status;
}

static
NTSTATUS
VhdQueryProperty(
    _In_ PVHD_PDO_EXTENSION Extension,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PSTORAGE_PROPERTY_QUERY Query = Irp->AssociatedIrp.SystemBuffer;
    ULONG OutputLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;
    UCHAR Data[sizeof(STORAGE_DEVICE_DESCRIPTOR) + 64];
    PSTORAGE_DEVICE_DESCRIPTOR Device = (PSTORAGE_DEVICE_DESCRIPTOR)Data;
    PSTORAGE_ADAPTER_DESCRIPTOR Adapter = (PSTORAGE_ADAPTER_DESCRIPTOR)Data;
    PSTORAGE_ACCESS_ALIGNMENT_DESCRIPTOR Alignment = (PSTORAGE_ACCESS_ALIGNMENT_DESCRIPTOR)Data;
    PVHD_DISK Disk = &Extension->Disk;
    ULONG Length;
    PCHAR Strings;

    if (Stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(STORAGE_PROPERTY_QUERY))
        return STATUS_INVALID_PARAMETER;

    if (Query->PropertyId != StorageDeviceProperty &&
        Query->PropertyId != StorageAdapterProperty &&
        Query->PropertyId != StorageAccessAlignmentProperty)
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (Query->QueryType == PropertyExistsQuery)
        return STATUS_SUCCESS;
    if (Query->QueryType != PropertyStandardQuery)
        return STATUS_NOT_SUPPORTED;

    RtlZeroMemory(Data, sizeof(Data));
    switch (Query->PropertyId)
    {
        case StorageDeviceProperty:
            Strings = (PCHAR)(Device + 1);
            Device->DeviceType = DIRECT_ACCESS_DEVICE;
            Device->BusType = BusTypeFileBackedVirtual;
            Device->VendorIdOffset = sizeof(*Device);
            RtlCopyMemory(Strings, "Msft", sizeof("Msft"));
            Device->ProductIdOffset = Device->VendorIdOffset + sizeof("Msft");
            RtlCopyMemory(Strings + sizeof("Msft"), "Virtual Disk", sizeof("Virtual Disk"));
            Device->ProductRevisionOffset = Device->ProductIdOffset + sizeof("Virtual Disk");
            RtlCopyMemory(Strings + sizeof("Msft") + sizeof("Virtual Disk"), "1.0", sizeof("1.0"));
            Length = Device->ProductRevisionOffset + sizeof("1.0");
            break;

        case StorageAdapterProperty:
            Adapter->MaximumTransferLength = VHD_MAX_TRANSFER;
            Adapter->MaximumPhysicalPages = VHD_MAX_TRANSFER / PAGE_SIZE + 1;
            Adapter->AlignmentMask = FILE_WORD_ALIGNMENT;
            Adapter->BusType = BusTypeFileBackedVirtual;
            Adapter->BusMajorVersion = 1;
            Adapter->SrbType = SRB_TYPE_SCSI_REQUEST_BLOCK;
            Adapter->AddressType = STORAGE_ADDRESS_TYPE_BTL8;
            Length = sizeof(*Adapter);
            break;

        default:
            Alignment->BytesPerLogicalSector = Disk->Image.LogicalSectorSize;
            Alignment->BytesPerPhysicalSector = Disk->Image.PhysicalSectorSize;
            Alignment->BytesPerCacheLine = Disk->Image.PhysicalSectorSize;
            Length = sizeof(*Alignment);
            break;
    }

    Device->Version = Length;
    Device->Size = Length;
    if (OutputLength < sizeof(STORAGE_DESCRIPTOR_HEADER))
        return STATUS_INFO_LENGTH_MISMATCH;

    Irp->IoStatus.Information = min(OutputLength, Length);
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Data, Irp->IoStatus.Information);
    return STATUS_SUCCESS;
}

static
VOID
VhdAppendName(
    _Inout_ PSTORAGE_QUERY_DEPENDENT_VOLUME_RESPONSE Response,
    _Inout_ PULONG Length,
    _In_ PCUNICODE_STRING Name,
    _Out_ PULONG Offset,
    _Out_ PULONG Size)
{
    *Offset = *Length;
    *Size = Name->Length;
    RtlCopyMemory((PUCHAR)Response + *Length, Name->Buffer, Name->Length);
    *(PWCHAR)((PUCHAR)Response + *Length + Name->Length) = UNICODE_NULL;
    *Length += Name->Length + sizeof(WCHAR);
}

static
NTSTATUS
VhdQueryDependentVolume(
    _In_ PVHD_PDO_EXTENSION Extension,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PSTORAGE_QUERY_DEPENDENT_VOLUME_REQUEST Request = Irp->AssociatedIrp.SystemBuffer;
    PSTORAGE_QUERY_DEPENDENT_VOLUME_RESPONSE Response = Irp->AssociatedIrp.SystemBuffer;
    PSTORAGE_QUERY_DEPENDENT_VOLUME_LEV2_ENTRY Entry = &Response->Lev2Depends[0];
    PVHD_DISK Disk = &Extension->Disk;
    STORAGE_DEVICE_NUMBER Number;
    WCHAR DeviceBuffer[32];
    UNICODE_STRING Device;
    PDEVICE_OBJECT Top;
    ULONG Level, Flags, Length;
    NTSTATUS Status;

    if (Stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(*Request))
        return STATUS_INVALID_PARAMETER;

    Level = Request->RequestLevel;
    Flags = Request->RequestFlags;
    if (Level != 1 && Level != 2)
        return STATUS_INVALID_PARAMETER;

    Top = IoGetAttachedDeviceReference(Extension->Common.Self);
    Status = VhdDeviceIoControl(Top, IOCTL_STORAGE_GET_DEVICE_NUMBER, &Number, sizeof(Number));
    ObDereferenceObject(Top);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitEmptyUnicodeString(&Device, DeviceBuffer, sizeof(DeviceBuffer));
    RtlUnicodeStringPrintf(&Device, L"\\??\\PhysicalDrive%lu", Number.DeviceNumber);

    Length = Level == 1 ? FIELD_OFFSET(STORAGE_QUERY_DEPENDENT_VOLUME_RESPONSE, Lev1Depends[1]) :
                          FIELD_OFFSET(STORAGE_QUERY_DEPENDENT_VOLUME_RESPONSE, Lev2Depends[1]) +
                          Disk->HostVolume.Length + Disk->RelativePath.Length + Device.Length + 3 * sizeof(WCHAR);
    if (!(Flags & QUERY_DEPENDENT_VOLUME_REQUEST_FLAG_HOST_VOLUMES))
        Length = FIELD_OFFSET(STORAGE_QUERY_DEPENDENT_VOLUME_RESPONSE, Lev1Depends);
    if (Stack->Parameters.DeviceIoControl.OutputBufferLength < Length)
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(Response, Length);
    Response->ResponseLevel = Level;
    if (!(Flags & QUERY_DEPENDENT_VOLUME_REQUEST_FLAG_HOST_VOLUMES))
    {
        Irp->IoStatus.Information = Length;
        return STATUS_SUCCESS;
    }

    Response->NumberEntries = 1;
    Entry->EntryLength = Level == 1 ? sizeof(STORAGE_QUERY_DEPENDENT_VOLUME_LEV1_ENTRY) :
                                      sizeof(STORAGE_QUERY_DEPENDENT_VOLUME_LEV2_ENTRY);
    Entry->DependencyTypeFlags = DEPENDENT_DISK_FLAG_SYSTEM_VOLUME | DEPENDENT_DISK_FLAG_REMOVABLE |
                                 DEPENDENT_DISK_FLAG_PERMANENT_LIFETIME;
    Entry->VirtualStorageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    Entry->VirtualStorageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    if (Level == 2)
    {
        Length = FIELD_OFFSET(STORAGE_QUERY_DEPENDENT_VOLUME_RESPONSE, Lev2Depends[1]);
        VhdAppendName(Response, &Length, &Disk->HostVolume, &Entry->HostVolumeNameOffset, &Entry->HostVolumeNameSize);
        VhdAppendName(Response, &Length, &Disk->RelativePath, &Entry->RelativePathOffset, &Entry->RelativePathSize);
        VhdAppendName(Response, &Length, &Device, &Entry->DependentDeviceNameOffset, &Entry->DependentDeviceNameSize);
    }

    Irp->IoStatus.Information = Length;
    return STATUS_SUCCESS;
}

NTSTATUS
VhdPdoDeviceControl(
    _In_ PVHD_PDO_EXTENSION Extension,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PSCSI_ADDRESS Address = Irp->AssociatedIrp.SystemBuffer;

    Irp->IoStatus.Information = 0;
    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_STORAGE_QUERY_PROPERTY:
            return VhdQueryProperty(Extension, Irp);

        case FSCTL_QUERY_DEPENDENT_VOLUME:
            return VhdQueryDependentVolume(Extension, Irp);

        case IOCTL_SCSI_GET_ADDRESS:
            if (Stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(SCSI_ADDRESS))
                return STATUS_BUFFER_TOO_SMALL;

            RtlZeroMemory(Address, sizeof(*Address));
            Address->Length = sizeof(*Address);
            Address->Lun = VHD_LUN;
            Irp->IoStatus.Information = sizeof(*Address);
            return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
