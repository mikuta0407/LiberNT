/*
 * PROJECT:     LiberNT VHD Miniport
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Virtual disk driver for booting from a VHDX file
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include <initguid.h>
#include "vhdmp.h"

#define NDEBUG
#include <debug.h>

#define VHD_BOOT_OPTION     L"VHDBOOT="
#define VHD_HOST_TRIES      50
#define VHD_HOST_WAIT_MS    200
#define VHD_PATH_CHARS      260

static const WCHAR VhdDeviceId[] = L"SCSI\\Disk&Ven_Msft&Prod_Virtual_Disk";
static const WCHAR VhdHardwareIds[] =
    L"SCSI\\DiskMsft____Virtual_Disk____1.0_\0"
    L"SCSI\\DiskMsft____Virtual_Disk____\0"
    L"SCSI\\DiskMsft____\0"
    L"SCSI\\Msft____Virtual_Disk____1\0"
    L"Msft____Virtual_Disk____1\0"
    L"GenDisk\0";
static const WCHAR VhdCompatibleIds[] = L"SCSI\\Disk\0SCSI\\RAW\0";
static const WCHAR VhdInstanceId[] = L"000001";
static const WCHAR VhdDescription[] = L"Msft Virtual Disk";
static const WCHAR VhdLocation[] = L"Bus Number 0, Target Id 0, LUN 1";

static
NTSTATUS
VhdCompleteIrp(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS
VhdDeviceIoControl(
    _In_ PDEVICE_OBJECT Device,
    _In_ ULONG IoControlCode,
    _Out_writes_bytes_(Length) PVOID Output,
    _In_ ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IoControlCode, Device, NULL, 0, Output, Length, FALSE, &Event, &IoStatus);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    return Status;
}

static
BOOLEAN
VhdParseHex(
    _Inout_ PCWSTR* Text,
    _In_ ULONG MaxDigits,
    _Out_ PULONGLONG Value)
{
    ULONG Digits = 0, Digit;
    WCHAR Char;

    *Value = 0;
    while (Digits < MaxDigits)
    {
        Char = **Text;
        if (Char >= L'0' && Char <= L'9')
            Digit = Char - L'0';
        else if (Char >= L'A' && Char <= L'F')
            Digit = Char - L'A' + 10;
        else if (Char >= L'a' && Char <= L'f')
            Digit = Char - L'a' + 10;
        else
            break;

        *Value = *Value * 16 + Digit;
        (*Text)++;
        Digits++;
    }

    return Digits != 0;
}

static
NTSTATUS
VhdReadBootOption(
    _Out_ PULONG Signature,
    _Out_ GUID* DiskId,
    _Out_ PBOOLEAN IsGpt,
    _Out_ PULONGLONG Offset,
    _Out_writes_z_(PathChars) PWSTR Path,
    _In_ ULONG PathChars)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control");
    UNICODE_STRING Value = RTL_CONSTANT_STRING(L"SystemStartOptions");
    UNICODE_STRING Guid;
    OBJECT_ATTRIBUTES Attributes;
    PKEY_VALUE_PARTIAL_INFORMATION Information;
    ULONGLONG Number;
    PCWSTR Text, End;
    ULONG Length, Chars = 0;
    HANDLE Key;
    NTSTATUS Status;

    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&Key, KEY_QUERY_VALUE, &Attributes);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ZwQueryValueKey(Key, &Value, KeyValuePartialInformation, NULL, 0, &Length);
    if (Status != STATUS_BUFFER_TOO_SMALL && Status != STATUS_BUFFER_OVERFLOW)
    {
        ZwClose(Key);
        return NT_SUCCESS(Status) ? STATUS_NOT_FOUND : Status;
    }

    Information = ExAllocatePoolWithTag(PagedPool, Length + sizeof(WCHAR), VHD_TAG);
    if (!Information)
    {
        ZwClose(Key);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ZwQueryValueKey(Key, &Value, KeyValuePartialInformation, Information, Length, &Length);
    ZwClose(Key);
    if (!NT_SUCCESS(Status) || Information->Type != REG_SZ)
    {
        ExFreePoolWithTag(Information, VHD_TAG);
        return NT_SUCCESS(Status) ? STATUS_NOT_FOUND : Status;
    }

    ((PWCHAR)Information->Data)[Information->DataLength / sizeof(WCHAR)] = UNICODE_NULL;
    Text = wcsstr((PCWSTR)Information->Data, VHD_BOOT_OPTION);
    Status = STATUS_NOT_FOUND;
    if (!Text)
        goto Done;

    Text += RTL_NUMBER_OF(VHD_BOOT_OPTION) - 1;
    Status = STATUS_INVALID_PARAMETER;
    *IsGpt = *Text == L'{';
    if (*IsGpt)
    {
        End = wcschr(Text, L'}');
        if (!End)
            goto Done;

        Guid.Buffer = (PWSTR)Text;
        Guid.Length = Guid.MaximumLength = (USHORT)((End - Text + 1) * sizeof(WCHAR));
        if (!NT_SUCCESS(RtlGUIDFromString(&Guid, DiskId)))
            goto Done;
        Text = End + 1;
    }
    else
    {
        if (!VhdParseHex(&Text, 8, &Number))
            goto Done;
        *Signature = (ULONG)Number;
    }

    if (*Text++ != L':' || !VhdParseHex(&Text, 16, Offset) || *Text++ != L':')
        goto Done;

    while (*Text != UNICODE_NULL && *Text != L' ' && Chars < PathChars - 1)
    {
        if (*Text == L'%')
        {
            Text++;
            if (!VhdParseHex(&Text, 2, &Number))
                goto Done;
            Path[Chars++] = (WCHAR)Number;
        }
        else
        {
            Path[Chars++] = *Text++;
        }
    }
    Path[Chars] = UNICODE_NULL;
    Status = Chars != 0 && Path[0] == L'\\' ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;

Done:
    ExFreePoolWithTag(Information, VHD_TAG);
    return Status;
}

static
BOOLEAN
VhdMatchDisk(
    _In_ PDEVICE_OBJECT Device,
    _In_ ULONG Signature,
    _In_ const GUID* DiskId,
    _In_ BOOLEAN IsGpt,
    _In_ ULONGLONG Offset,
    _Out_ PULONG DiskNumber,
    _Out_ PULONG PartitionNumber)
{
    STORAGE_DEVICE_NUMBER Number;
    PDRIVE_LAYOUT_INFORMATION_EX Layout;
    ULONG Length = FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry[128]), i;
    BOOLEAN Found = FALSE;

    if (!NT_SUCCESS(VhdDeviceIoControl(Device, IOCTL_STORAGE_GET_DEVICE_NUMBER, &Number, sizeof(Number))))
        return FALSE;

    Layout = ExAllocatePoolWithTag(NonPagedPoolNx, Length, VHD_TAG);
    if (!Layout)
        return FALSE;

    if (NT_SUCCESS(VhdDeviceIoControl(Device, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, Layout, Length)) &&
        (IsGpt ? Layout->PartitionStyle == PARTITION_STYLE_GPT && IsEqualGUID(&Layout->Gpt.DiskId, DiskId) :
                 Layout->PartitionStyle == PARTITION_STYLE_MBR && Layout->Mbr.Signature == Signature))
    {
        for (i = 0; i < Layout->PartitionCount && !Found; i++)
        {
            if (Layout->PartitionEntry[i].PartitionNumber != 0 &&
                (ULONGLONG)Layout->PartitionEntry[i].StartingOffset.QuadPart == Offset)
            {
                *DiskNumber = Number.DeviceNumber;
                *PartitionNumber = Layout->PartitionEntry[i].PartitionNumber;
                Found = TRUE;
            }
        }
    }

    ExFreePoolWithTag(Layout, VHD_TAG);
    return Found;
}

static
NTSTATUS
VhdFindHost(
    _Out_ PUNICODE_STRING HostPath)
{
    WCHAR Path[VHD_PATH_CHARS];
    UNICODE_STRING Name;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT Device;
    ULONG Signature = 0, DiskNumber, PartitionNumber;
    ULONGLONG Offset;
    GUID DiskId;
    BOOLEAN IsGpt, Found = FALSE;
    PWSTR List, Link;
    NTSTATUS Status;

    Status = VhdReadBootOption(&Signature, &DiskId, &IsGpt, &Offset, Path, RTL_NUMBER_OF(Path));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_DISK, NULL, 0, &List);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Link = List; *Link != UNICODE_NULL && !Found; Link += wcslen(Link) + 1)
    {
        RtlInitUnicodeString(&Name, Link);
        if (!NT_SUCCESS(IoGetDeviceObjectPointer(&Name, FILE_READ_ATTRIBUTES, &FileObject, &Device)))
            continue;

        Found = VhdMatchDisk(Device, Signature, &DiskId, IsGpt, Offset, &DiskNumber, &PartitionNumber);
        ObDereferenceObject(FileObject);
    }

    ExFreePool(List);
    if (!Found)
        return STATUS_NO_SUCH_DEVICE;

    HostPath->MaximumLength = (USHORT)(sizeof(L"\\Device\\Harddisk4294967295\\Partition4294967295") + wcslen(Path) * sizeof(WCHAR));
    HostPath->Buffer = ExAllocatePoolWithTag(PagedPool, HostPath->MaximumLength, VHD_TAG);
    if (!HostPath->Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    HostPath->Length = 0;
    return RtlUnicodeStringPrintf(HostPath, L"\\Device\\Harddisk%lu\\Partition%lu%ls", DiskNumber, PartitionNumber, Path);
}

static
VOID
NTAPI
VhdWorker(
    _In_ PVOID Context)
{
    PVHD_PDO_EXTENSION Extension = Context;
    PLIST_ENTRY Entry;
    PIRP Irp;

    for (;;)
    {
        KeWaitForSingleObject(&Extension->QueueEvent, Executive, KernelMode, FALSE, NULL);
        while ((Entry = ExInterlockedRemoveHeadList(&Extension->Queue, &Extension->QueueLock)) != NULL)
        {
            Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
            VhdExecuteSrb(Extension, Irp);
            IoCompleteRequest(Irp, IO_DISK_INCREMENT);
        }
    }
}

static
NTSTATUS
VhdCreateChild(
    _In_ PVHD_FDO_EXTENSION Fdo)
{
    PVHD_PDO_EXTENSION Extension;
    PDEVICE_OBJECT Pdo;
    HANDLE Thread;
    NTSTATUS Status;

    Status = IoCreateDevice(Fdo->Common.Self->DriverObject,
                            sizeof(VHD_PDO_EXTENSION),
                            NULL,
                            FILE_DEVICE_DISK,
                            FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &Pdo);
    if (!NT_SUCCESS(Status))
        return Status;

    Extension = Pdo->DeviceExtension;
    RtlZeroMemory(Extension, sizeof(*Extension));
    Extension->Common.Self = Pdo;
    Extension->Fdo = Fdo->Common.Self;
    InitializeListHead(&Extension->Queue);
    KeInitializeSpinLock(&Extension->QueueLock);
    KeInitializeEvent(&Extension->QueueEvent, SynchronizationEvent, FALSE);

    Status = VhdOpenDisk(&Extension->Disk, &Fdo->HostPath);
    if (NT_SUCCESS(Status))
        Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, VhdWorker, Extension);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(Pdo);
        return Status;
    }

    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID*)&Extension->Worker, NULL);
    ZwClose(Thread);

    Status = PoDisableSleepStates(PoSleepDisableReasonVhdBoot, PO_REASON_STATE_S4, &Extension->SleepToken);
    if (!NT_SUCCESS(Status))
        DPRINT1("VHDMP: hibernation could not be disabled: 0x%08lx\n", Status);

    Pdo->Flags |= DO_DIRECT_IO | DO_POWER_PAGABLE;
    Pdo->AlignmentRequirement = FILE_WORD_ALIGNMENT;
    Pdo->Flags &= ~DO_DEVICE_INITIALIZING;
    Extension->Present = TRUE;
    Fdo->Child = Pdo;

    DPRINT1("VHDMP: %wZ attached, %I64u bytes\n", &Fdo->HostPath, Extension->Disk.Image.VirtualSize);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
VhdReinitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_opt_ PVOID Context,
    _In_ ULONG Count)
{
    PVHD_FDO_EXTENSION Fdo = ((PDEVICE_OBJECT)Context)->DeviceExtension;
    LARGE_INTEGER Wait;
    NTSTATUS Status;
    ULONG Try;

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(Count);

    Wait.QuadPart = -10000LL * VHD_HOST_WAIT_MS;
    for (Try = 0; Try < VHD_HOST_TRIES; Try++)
    {
        if (Try != 0)
            KeDelayExecutionThread(KernelMode, FALSE, &Wait);

        /* The host disk and its partitions are still being enumerated while boot drivers reinitialize. */
        Status = VhdFindHost(&Fdo->HostPath);
        if (NT_SUCCESS(Status))
        {
            Status = VhdCreateChild(Fdo);
            if (!NT_SUCCESS(Status))
            {
                ExFreePoolWithTag(Fdo->HostPath.Buffer, VHD_TAG);
                RtlInitEmptyUnicodeString(&Fdo->HostPath, NULL, 0);
            }
        }

        if (Status != STATUS_NO_SUCH_DEVICE &&
            Status != STATUS_OBJECT_PATH_NOT_FOUND &&
            Status != STATUS_OBJECT_NAME_NOT_FOUND)
        {
            break;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("VHDMP: the boot virtual disk could not be attached: 0x%08lx\n", Status);
        return;
    }

    IoInvalidateDeviceRelations(Fdo->Pdo, BusRelations);
}

