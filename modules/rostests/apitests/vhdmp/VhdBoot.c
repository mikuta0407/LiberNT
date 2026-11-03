/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the boot disk of a system started from a VHDX file
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include <apitest.h>
#include <ntstatus.h>
#include <winreg.h>
#include <ndk/exfuncs.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <ntddscsi.h>
#include <powrprof.h>
#include <stdio.h>

#define BOOT_DEVICE_PREFIX "multi(0)disk(0)vdisk(0)partition("

static BOOL
QueryProperty(HANDLE Disk, STORAGE_PROPERTY_ID Id, PVOID Buffer, DWORD Size)
{
    STORAGE_PROPERTY_QUERY Query = {0};
    DWORD Bytes;

    Query.PropertyId = Id;
    Query.QueryType = PropertyStandardQuery;
    return DeviceIoControl(Disk, IOCTL_STORAGE_QUERY_PROPERTY, &Query, sizeof(Query), Buffer, Size, &Bytes, NULL);
}

static VOID
GetDescriptorString(PSTORAGE_DEVICE_DESCRIPTOR Device, DWORD Offset, PSTR Text, DWORD Size)
{
    DWORD Length;

    Text[0] = ANSI_NULL;
    if (Offset == 0 || Offset >= Device->Size)
        return;

    lstrcpynA(Text, (PCSTR)Device + Offset, Size);
    for (Length = lstrlenA(Text); Length > 0 && Text[Length - 1] == ' '; Length--)
        Text[Length - 1] = ANSI_NULL;
}

static const GUID VendorMicrosoft = {0xec984aec, 0xa0f9, 0x47e9, {0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b}};

static PCWSTR
EntryString(PSTORAGE_QUERY_DEPENDENT_VOLUME_RESPONSE Response, DWORD Bytes, DWORD Offset, DWORD Size, PWSTR Text, DWORD Chars)
{
    Text[0] = UNICODE_NULL;
    if (Offset <= Bytes && Size <= Bytes - Offset && Size / sizeof(WCHAR) < Chars)
    {
        memcpy(Text, (PUCHAR)Response + Offset, Size);
        Text[Size / sizeof(WCHAR)] = UNICODE_NULL;
    }
    return Text;
}

