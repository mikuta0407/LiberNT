/*
 * PROJECT:     LiberNT VHD boot setup tool
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Make a LiberNT VHDX file bootable on a UEFI PC that runs Windows
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 *
 * Usage: lntvhdboot <path of the VHDX file> [/noorder]
 */

#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <virtdisk.h>
#include <rpc.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#include "vhdbcd.h"

#ifndef VARIABLE_ATTRIBUTE_NON_VOLATILE
#define VARIABLE_ATTRIBUTE_NON_VOLATILE       0x00000001
#define VARIABLE_ATTRIBUTE_BOOTSERVICE_ACCESS 0x00000002
#define VARIABLE_ATTRIBUTE_RUNTIME_ACCESS     0x00000004
#endif

#define BOOT_DIRECTORY      L"\\EFI\\LiberNT\\Boot"
#define BOOT_MANAGER        L"\\EFI\\LiberNT\\Boot\\bootmgfw.efi"
#define BOOT_STORE          L"\\EFI\\LiberNT\\Boot\\BCD"
#define BOOT_DESCRIPTION    L"LiberNT Boot Manager"
#define ENTRY_DESCRIPTION   L"LiberNT (VHD)"
#define BCD_BOOTMGR         L"{9dea862c-5cdd-4e70-acc1-f32b344d4795}"
#define BCD_TARGET_KEY      L"LNTVHDBOOT_BCD"
#define BCD_SOURCE_KEY      L"LNTVHDBOOT_SOURCE"
#define EFI_GLOBAL_VARIABLE L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}"
#define EFI_VARIABLE_FLAGS  (VARIABLE_ATTRIBUTE_NON_VOLATILE | VARIABLE_ATTRIBUTE_BOOTSERVICE_ACCESS | VARIABLE_ATTRIBUTE_RUNTIME_ACCESS)
#define LOAD_OPTION_ACTIVE  0x00000001
#define MAX_BOOT_OPTIONS    256
#define ATTACH_TRIES        50

typedef DWORD (WINAPI *POPEN_VIRTUAL_DISK)(PVIRTUAL_STORAGE_TYPE, PCWSTR, VIRTUAL_DISK_ACCESS_MASK,
                                           OPEN_VIRTUAL_DISK_FLAG, POPEN_VIRTUAL_DISK_PARAMETERS, PHANDLE);
typedef DWORD (WINAPI *PATTACH_VIRTUAL_DISK)(HANDLE, PSECURITY_DESCRIPTOR, ATTACH_VIRTUAL_DISK_FLAG, ULONG,
                                             PATTACH_VIRTUAL_DISK_PARAMETERS, LPOVERLAPPED);
typedef DWORD (WINAPI *PDETACH_VIRTUAL_DISK)(HANDLE, DETACH_VIRTUAL_DISK_FLAG, ULONG);
typedef DWORD (WINAPI *PGET_VIRTUAL_DISK_PHYSICAL_PATH)(HANDLE, PULONG, PWSTR);

typedef struct _PARTITION_LOCATION
{
    DWORD DiskNumber;
    DWORD PartitionNumber;
    BOOL IsGpt;
    DWORD Signature;
    GUID DiskId;
    GUID PartitionId;
    ULONGLONG Offset;
    ULONGLONG Length;
    DWORD SectorSize;
} PARTITION_LOCATION, *PPARTITION_LOCATION;

static
BOOL
Fail(
    _In_ PCWSTR Format,
    ...)
{
    va_list Arguments;

    va_start(Arguments, Format);
    fwprintf(stderr, L"lntvhdboot: ");
    vfwprintf(stderr, Format, Arguments);
    va_end(Arguments);
    return FALSE;
}

static
BOOL
EnablePrivilege(
    _In_ PCWSTR Name)
{
    TOKEN_PRIVILEGES Privileges;
    HANDLE Token;
    BOOL Result;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &Token))
        return FALSE;

    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    Result = LookupPrivilegeValueW(NULL, Name, &Privileges.Privileges[0].Luid) &&
             AdjustTokenPrivileges(Token, FALSE, &Privileges, 0, NULL, NULL) &&
             GetLastError() == ERROR_SUCCESS;
    CloseHandle(Token);
    return Result;
}