static
NTSTATUS
NTAPI
VhdSignalCompletion(
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
VhdForwardAndWait(
    _In_ PVHD_FDO_EXTENSION Fdo,
    _Inout_ PIRP Irp)
{
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, VhdSignalCompletion, &Event, TRUE, TRUE, TRUE);
    if (IoCallDriver(Fdo->Lower, Irp) == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    return Irp->IoStatus.Status;
}

static
NTSTATUS
VhdFdoPnp(
    _In_ PVHD_FDO_EXTENSION Fdo,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_RELATIONS Old, New;
    ULONG Count;
    NTSTATUS Status;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = VhdForwardAndWait(Fdo, Irp);
            if (Status == STATUS_NOT_SUPPORTED)
                Status = STATUS_SUCCESS;
            return VhdCompleteIrp(Irp, Status);

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (Stack->Parameters.QueryDeviceRelations.Type != BusRelations || !Fdo->Child)
                break;

            Old = (PDEVICE_RELATIONS)Irp->IoStatus.Information;
            Count = Old ? Old->Count : 0;
            New = ExAllocatePoolWithTag(PagedPool, FIELD_OFFSET(DEVICE_RELATIONS, Objects[Count + 1]), VHD_TAG);
            if (!New)
                return VhdCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);

            if (Old)
            {
                RtlCopyMemory(New->Objects, Old->Objects, Count * sizeof(PDEVICE_OBJECT));
                ExFreePool(Old);
            }
            New->Objects[Count] = Fdo->Child;
            New->Count = Count + 1;
            ObReferenceObject(Fdo->Child);
            Irp->IoStatus.Information = (ULONG_PTR)New;
            Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_QUERY_REMOVE_DEVICE:
            if (Fdo->Child)
                return VhdCompleteIrp(Irp, STATUS_UNSUCCESSFUL);
            break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Fdo->Lower, Irp);
}

