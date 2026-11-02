/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for the boot disk of a system started from a VHDX file
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include <apitest.h>
#include <winreg.h>
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

START_TEST(VhdBoot)
{
    UCHAR Buffer[512];
    PSTORAGE_ADAPTER_DESCRIPTOR Adapter = (PSTORAGE_ADAPTER_DESCRIPTOR)Buffer;
    PSTORAGE_DEVICE_DESCRIPTOR Device = (PSTORAGE_DEVICE_DESCRIPTOR)Buffer;
    VOLUME_DISK_EXTENTS Extents;
    DISK_GEOMETRY_EX Geometry;
    SYSTEM_POWER_CAPABILITIES Power;
    SCSI_ADDRESS Address;
    CHAR Path[MAX_PATH], Text[64], BootDevice[MAX_PATH];
    DWORD Bytes, Size = sizeof(BootDevice);
    HANDLE Volume, Disk;
    LONG Error;

    GetSystemWindowsDirectoryA(Path, sizeof(Path));
    sprintf(Path, "\\\\.\\%c:", Path[0]);
    Volume = CreateFileA(Path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(Volume != INVALID_HANDLE_VALUE, "Cannot open %s: %lu\n", Path, GetLastError());
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

    ok(GetPwrCapabilities(&Power), "GetPwrCapabilities failed: %lu\n", GetLastError());
    ok(!Power.SystemS4, "Hibernation is available\n");
    ok(!Power.HiberFilePresent, "A hibernation file is present\n");
}