static
BOOL
DeviceControl(
    _In_ PCWSTR Path,
    _In_ DWORD Code,
    _Out_writes_bytes_(Size) PVOID Output,
    _In_ DWORD Size)
{
    HANDLE Handle;
    DWORD Bytes;
    BOOL Result;

    Handle = CreateFileW(Path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;

    Result = DeviceIoControl(Handle, Code, NULL, 0, Output, Size, &Bytes, NULL);
    CloseHandle(Handle);
    return Result;
}

static
BOOL
LocatePartition(
    _In_ PCWSTR Volume,
    _Out_ PPARTITION_LOCATION Location)
{
    BYTE Buffer[sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 127 * sizeof(PARTITION_INFORMATION_EX)];
    PDRIVE_LAYOUT_INFORMATION_EX Layout = (PDRIVE_LAYOUT_INFORMATION_EX)Buffer;
    VOLUME_DISK_EXTENTS Extents;
    DISK_GEOMETRY Geometry;
    WCHAR Disk[32];
    DWORD i;

    ZeroMemory(Location, sizeof(*Location));
    if (!DeviceControl(Volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, &Extents, sizeof(Extents)) ||
        Extents.NumberOfDiskExtents != 1)
    {
        return FALSE;
    }

    swprintf(Disk, 32, L"\\\\.\\PhysicalDrive%lu", Extents.Extents[0].DiskNumber);
    if (!DeviceControl(Disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, Layout, sizeof(Buffer)) ||
        !DeviceControl(Disk, IOCTL_DISK_GET_DRIVE_GEOMETRY, &Geometry, sizeof(Geometry)))
    {
        return FALSE;
    }

    Location->DiskNumber = Extents.Extents[0].DiskNumber;
    Location->Offset = (ULONGLONG)Extents.Extents[0].StartingOffset.QuadPart;
    Location->Length = (ULONGLONG)Extents.Extents[0].ExtentLength.QuadPart;
    Location->SectorSize = Geometry.BytesPerSector;
    Location->IsGpt = Layout->PartitionStyle == PARTITION_STYLE_GPT;
    if (Location->IsGpt)
        Location->DiskId = Layout->Gpt.DiskId;
    else if (Layout->PartitionStyle == PARTITION_STYLE_MBR)
        Location->Signature = Layout->Mbr.Signature;
    else
        return FALSE;

    for (i = 0; i < Layout->PartitionCount; i++)
    {
        if ((ULONGLONG)Layout->PartitionEntry[i].StartingOffset.QuadPart != Location->Offset)
            continue;

        Location->PartitionNumber = Layout->PartitionEntry[i].PartitionNumber;
        if (Location->IsGpt)
            Location->PartitionId = Layout->PartitionEntry[i].Gpt.PartitionId;
        return TRUE;
    }

    return FALSE;
}

static
BOOL
CopyBootFiles(
    _In_ PCWSTR VhdxPath,
    _In_ PCWSTR EspRoot,
    _In_ PCWSTR SourceStore,
    _Out_ PBOOL FreshStore)
{
    VIRTUAL_STORAGE_TYPE Type = {VIRTUAL_STORAGE_TYPE_DEVICE_VHDX};
    OPEN_VIRTUAL_DISK_PARAMETERS Open = {OPEN_VIRTUAL_DISK_VERSION_1};
    ATTACH_VIRTUAL_DISK_PARAMETERS Attach = {ATTACH_VIRTUAL_DISK_VERSION_1};
    POPEN_VIRTUAL_DISK OpenVirtualDiskFn;
    PATTACH_VIRTUAL_DISK AttachVirtualDiskFn;
    PDETACH_VIRTUAL_DISK DetachVirtualDiskFn;
    PGET_VIRTUAL_DISK_PHYSICAL_PATH GetPhysicalPathFn;
    WCHAR Physical[MAX_PATH], Source[MAX_PATH], Target[MAX_PATH];
    ULONG Size = sizeof(Physical);
    HMODULE Library;
    HANDLE Disk;
    DWORD Error, Number, Try;
    BOOL Result = FALSE;
    PCWSTR Digits;

    Library = LoadLibraryW(L"virtdisk.dll");
    if (!Library)
        return Fail(L"virtdisk.dll is not available (%lu).\n", GetLastError());

    OpenVirtualDiskFn = (POPEN_VIRTUAL_DISK)GetProcAddress(Library, "OpenVirtualDisk");
    AttachVirtualDiskFn = (PATTACH_VIRTUAL_DISK)GetProcAddress(Library, "AttachVirtualDisk");
    DetachVirtualDiskFn = (PDETACH_VIRTUAL_DISK)GetProcAddress(Library, "DetachVirtualDisk");
    GetPhysicalPathFn = (PGET_VIRTUAL_DISK_PHYSICAL_PATH)GetProcAddress(Library, "GetVirtualDiskPhysicalPath");
    if (!OpenVirtualDiskFn || !AttachVirtualDiskFn || !DetachVirtualDiskFn || !GetPhysicalPathFn)
    {
        FreeLibrary(Library);
        return Fail(L"virtdisk.dll lacks the attach functions.\n");
    }

    Type.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
    Error = OpenVirtualDiskFn(&Type, VhdxPath, VIRTUAL_DISK_ACCESS_ATTACH_RO | VIRTUAL_DISK_ACCESS_GET_INFO |
                              VIRTUAL_DISK_ACCESS_DETACH, OPEN_VIRTUAL_DISK_FLAG_NONE, &Open, &Disk);
    if (Error != ERROR_SUCCESS)
    {
        FreeLibrary(Library);
        return Fail(L"%ls cannot be opened as a VHDX file (%lu).\n", VhdxPath, Error);
    }

    Error = AttachVirtualDiskFn(Disk, NULL, ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY | ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER,
                                0, &Attach, NULL);
    if (Error != ERROR_SUCCESS)
    {
        Fail(L"%ls cannot be attached (%lu).\n", VhdxPath, Error);
        goto Close;
    }

    Error = GetPhysicalPathFn(Disk, &Size, Physical);
    Digits = wcspbrk(Physical, L"0123456789");
    if (Error != ERROR_SUCCESS || !Digits)
    {
        Fail(L"The attached disk of %ls was not found (%lu).\n", VhdxPath, Error);
        goto Detach;
    }

    Number = wcstoul(Digits, NULL, 10);
    swprintf(Source, MAX_PATH, L"\\\\?\\GLOBALROOT\\Device\\Harddisk%lu\\Partition1%ls", Number, BOOT_MANAGER);
    for (Try = 0; Try < ATTACH_TRIES && GetFileAttributesW(Source) == INVALID_FILE_ATTRIBUTES; Try++)
        Sleep(100);

    swprintf(Target, MAX_PATH, L"%ls\\EFI\\LiberNT", EspRoot);
    CreateDirectoryW(Target, NULL);
    swprintf(Target, MAX_PATH, L"%ls%ls", EspRoot, BOOT_DIRECTORY);
    CreateDirectoryW(Target, NULL);

    swprintf(Target, MAX_PATH, L"%ls%ls", EspRoot, BOOT_MANAGER);
    if (!CopyFileW(Source, Target, FALSE))
    {
        Fail(L"The boot manager could not be copied from %ls (%lu).\n", Source, GetLastError());
        goto Detach;
    }

    swprintf(Source, MAX_PATH, L"\\\\?\\GLOBALROOT\\Device\\Harddisk%lu\\Partition1%ls", Number, BOOT_STORE);
    swprintf(Target, MAX_PATH, L"%ls%ls", EspRoot, BOOT_STORE);
    *FreshStore = GetFileAttributesW(Target) == INVALID_FILE_ATTRIBUTES;
    if (!CopyFileW(Source, *FreshStore ? Target : SourceStore, FALSE))
    {
        Fail(L"The boot configuration could not be copied from %ls (%lu).\n", Source, GetLastError());
        goto Detach;
    }

    SetFileAttributesW(*FreshStore ? Target : SourceStore, FILE_ATTRIBUTE_NORMAL);
    Result = TRUE;

Detach:
    DetachVirtualDiskFn(Disk, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
Close:
    CloseHandle(Disk);
    FreeLibrary(Library);
    return Result;
}

static
BOOL
GetDefaultEntry(
    _In_ PCWSTR Store,
    _Out_writes_(39) PWSTR Id)
{
    WCHAR Key[MAX_PATH];
    DWORD Size = 39 * sizeof(WCHAR);

    swprintf(Key, MAX_PATH, L"%ls\\Objects\\%ls\\Elements\\23000003", Store, BCD_BOOTMGR);
    return RegGetValueW(HKEY_LOCAL_MACHINE, Key, L"Element", RRF_RT_REG_SZ, NULL, Id, &Size) == ERROR_SUCCESS;
}

static
BOOL
SetElement(
    _In_ PCWSTR Id,
    _In_ PCWSTR Element,
    _In_ DWORD Type,
    _In_reads_bytes_(Size) const VOID* Data,
    _In_ DWORD Size)
{
    WCHAR Path[MAX_PATH];
    HKEY Key;
    LONG Error;

    swprintf(Path, MAX_PATH, L"%ls\\Objects\\%ls\\Elements\\%ls", BCD_TARGET_KEY, Id, Element);
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, REG_OPTION_BACKUP_RESTORE, KEY_SET_VALUE, NULL, &Key, NULL);
    if (Error == ERROR_SUCCESS)
    {
        Error = RegSetValueExW(Key, L"Element", 0, Type, Data, Size);
        RegCloseKey(Key);
    }

    return Error == ERROR_SUCCESS ? TRUE : Fail(L"Element %ls of %ls could not be written (%ld).\n", Element, Id, Error);
}

static
BOOL
AddEntry(
    _In_ PCWSTR SourceStore,
    _Out_writes_(39) PWSTR Id)
{
    WCHAR SourceId[39], Path[MAX_PATH], Order[4096];
    DWORD Size = sizeof(Order) - 39 * sizeof(WCHAR), Length;
    HKEY Source, Target;
    UUID Uuid;
    RPC_WSTR Text;
    BOOL Result = FALSE;

    if (RegLoadKeyW(HKEY_LOCAL_MACHINE, BCD_SOURCE_KEY, SourceStore) != ERROR_SUCCESS)
        return Fail(L"The boot configuration of the virtual disk could not be loaded.\n");

    if (!GetDefaultEntry(BCD_SOURCE_KEY, SourceId) ||
        UuidCreate(&Uuid) != RPC_S_OK ||
        UuidToStringW(&Uuid, &Text) != RPC_S_OK)
    {
        goto Unload;
    }

    swprintf(Id, 39, L"{%ls}", (PCWSTR)Text);
    RpcStringFreeW(&Text);

    swprintf(Path, MAX_PATH, L"%ls\\Objects\\%ls", BCD_SOURCE_KEY, SourceId);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, REG_OPTION_BACKUP_RESTORE, KEY_READ, NULL, &Source, NULL) != ERROR_SUCCESS)
        goto Unload;

    swprintf(Path, MAX_PATH, L"%ls\\Objects\\%ls", BCD_TARGET_KEY, Id);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, REG_OPTION_BACKUP_RESTORE, KEY_ALL_ACCESS, NULL, &Target, NULL) == ERROR_SUCCESS)
    {
        Result = RegCopyTreeW(Source, NULL, Target) == ERROR_SUCCESS;
        RegCloseKey(Target);
    }
    RegCloseKey(Source);
    if (!Result)
        goto Unload;

    swprintf(Path, MAX_PATH, L"%ls\\Objects\\%ls\\Elements\\24000001", BCD_TARGET_KEY, BCD_BOOTMGR);
    lstrcpyW(Order, Id);
    Length = lstrlenW(Id) + 1;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"Element", RRF_RT_REG_MULTI_SZ, NULL, Order + Length, &Size) != ERROR_SUCCESS)
    {
        Size = sizeof(WCHAR);
        Order[Length] = UNICODE_NULL;
    }

    Result = SetElement(BCD_BOOTMGR, L"24000001", REG_MULTI_SZ, Order, Length * sizeof(WCHAR) + Size) &&
             SetElement(BCD_BOOTMGR, L"23000003", REG_SZ, Id, (lstrlenW(Id) + 1) * sizeof(WCHAR));