static
PWSTR
VhdDuplicate(
    _In_reads_bytes_(Size) const WCHAR* Text,
    _In_ SIZE_T Size)
{
    PWSTR Copy = ExAllocatePoolWithTag(PagedPool, Size, VHD_TAG);

    if (Copy)
        RtlCopyMemory(Copy, Text, Size);
    return Copy;
}

static
NTSTATUS
VhdPdoPnp(
    _In_ PVHD_PDO_EXTENSION Pdo,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_CAPABILITIES Capabilities;
    PDEVICE_RELATIONS Relations;
    NTSTATUS Status = Irp->IoStatus.Status;
    PWSTR Text = NULL;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        case IRP_MN_STOP_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_REMOVE_DEVICE:
            Status = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_QUERY_REMOVE_DEVICE:
            Status = STATUS_UNSUCCESSFUL;
            break;

        case IRP_MN_QUERY_ID:
            switch (Stack->Parameters.QueryId.IdType)
            {
                case BusQueryDeviceID:
                    Text = VhdDuplicate(VhdDeviceId, sizeof(VhdDeviceId));
                    break;
                case BusQueryHardwareIDs:
                    Text = VhdDuplicate(VhdHardwareIds, sizeof(VhdHardwareIds));
                    break;
                case BusQueryCompatibleIDs:
                    Text = VhdDuplicate(VhdCompatibleIds, sizeof(VhdCompatibleIds));
                    break;
                case BusQueryInstanceID:
                    Text = VhdDuplicate(VhdInstanceId, sizeof(VhdInstanceId));
                    break;
                default:
                    goto Complete;
            }
            Status = Text ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = (ULONG_PTR)Text;
            break;

        case IRP_MN_QUERY_DEVICE_TEXT:
            if (Stack->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription)
                Text = VhdDuplicate(VhdDescription, sizeof(VhdDescription));
            else if (Stack->Parameters.QueryDeviceText.DeviceTextType == DeviceTextLocationInformation)
                Text = VhdDuplicate(VhdLocation, sizeof(VhdLocation));
            else
                break;
            Status = Text ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = (ULONG_PTR)Text;
            break;

        case IRP_MN_QUERY_CAPABILITIES:
            Capabilities = Stack->Parameters.DeviceCapabilities.Capabilities;
            Capabilities->UniqueID = FALSE;
            Capabilities->SilentInstall = TRUE;
            Capabilities->Removable = FALSE;
            Capabilities->SurpriseRemovalOK = FALSE;
            Capabilities->Address = VHD_LUN;
            Capabilities->UINumber = 0;
            Status = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (Stack->Parameters.QueryDeviceRelations.Type != TargetDeviceRelation)
                break;

            Relations = ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS), VHD_TAG);
            if (!Relations)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            Relations->Count = 1;
            Relations->Objects[0] = Pdo->Common.Self;
            ObReferenceObject(Pdo->Common.Self);
            Irp->IoStatus.Information = (ULONG_PTR)Relations;
            Status = STATUS_SUCCESS;
            break;

        case IRP_MN_DEVICE_USAGE_NOTIFICATION:
            Status = Stack->Parameters.UsageNotification.Type == DeviceUsageTypePaging ? STATUS_SUCCESS :
                                                                                         STATUS_NOT_SUPPORTED;
            break;
    }