static VOID
TestDependentVolume(CHAR Drive, DWORD DiskNumber, ULONGLONG DiskSize, PWSTR Host, DWORD HostChars)
{
    WIN32_FILE_ATTRIBUTE_DATA Attributes;
    ULONGLONG FileSize;
    STORAGE_QUERY_DEPENDENT_VOLUME_REQUEST Request = {2, QUERY_DEPENDENT_VOLUME_REQUEST_FLAG_HOST_VOLUMES};
    PSTORAGE_QUERY_DEPENDENT_VOLUME_RESPONSE Response;
    PSTORAGE_QUERY_DEPENDENT_VOLUME_LEV2_ENTRY Entry;
    WCHAR Path[MAX_PATH], Text[MAX_PATH], Expected[MAX_PATH];
    UCHAR Buffer[4096];
    CHAR Name[16];
    HANDLE Handle;
    DWORD Bytes;
    BOOL Result;

    Response = (PSTORAGE_QUERY_DEPENDENT_VOLUME_RESPONSE)Buffer;
    sprintf(Name, "\\\\.\\%c:", Drive);
    Handle = CreateFileA(Name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Cannot open %s: %lu\n", Name, GetLastError());
    if (Handle != INVALID_HANDLE_VALUE)
    {
        SetLastError(0xdeadbeef);
        Result = DeviceIoControl(Handle, FSCTL_QUERY_DEPENDENT_VOLUME, &Request, sizeof(Request),
                                 Buffer, sizeof(Buffer), &Bytes, NULL);
        ok(!Result && GetLastError() == ERROR_INVALID_FUNCTION,
           "FSCTL_QUERY_DEPENDENT_VOLUME on the volume returned %d, error %lu\n", Result, GetLastError());
        CloseHandle(Handle);
    }

    sprintf(Name, "%c:\\", Drive);
    Handle = CreateFileA(Name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ok(Handle != INVALID_HANDLE_VALUE, "Cannot open %s: %lu\n", Name, GetLastError());
    if (Handle == INVALID_HANDLE_VALUE)
        return;

    Result = DeviceIoControl(Handle, FSCTL_QUERY_DEPENDENT_VOLUME, &Request, sizeof(Request),
                             Buffer, sizeof(Buffer), &Bytes, NULL);
    ok(Result, "FSCTL_QUERY_DEPENDENT_VOLUME failed: %lu\n", GetLastError());
    CloseHandle(Handle);
    if (!Result)
        return;

    ok(Response->ResponseLevel == 2, "Response level %lu\n", Response->ResponseLevel);
    ok(Response->NumberEntries == 1, "%lu entries\n", Response->NumberEntries);
    if (Response->NumberEntries == 0)
        return;

    Entry = &Response->Lev2Depends[0];
    ok(Entry->EntryLength == sizeof(*Entry), "Entry length %lu\n", Entry->EntryLength);
    ok(Entry->DependencyTypeFlags == 0x450, "Dependency flags 0x%lx\n", Entry->DependencyTypeFlags);
    ok(Entry->ProviderSpecificFlags == 0, "Provider flags 0x%lx\n", Entry->ProviderSpecificFlags);
    ok(Entry->VirtualStorageType.DeviceId == 3, "Device type %lu\n", Entry->VirtualStorageType.DeviceId);
    ok(IsEqualGUID(&Entry->VirtualStorageType.VendorId, &VendorMicrosoft),
       "Vendor %s\n", wine_dbgstr_guid(&Entry->VirtualStorageType.VendorId));
    ok(Entry->AncestorLevel == 0, "Ancestor level %lu\n", Entry->AncestorLevel);

    EntryString(Response, Bytes, Entry->HostVolumeNameOffset, Entry->HostVolumeNameSize, Host, HostChars);
    ok(!wcsncmp(Host, L"\\Device\\HarddiskVolume", 22), "Host volume %s\n", wine_dbgstr_w(Host));
    EntryString(Response, Bytes, Entry->RelativePathOffset, Entry->RelativePathSize, Text, MAX_PATH);
    ok(Text[0] == L'\\', "Relative path %s\n", wine_dbgstr_w(Text));
    _snwprintf(Path, MAX_PATH, L"\\\\?\\GLOBALROOT%s%s", Host, Text);
    ok(GetFileAttributesExW(Path, GetFileExInfoStandard, &Attributes), "Cannot query %s: %lu\n", wine_dbgstr_w(Path), GetLastError());
    FileSize = ((ULONGLONG)Attributes.nFileSizeHigh << 32) | Attributes.nFileSizeLow;
    ok(FileSize >= DiskSize && FileSize <= DiskSize + 64 * 1024 * 1024,
       "The virtual disk file holds %I64u bytes for a %I64u-byte disk\n", FileSize, DiskSize);

    _snwprintf(Path, MAX_PATH, L"%c:", Drive);
    QueryDosDeviceW(Path, Expected, MAX_PATH);
    EntryString(Response, Bytes, Entry->DependentVolumeNameOffset, Entry->DependentVolumeNameSize, Text, MAX_PATH);
    ok(!wcscmp(Text, Expected), "Dependent volume %s, expected %s\n", wine_dbgstr_w(Text), wine_dbgstr_w(Expected));

    _snwprintf(Expected, MAX_PATH, L"\\??\\PhysicalDrive%lu", DiskNumber);
    EntryString(Response, Bytes, Entry->DependentDeviceNameOffset, Entry->DependentDeviceNameSize, Text, MAX_PATH);
    ok(!wcscmp(Text, Expected), "Dependent device %s, expected %s\n", wine_dbgstr_w(Text), wine_dbgstr_w(Expected));
}

static VOID
TestPagingFile(PCWSTR Host)
{
    UCHAR Buffer[1024];
    PSYSTEM_PAGEFILE_INFORMATION Information = (PSYSTEM_PAGEFILE_INFORMATION)Buffer;
    WCHAR Drive[3] = L"?:", Device[MAX_PATH], Name[MAX_PATH];
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemPageFileInformation, Buffer, sizeof(Buffer), NULL);
    ok(Status == STATUS_SUCCESS, "SystemPageFileInformation: 0x%lx\n", Status);
    if (Status != STATUS_SUCCESS || Information->TotalSize == 0)
    {
        ok(0, "No paging file is in use\n");
        return;
    }

    lstrcpynW(Name, Information->PageFileName.Buffer, min(MAX_PATH, Information->PageFileName.Length / sizeof(WCHAR) + 1));
    ok(!wcsncmp(Name, L"\\??\\", 4), "Paging file %s\n", wine_dbgstr_w(Name));
    Drive[0] = Name[4];
    QueryDosDeviceW(Drive, Device, MAX_PATH);
    ok(!wcscmp(Device, Host), "The paging file %s is on %s, not on the host volume %s\n",
       wine_dbgstr_w(Name), wine_dbgstr_w(Device), wine_dbgstr_w(Host));
}

START_TEST(VhdBoot)
{
    UCHAR Buffer[512];
    PSTORAGE_ADAPTER_DESCRIPTOR Adapter = (PSTORAGE_ADAPTER_DESCRIPTOR)Buffer;
    PSTORAGE_DEVICE_DESCRIPTOR Device = (PSTORAGE_DEVICE_DESCRIPTOR)Buffer;
    VOLUME_DISK_EXTENTS Extents;
    DISK_GEOMETRY_EX Geometry;
    SYSTEM_POWER_CAPABILITIES Power;
    SCSI_ADDRESS Address;
    CHAR Path[MAX_PATH], VolumePath[16], Text[64], BootDevice[MAX_PATH];
    WCHAR Host[MAX_PATH] = L"";
    DWORD Bytes, Size = sizeof(BootDevice);
    HANDLE Volume, Disk;
    LONG Error;

    GetSystemWindowsDirectoryA(Path, sizeof(Path));
    sprintf(VolumePath, "\\\\.\\%c:", Path[0]);
    Volume = CreateFileA(VolumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(Volume != INVALID_HANDLE_VALUE, "Cannot open %s: %lu\n", VolumePath, GetLastError());
    if (Volume == INVALID_HANDLE_VALUE)
        return;

    ok(DeviceIoControl(Volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &Extents, sizeof(Extents), &Bytes, NULL),
       "IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS failed: %lu\n", GetLastError());
    CloseHandle(Volume);

    sprintf(Path, "\\\\.\\PhysicalDrive%lu", Extents.Extents[0].DiskNumber);
    Disk = CreateFileA(Path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(Disk != INVALID_HANDLE_VALUE, "Cannot open %s: %lu\n", Path, GetLastError());
    if (Disk == INVALID_HANDLE_VALUE)
        return;

    ok(QueryProperty(Disk, StorageAdapterProperty, Buffer, sizeof(Buffer)),
       "StorageAdapterProperty failed: %lu\n", GetLastError());
    if (Adapter->BusType != BusTypeFileBackedVirtual)
    {
        skip("The system disk is not a virtual disk (bus type %u)\n", Adapter->BusType);
        CloseHandle(Disk);
        return;
    }

    ok(QueryProperty(Disk, StorageDeviceProperty, Buffer, sizeof(Buffer)),
       "StorageDeviceProperty failed: %lu\n", GetLastError());
    ok(Device->BusType == BusTypeFileBackedVirtual, "Device bus type %u\n", Device->BusType);
    ok(Device->DeviceType == 0, "Device type %u\n", Device->DeviceType);
    ok(!Device->RemovableMedia, "The virtual disk is removable\n");
    GetDescriptorString(Device, Device->VendorIdOffset, Text, sizeof(Text));
    ok(!strcmp(Text, "Msft"), "Vendor '%s'\n", Text);
    GetDescriptorString(Device, Device->ProductIdOffset, Text, sizeof(Text));
    ok(!strcmp(Text, "Virtual Disk"), "Product '%s'\n", Text);
    GetDescriptorString(Device, Device->ProductRevisionOffset, Text, sizeof(Text));
    ok(!strcmp(Text, "1.0"), "Revision '%s'\n", Text);

    ok(DeviceIoControl(Disk, IOCTL_SCSI_GET_ADDRESS, NULL, 0, &Address, sizeof(Address), &Bytes, NULL),
       "IOCTL_SCSI_GET_ADDRESS failed: %lu\n", GetLastError());
    ok(Address.PathId == 0 && Address.TargetId == 0 && Address.Lun == 1,
       "SCSI address path %u target %u LUN %u\n", Address.PathId, Address.TargetId, Address.Lun);

    ok(DeviceIoControl(Disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &Geometry, sizeof(Geometry), &Bytes, NULL),
       "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX failed: %lu\n", GetLastError());
    ok(Geometry.Geometry.BytesPerSector == 512, "%lu bytes per sector\n", Geometry.Geometry.BytesPerSector);
    ok(Geometry.Geometry.MediaType == FixedMedia, "Media type %u\n", Geometry.Geometry.MediaType);
    CloseHandle(Disk);

    Error = RegGetValueA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control", "SystemBootDevice",
                         RRF_RT_REG_SZ, NULL, BootDevice, &Size);
    ok(Error == ERROR_SUCCESS, "SystemBootDevice: %ld\n", Error);
    ok(Error != ERROR_SUCCESS || !strncmp(BootDevice, BOOT_DEVICE_PREFIX, sizeof(BOOT_DEVICE_PREFIX) - 1),
       "SystemBootDevice is %s\n", BootDevice);

    TestDependentVolume(VolumePath[4], Extents.Extents[0].DiskNumber, Geometry.DiskSize.QuadPart, Host, MAX_PATH);
    TestPagingFile(Host);

    ok(GetPwrCapabilities(&Power), "GetPwrCapabilities failed: %lu\n", GetLastError());
    ok(!Power.SystemS4, "Hibernation is available\n");
    ok(!Power.HiberFilePresent, "A hibernation file is present\n");
}