Unload:
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, BCD_SOURCE_KEY);
    return Result ? TRUE : Fail(L"The boot entry could not be added.\n");
}

static
BOOL
ConfigureStore(
    _In_ PCWSTR EspRoot,
    _In_ PCWSTR SourceStore,
    _In_ BOOL FreshStore,
    _In_ const VHD_BCD_HOST* Host,
    _In_ PCWSTR RelativePath)
{
    BYTE Device[1024];
    WCHAR Store[MAX_PATH], Id[39];
    DWORD Length;
    BOOL Result;

    swprintf(Store, MAX_PATH, L"%ls%ls", EspRoot, BOOT_STORE);
    if (RegLoadKeyW(HKEY_LOCAL_MACHINE, BCD_TARGET_KEY, Store) != ERROR_SUCCESS)
        return Fail(L"%ls could not be loaded.\n", Store);

    Result = FreshStore ? GetDefaultEntry(BCD_TARGET_KEY, Id) : AddEntry(SourceStore, Id);
    if (!Result)
        Fail(L"The default boot entry of %ls was not found.\n", Store);
    if (Result)
    {
        Length = VhdBuildBcdDevice(Host, RelativePath, VHD_BCD_APPLICATION_PATH, Device, sizeof(Device));
        Result = Length != 0 && SetElement(Id, L"11000001", REG_BINARY, Device, Length);
        Length = VhdBuildBcdDevice(Host, RelativePath, VHD_BCD_SYSTEM_ROOT, Device, sizeof(Device));
        Result = Result && Length != 0 && SetElement(Id, L"21000001", REG_BINARY, Device, Length) &&
                 SetElement(Id, L"12000004", REG_SZ, ENTRY_DESCRIPTION, sizeof(ENTRY_DESCRIPTION));
    }

    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, BCD_TARGET_KEY);
    if (Result)
        wprintf(L"Boot entry %ls starts %ls.\n", Id, RelativePath);
    return Result ? TRUE : Fail(L"The boot entry could not be written to %ls.\n", Store);
}