Complete:
    return VhdCompleteIrp(Irp, Status);
}

static
NTSTATUS
NTAPI
VhdDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVHD_EXTENSION Extension = DeviceObject->DeviceExtension;

    return Extension->IsFdo ? VhdFdoPnp((PVHD_FDO_EXTENSION)Extension, Irp) :
                              VhdPdoPnp((PVHD_PDO_EXTENSION)Extension, Irp);
}

static
NTSTATUS
NTAPI
VhdDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVHD_EXTENSION Extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = Irp->IoStatus.Status;

    PoStartNextPowerIrp(Irp);
    if (Extension->IsFdo)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(((PVHD_FDO_EXTENSION)Extension)->Lower, Irp);
    }

    if (Stack->MinorFunction == IRP_MN_SET_POWER || Stack->MinorFunction == IRP_MN_QUERY_POWER)
        Status = STATUS_SUCCESS;
    return VhdCompleteIrp(Irp, Status);
}

static
NTSTATUS
NTAPI
VhdDispatchScsi(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVHD_PDO_EXTENSION Extension = DeviceObject->DeviceExtension;

    if (Extension->Common.IsFdo || !Extension->Present)
        return VhdCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);

    IoMarkIrpPending(Irp);
    ExInterlockedInsertTailList(&Extension->Queue, &Irp->Tail.Overlay.ListEntry, &Extension->QueueLock);
    KeSetEvent(&Extension->QueueEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_PENDING;
}