static
DWORD
BuildLoadOption(
    _In_ const PARTITION_LOCATION* Esp,
    _Out_writes_bytes_(Size) PBYTE Buffer,
    _In_ DWORD Size)
{
    DWORD Description = sizeof(BOOT_DESCRIPTION);
    DWORD FilePath = 4 + sizeof(BOOT_MANAGER);
    DWORD PathList = 42 + FilePath + 4;
    DWORD Total = 6 + Description + PathList;
    ULONGLONG Start = Esp->Offset / Esp->SectorSize;
    ULONGLONG Length = Esp->Length / Esp->SectorSize;
    PBYTE Node;

    if (Size < Total)
        return 0;

    ZeroMemory(Buffer, Total);
    *(PDWORD)Buffer = LOAD_OPTION_ACTIVE;
    *(PWORD)(Buffer + 4) = (WORD)PathList;
    CopyMemory(Buffer + 6, BOOT_DESCRIPTION, Description);

    Node = Buffer + 6 + Description;
    Node[0] = 4;
    Node[1] = 1;
    *(PWORD)(Node + 2) = 42;
    *(PDWORD)(Node + 4) = Esp->PartitionNumber;
    CopyMemory(Node + 8, &Start, sizeof(Start));
    CopyMemory(Node + 16, &Length, sizeof(Length));
    if (Esp->IsGpt)
        CopyMemory(Node + 24, &Esp->PartitionId, sizeof(GUID));
    else
        CopyMemory(Node + 24, &Esp->Signature, sizeof(DWORD));
    Node[40] = Esp->IsGpt ? 2 : 1;
    Node[41] = Esp->IsGpt ? 2 : 1;

    Node += 42;
    Node[0] = 4;
    Node[1] = 4;
    *(PWORD)(Node + 2) = (WORD)FilePath;
    CopyMemory(Node + 4, BOOT_MANAGER, sizeof(BOOT_MANAGER));

    Node += FilePath;
    Node[0] = 0x7F;
    Node[1] = 0xFF;
    *(PWORD)(Node + 2) = 4;
    return Total;
}

static
BOOL
IsLiberNTOption(
    _In_reads_bytes_(Size) const BYTE* Option,
    _In_ DWORD Size)
{
    DWORD Chars = (DWORD)wcslen(BOOT_MANAGER), i;

    for (i = 0; i + Chars * sizeof(WCHAR) <= Size; i += sizeof(WCHAR))
    {
        if (!_wcsnicmp((PCWSTR)(Option + i), BOOT_MANAGER, Chars))
            return TRUE;
    }

    return FALSE;
}

static
BOOL
ConfigureFirmware(
    _In_ PCWSTR EspVolume,
    _In_ BOOL First)
{
    PARTITION_LOCATION Esp;
    WORD Order[MAX_BOOT_OPTIONS], NewOrder[MAX_BOOT_OPTIONS];
    BYTE Option[1024];
    WCHAR Name[16];
    DWORD Count, Size, Length, i, j = 0, Number = MAXDWORD;

    if (!LocatePartition(EspVolume, &Esp))
        return Fail(L"The EFI system partition %ls was not found.\n", EspVolume);

    Count = GetFirmwareEnvironmentVariableW(L"BootOrder", EFI_GLOBAL_VARIABLE, Order, sizeof(Order)) / sizeof(WORD);
    for (i = 0; i < Count && Number == MAXDWORD; i++)
    {
        swprintf(Name, 16, L"Boot%04X", Order[i]);
        Size = GetFirmwareEnvironmentVariableW(Name, EFI_GLOBAL_VARIABLE, Option, sizeof(Option));
        if (Size != 0 && IsLiberNTOption(Option, Size))
            Number = Order[i];
    }

    for (i = 0; i <= 0xFFFF && Number == MAXDWORD; i++)
    {
        swprintf(Name, 16, L"Boot%04lX", i);
        if (!GetFirmwareEnvironmentVariableW(Name, EFI_GLOBAL_VARIABLE, Option, sizeof(Option)) &&
            GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        {
            Number = i;
        }
    }

    Length = BuildLoadOption(&Esp, Option, sizeof(Option));
    swprintf(Name, 16, L"Boot%04lX", Number);
    if (Number == MAXDWORD || Length == 0 ||
        !SetFirmwareEnvironmentVariableExW(Name, EFI_GLOBAL_VARIABLE, Option, Length, EFI_VARIABLE_FLAGS))
    {
        return Fail(L"The firmware boot option could not be written (%lu).\n", GetLastError());
    }

    if (First)
        NewOrder[j++] = (WORD)Number;
    for (i = 0; i < Count && j < MAX_BOOT_OPTIONS; i++)
    {
        if (Order[i] != Number)
            NewOrder[j++] = Order[i];
    }
    if (!First && j < MAX_BOOT_OPTIONS)
        NewOrder[j++] = (WORD)Number;

    if (!SetFirmwareEnvironmentVariableExW(L"BootOrder", EFI_GLOBAL_VARIABLE, NewOrder, j * sizeof(WORD), EFI_VARIABLE_FLAGS))
        return Fail(L"The firmware boot order could not be written (%lu).\n", GetLastError());

    wprintf(L"Firmware boot option %ls (%ls) is %ls in the boot order.\n",
            Name, BOOT_DESCRIPTION, First ? L"first" : L"last");
    return TRUE;
}

int
wmain(
    int argc,
    WCHAR** argv)
{
    WCHAR Path[MAX_PATH], Mount[MAX_PATH], Volume[MAX_PATH], System[MAX_PATH];
    WCHAR EspRoot[MAX_PATH], EspVolume[MAX_PATH], FileSystem[16], Stream[MAX_PATH + 32];
    WCHAR SourceStore[MAX_PATH], TempDirectory[MAX_PATH];
    PARTITION_LOCATION Host;
    VHD_BCD_HOST BcdHost;
    DWORD Size = sizeof(System);
    BOOL First = TRUE, FreshStore = FALSE, Result;
    PCWSTR Relative;
    int i;

    if (argc < 2)
    {
        wprintf(L"Usage: lntvhdboot <path of the LiberNT VHDX file> [/noorder]\n");
        return 1;
    }

    for (i = 2; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"/noorder"))
            First = FALSE;
    }

    if (!GetFirmwareEnvironmentVariableW(L"", L"{00000000-0000-0000-0000-000000000000}", NULL, 0) &&
        GetLastError() == ERROR_INVALID_FUNCTION)
    {
        Fail(L"This PC does not start with UEFI; the LiberNT boot manager needs UEFI.\n");
        return 1;
    }

    if (!EnablePrivilege(SE_BACKUP_NAME) || !EnablePrivilege(SE_RESTORE_NAME) ||
        !EnablePrivilege(SE_SYSTEM_ENVIRONMENT_NAME))
    {
        Fail(L"Run this tool as an administrator.\n");
        return 1;
    }

    if (!GetFullPathNameW(argv[1], MAX_PATH, Path, NULL) ||
        GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES ||
        !GetVolumePathNameW(Path, Mount, MAX_PATH) ||
        !GetVolumeNameForVolumeMountPointW(Mount, Volume, MAX_PATH) ||
        !GetVolumeInformationW(Mount, NULL, 0, NULL, NULL, NULL, FileSystem, ARRAYSIZE(FileSystem)))
    {
        Fail(L"%ls was not found.\n", argv[1]);
        return 1;
    }

    if (_wcsicmp(FileSystem, L"NTFS"))
    {
        Fail(L"%ls must be on an NTFS volume.\n", Path);
        return 1;
    }

    swprintf(Stream, ARRAYSIZE(Stream), L"%ls:Zone.Identifier", Path);
    if (GetFileAttributesW(Stream) != INVALID_FILE_ATTRIBUTES)
    {
        Fail(L"%ls came from the Internet. Unblock it in its properties first.\n", Path);
        return 1;
    }

    Volume[lstrlenW(Volume) - 1] = UNICODE_NULL;
    if (!LocatePartition(Volume, &Host))
    {
        Fail(L"The partition that holds %ls was not found.\n", Path);
        return 1;
    }

    Relative = Path + lstrlenW(Mount) - 1;
    BcdHost.IsGpt = Host.IsGpt;
    BcdHost.Signature = Host.Signature;
    BcdHost.Offset = Host.Offset;
    BcdHost.DiskId = Host.DiskId;
    BcdHost.PartitionId = Host.PartitionId;

    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", L"SystemPartition", RRF_RT_REG_SZ, NULL, System, &Size) != ERROR_SUCCESS)
    {
        Fail(L"The EFI system partition is not known.\n");
        return 1;
    }

    swprintf(EspRoot, MAX_PATH, L"\\\\?\\GLOBALROOT%ls", System);
    swprintf(EspVolume, MAX_PATH, L"\\\\.\\GLOBALROOT%ls", System);
    GetTempPathW(MAX_PATH, TempDirectory);
    GetTempFileNameW(TempDirectory, L"lnt", 0, SourceStore);

    Result = CopyBootFiles(Path, EspRoot, SourceStore, &FreshStore) &&
             ConfigureStore(EspRoot, SourceStore, FreshStore, &BcdHost, Relative) &&
             ConfigureFirmware(EspVolume, First);
    DeleteFileW(SourceStore);

    if (!Result)
        return 1;

    wprintf(L"Restart the PC to start LiberNT from %ls.\n", Path);
    return 0;
}