static
NTSTATUS
NTAPI
VhdDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVHD_EXTENSION Extension = DeviceObject->DeviceExtension;

    if (Extension->IsFdo)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(((PVHD_FDO_EXTENSION)Extension)->Lower, Irp);
    }

    return VhdCompleteIrp(Irp, VhdPdoDeviceControl((PVHD_PDO_EXTENSION)Extension, Irp));
}

static
NTSTATUS
NTAPI
VhdDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Information = 0;
    return VhdCompleteIrp(Irp, STATUS_SUCCESS);
}

static
NTSTATUS
NTAPI
VhdDispatchSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PVHD_EXTENSION Extension = DeviceObject->DeviceExtension;

    if (Extension->IsFdo)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(((PVHD_FDO_EXTENSION)Extension)->Lower, Irp);
    }

    return VhdCompleteIrp(Irp, Irp->IoStatus.Status);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    PVHD_FDO_EXTENSION Fdo;
    PDEVICE_OBJECT Pdo = NULL, Device;
    ULONG Signature;
    ULONGLONG Offset;
    GUID DiskId;
    BOOLEAN IsGpt;
    WCHAR Buffer[VHD_PATH_CHARS];
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->MajorFunction[IRP_MJ_CREATE] = VhdDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = VhdDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = VhdDispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_SCSI] = VhdDispatchScsi;
    DriverObject->MajorFunction[IRP_MJ_PNP] = VhdDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = VhdDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = VhdDispatchSystemControl;

    /* TODO: attaching virtual disks other than the boot disk (virtdisk.dll, diskpart vdisk) */
    if (!NT_SUCCESS(VhdReadBootOption(&Signature, &DiskId, &IsGpt, &Offset, Buffer, RTL_NUMBER_OF(Buffer))))
        return STATUS_SUCCESS;

    Status = IoReportDetectedDevice(DriverObject, InterfaceTypeUndefined, -1, -1, NULL, NULL, FALSE, &Pdo);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IoCreateDevice(DriverObject, sizeof(VHD_FDO_EXTENSION), NULL, FILE_DEVICE_BUS_EXTENDER,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &Device);
    if (!NT_SUCCESS(Status))
        return Status;

    Fdo = Device->DeviceExtension;
    RtlZeroMemory(Fdo, sizeof(*Fdo));
    Fdo->Common.IsFdo = TRUE;
    Fdo->Common.Self = Device;
    Fdo->Pdo = Pdo;
    Fdo->Lower = IoAttachDeviceToDeviceStack(Device, Pdo);
    if (!Fdo->Lower)
    {
        IoDeleteDevice(Device);
        return STATUS_NO_SUCH_DEVICE;
    }

    Device->Flags |= DO_POWER_PAGABLE;
    Device->Flags &= ~DO_DEVICE_INITIALIZING;

    IoRegisterBootDriverReinitialization(DriverObject, VhdReinitialize, Device);
    return STATUS_SUCCESS;
}
