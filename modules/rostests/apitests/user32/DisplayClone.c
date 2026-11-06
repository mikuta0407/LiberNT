/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test cloning the desktop to a second display adapter
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include "precomp.h"
#include <winreg.h>
#include <cfgmgr32.h>
#include <swdevice.h>

#define MAX_TEST_DEVICES 16
#define REMOTE_HARDWARE_ID L"RdpVdd_RemoteDisplay"
#define REMOTE_ENUMERATOR  L"LiberNTDisplayTest"
#define REMOTE_INSTANCE    L"CloneDisplay0001"
#define REMOTE_INSTANCE2   L"CloneDisplay0002"

typedef struct _OPEN_ADAPTER_FROM_HDC
{
    HDC hDc;
    UINT hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
} OPEN_ADAPTER_FROM_HDC;

typedef struct _OPEN_ADAPTER_FROM_GDI_NAME
{
    WCHAR DeviceName[32];
    UINT hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
} OPEN_ADAPTER_FROM_GDI_NAME;

typedef struct _CLOSE_ADAPTER
{
    UINT hAdapter;
} CLOSE_ADAPTER;

typedef LONG (APIENTRY *PFN_OPEN_ADAPTER_FROM_HDC)(OPEN_ADAPTER_FROM_HDC *);
typedef LONG (APIENTRY *PFN_OPEN_ADAPTER_FROM_GDI_NAME)(OPEN_ADAPTER_FROM_GDI_NAME *);
typedef LONG (APIENTRY *PFN_CLOSE_ADAPTER)(CLOSE_ADAPTER *);

typedef struct _CREATE_RESULT
{
    HANDLE Event;
    HRESULT Result;
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
} CREATE_RESULT, *PCREATE_RESULT;

typedef LONG (WINAPI *PFN_SET_DISPLAY_CONFIG)(UINT32, DISPLAYCONFIG_PATH_INFO *, UINT32, DISPLAYCONFIG_MODE_INFO *, UINT32);

static PFN_SET_DISPLAY_CONFIG pSetDisplayConfig;
static LONG DisplayChanges;

static LRESULT CALLBACK
WindowProc(
    _In_ HWND hWnd,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (Message == WM_DISPLAYCHANGE)
        DisplayChanges++;
    return DefWindowProcW(hWnd, Message, wParam, lParam);
}

static VOID
Pump(
    _In_ DWORD Milliseconds)
{
    DWORD Start = GetTickCount();
    MSG Msg;

    do
    {
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&Msg);
        Sleep(20);
    } while (GetTickCount() - Start < Milliseconds);
}

static VOID WINAPI
CreateCallback(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT CreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId)
{
    PCREATE_RESULT Result = pContext;

    UNREFERENCED_PARAMETER(hSwDevice);
    Result->Result = CreateResult;
    lstrcpynW(Result->InstanceId, pszDeviceInstanceId ? pszDeviceInstanceId : L"", ARRAYSIZE(Result->InstanceId));
    SetEvent(Result->Event);
}

static BOOL
GetDevice(
    _In_ PCWSTR DeviceName,
    _Out_ PDISPLAY_DEVICEW Found)
{
    DISPLAY_DEVICEW Device;
    ULONG Index;

    for (Index = 0; Index < MAX_TEST_DEVICES; Index++)
    {
        ZeroMemory(&Device, sizeof(Device));
        Device.cb = sizeof(Device);
        if (!EnumDisplayDevicesW(NULL, Index, &Device, 0))
            break;
        if (!lstrcmpiW(Device.DeviceName, DeviceName))
        {
            *Found = Device;
            return TRUE;
        }
    }
    ZeroMemory(Found, sizeof(*Found));
    return FALSE;
}

static BOOL
FindDevice(
    _In_ BOOL Primary,
    _In_opt_ PCWSTR DeviceId,
    _Out_ PDISPLAY_DEVICEW Found)
{
    DISPLAY_DEVICEW Device;
    ULONG Index;

    for (Index = 0; Index < MAX_TEST_DEVICES; Index++)
    {
        ZeroMemory(&Device, sizeof(Device));
        Device.cb = sizeof(Device);
        if (!EnumDisplayDevicesW(NULL, Index, &Device, 0))
            break;
        if (Device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)
            continue;
        if (DeviceId ? !lstrcmpiW(Device.DeviceID, DeviceId) :
            !!(Device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) == Primary)
        {
            *Found = Device;
            return TRUE;
        }
    }
    return FALSE;
}

static ULONG
CountMonitors(
    _In_ PCWSTR DeviceName)
{
    DISPLAY_DEVICEW Monitor;
    ULONG Count;

    for (Count = 0; Count < MAX_TEST_DEVICES; Count++)
    {
        ZeroMemory(&Monitor, sizeof(Monitor));
        Monitor.cb = sizeof(Monitor);
        if (!EnumDisplayDevicesW(DeviceName, Count, &Monitor, 0))
            break;
    }
    return Count;
}

static BOOL
HasCurrentMode(
    _In_ PCWSTR DeviceName,
    _Out_opt_ PDEVMODEW Current)
{
    DEVMODEW Mode;

    ZeroMemory(&Mode, sizeof(Mode));
    Mode.dmSize = sizeof(Mode);
    if (!EnumDisplaySettingsW(DeviceName, ENUM_CURRENT_SETTINGS, &Mode))
        return FALSE;
    if (Current)
        *Current = Mode;
    return TRUE;
}

static LONG
StageDetach(
    _In_ PCWSTR DeviceName)
{
    DEVMODEW Mode;

    ZeroMemory(&Mode, sizeof(Mode));
    Mode.dmSize = sizeof(Mode);
    Mode.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
    return ChangeDisplaySettingsExW(DeviceName, &Mode, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
}

static BOOL
WaitForSingle(
    _In_ PCWSTR Second)
{
    DISPLAY_DEVICEW Device;
    DWORD Start = GetTickCount();

    for (;;)
    {
        if (!GetDevice(Second, &Device) || !(Device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            return TRUE;
        if (GetTickCount() - Start > 15000)
            return FALSE;
        Pump(100);
    }
}

static BOOL
FindDevNode(
    _In_ DEVINST DevInst,
    _In_ PCWSTR HardwareId,
    _Out_ DEVINST *Found)
{
    WCHAR Buffer[512];
    ULONG Size = sizeof(Buffer);
    DEVINST Child;

    if (CM_Get_DevNode_Registry_PropertyW(DevInst, CM_DRP_HARDWAREID, NULL, Buffer, &Size, 0) == CR_SUCCESS &&
        !lstrcmpiW(Buffer, HardwareId))
    {
        *Found = DevInst;
        return TRUE;
    }
    if (CM_Get_Child(&Child, DevInst, 0) != CR_SUCCESS)
        return FALSE;
    do
    {
        if (FindDevNode(Child, HardwareId, Found))
            return TRUE;
    } while (CM_Get_Sibling(&Child, Child, 0) == CR_SUCCESS);
    return FALSE;
}

static BOOL
HasDriver(
    _In_ PCWSTR InstanceId)
{
    DEVINST DevInst;
    ULONG Status, Problem;
    DWORD Start = GetTickCount();

    for (;;)
    {
        if (CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)InstanceId, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
            CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0) == CR_SUCCESS)
        {
            if (Status & DN_STARTED)
                return TRUE;
            if ((Status & DN_HAS_PROBLEM) && Problem != CM_PROB_NOT_CONFIGURED)
                return FALSE;
        }
        if (GetTickCount() - Start > 15000)
            return FALSE;
        Sleep(100);
    }
}

static BOOL
WaitForSecond(
    _In_ BOOL Present,
    _Out_ PDISPLAY_DEVICEW Found)
{
    DWORD Start = GetTickCount();

    for (;;)
    {
        if (FindDevice(FALSE, REMOTE_HARDWARE_ID, Found) == Present)
            return TRUE;
        if (GetTickCount() - Start > 15000)
            return FALSE;
        Pump(100);
    }
}

static HSWDEVICE
CreateRemoteDisplay(
    _In_ PCWSTR InstanceId,
    _Inout_ PCREATE_RESULT Result,
    _Out_ PDISPLAY_DEVICEW Second)
{
    static const WCHAR HardwareIds[] = REMOTE_HARDWARE_ID L"\0";
    SW_DEVICE_CREATE_INFO Info;
    HSWDEVICE SwDevice = NULL;
    HRESULT hr;

    ZeroMemory(&Info, sizeof(Info));
    Info.cbSize = sizeof(Info);
    Info.pszInstanceId = InstanceId;
    Info.pszzHardwareIds = HardwareIds;
    Info.pszzCompatibleIds = HardwareIds;
    Info.CapabilityFlags = SWDeviceCapabilitiesRemovable | SWDeviceCapabilitiesSilentInstall |
                           SWDeviceCapabilitiesDriverRequired;
    Info.pszDeviceDescription = L"LiberNT clone display test";

    ResetEvent(Result->Event);
    Result->Result = E_PENDING;
    hr = SwDeviceCreate(REMOTE_ENUMERATOR, L"HTREE\\ROOT\\0", &Info, 0, NULL, CreateCallback, Result, &SwDevice);
    if (FAILED(hr))
        return NULL;
    if (WaitForSingleObject(Result->Event, 15000) != WAIT_OBJECT_0 || FAILED(Result->Result) ||
        !HasDriver(Result->InstanceId) || !WaitForSecond(TRUE, Second))
    {
        SwDeviceClose(SwDevice);
        return NULL;
    }
    return SwDevice;
}

static VOID
TestDetachedDevice(
    _In_ const DISPLAY_DEVICEW *Primary,
    _In_ const DISPLAY_DEVICEW *Second)
{
    DEVMODEW Mode, PrimaryMode;
    LONG Before;

    ok(!HasCurrentMode(Second->DeviceName, NULL), "Detached %ls has a current mode\n", Second->DeviceName);
    ZeroMemory(&Mode, sizeof(Mode));
    Mode.dmSize = sizeof(Mode);
    ok(EnumDisplaySettingsW(Second->DeviceName, 0, &Mode), "Detached %ls lists no mode: %lu\n",
       Second->DeviceName, GetLastError());
    ok(HasCurrentMode(Primary->DeviceName, &PrimaryMode), "No current mode for %ls\n", Primary->DeviceName);

    Mode.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
    Mode.dmPosition.x = PrimaryMode.dmPelsWidth;
    Mode.dmPosition.y = 0;
    ok_long(ChangeDisplaySettingsExW(Second->DeviceName, &Mode, NULL, CDS_TEST, NULL), DISP_CHANGE_FAILED);
    ok_long(ChangeDisplaySettingsExW(Second->DeviceName, &Mode, NULL, 0, NULL), DISP_CHANGE_FAILED);

    Before = DisplayChanges;
    ok_long(StageDetach(Second->DeviceName), DISP_CHANGE_SUCCESSFUL);
    ok_long(ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL), DISP_CHANGE_SUCCESSFUL);
    Pump(1000);
    ok(DisplayChanges == Before, "%ld WM_DISPLAYCHANGE for an unchanged topology\n", DisplayChanges - Before);
    ok(!HasCurrentMode(Second->DeviceName, NULL), "%ls was attached\n", Second->DeviceName);
}

static VOID
TestClone(
    _In_ const DISPLAY_DEVICEW *Primary,
    _In_ const DISPLAY_DEVICEW *Second,
    _Out_ PDISPLAY_DEVICEW Source)
{
    PFN_OPEN_ADAPTER_FROM_HDC pOpenFromHdc;
    PFN_OPEN_ADAPTER_FROM_GDI_NAME pOpenFromName;
    PFN_CLOSE_ADAPTER pClose;
    OPEN_ADAPTER_FROM_HDC FromHdc;
    OPEN_ADAPTER_FROM_GDI_NAME FromName;
    CLOSE_ADAPTER Close;
    DISPLAY_DEVICEW PrimaryNow, SecondNow;
    const DISPLAY_DEVICEW *Other;
    DEVMODEW Mode;
    ULONG Monitors;
    LONG Before;
    HMODULE Gdi32 = GetModuleHandleW(L"gdi32.dll");

    Monitors = CountMonitors(Primary->DeviceName) + CountMonitors(Second->DeviceName);
    ok_long(pSetDisplayConfig(0, NULL, 0, NULL, SDC_TOPOLOGY_CLONE), ERROR_INVALID_PARAMETER);
    ok_long(pSetDisplayConfig(0, NULL, 0, NULL, SDC_VALIDATE | SDC_TOPOLOGY_CLONE), ERROR_SUCCESS);
    ok_long(pSetDisplayConfig(0, NULL, 0, NULL, SDC_VALIDATE | SDC_TOPOLOGY_INTERNAL), ERROR_SUCCESS);

    Before = DisplayChanges;
    ok_long(pSetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | SDC_TOPOLOGY_CLONE), ERROR_SUCCESS);
    Pump(1500);
    ok(DisplayChanges - Before == 1, "%ld WM_DISPLAYCHANGE for the clone\n", DisplayChanges - Before);

    GetDevice(Primary->DeviceName, &PrimaryNow);
    GetDevice(Second->DeviceName, &SecondNow);
    ok(!!(PrimaryNow.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != !!(SecondNow.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP),
       "Flags 0x%lx and 0x%lx, expected one source\n", PrimaryNow.StateFlags, SecondNow.StateFlags);
    *Source = (PrimaryNow.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? PrimaryNow : SecondNow;
    Other = (PrimaryNow.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? &SecondNow : &PrimaryNow;
    ok(Source->StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE, "Source %ls has flags 0x%lx\n", Source->DeviceName, Source->StateFlags);
    ok(!(Other->StateFlags & (DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE)),
       "Clone target %ls has flags 0x%lx\n", Other->DeviceName, Other->StateFlags);
    ok(!HasCurrentMode(Other->DeviceName, NULL), "Clone target %ls has a current mode\n", Other->DeviceName);
    ok(HasCurrentMode(Source->DeviceName, &Mode), "Source %ls has no current mode\n", Source->DeviceName);
    ok(GetSystemMetrics(SM_CXSCREEN) == (INT)Mode.dmPelsWidth && GetSystemMetrics(SM_CYSCREEN) == (INT)Mode.dmPelsHeight,
       "Screen %dx%d, source %lux%lu\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
       Mode.dmPelsWidth, Mode.dmPelsHeight);
    ok_int(GetSystemMetrics(SM_CMONITORS), 1);
    ok(CountMonitors(Source->DeviceName) == Monitors, "Source %ls lists %lu monitors, expected %lu\n",
       Source->DeviceName, CountMonitors(Source->DeviceName), Monitors);
    ok_long(CountMonitors(Other->DeviceName), 0);
    Mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    ok_long(ChangeDisplaySettingsExW(Other->DeviceName, &Mode, NULL, CDS_TEST, NULL), DISP_CHANGE_FAILED);

    pOpenFromHdc = (PFN_OPEN_ADAPTER_FROM_HDC)GetProcAddress(Gdi32, "D3DKMTOpenAdapterFromHdc");
    pOpenFromName = (PFN_OPEN_ADAPTER_FROM_GDI_NAME)GetProcAddress(Gdi32, "D3DKMTOpenAdapterFromGdiDisplayName");
    pClose = (PFN_CLOSE_ADAPTER)GetProcAddress(Gdi32, "D3DKMTCloseAdapter");
    if (pOpenFromHdc && pOpenFromName && pClose)
    {
        ZeroMemory(&FromHdc, sizeof(FromHdc));
        FromHdc.hDc = GetDC(NULL);
        ok_long(pOpenFromHdc(&FromHdc), 0);
        ReleaseDC(NULL, FromHdc.hDc);
        ZeroMemory(&FromName, sizeof(FromName));
        lstrcpynW(FromName.DeviceName, Source->DeviceName, ARRAYSIZE(FromName.DeviceName));
        ok_long(pOpenFromName(&FromName), 0);
        ok(FromHdc.AdapterLuid.LowPart == FromName.AdapterLuid.LowPart &&
           FromHdc.AdapterLuid.HighPart == FromName.AdapterLuid.HighPart,
           "The screen DC is on adapter %lx:%lx, the source %ls on %lx:%lx\n",
           FromHdc.AdapterLuid.HighPart, FromHdc.AdapterLuid.LowPart, Source->DeviceName,
           FromName.AdapterLuid.HighPart, FromName.AdapterLuid.LowPart);
        Close.hAdapter = FromHdc.hAdapter;
        if (Close.hAdapter)
            pClose(&Close);
        Close.hAdapter = FromName.hAdapter;
        if (Close.hAdapter)
            pClose(&Close);
    }
    else
    {
        skip("No D3DKMT adapter functions\n");
    }
}

static VOID
TestSingle(
    _In_ const DISPLAY_DEVICEW *Primary,
    _In_ PCWSTR Second)
{
    DISPLAY_DEVICEW Device;
    ULONG Attached = 0, Index;

    for (Index = 0; Index < MAX_TEST_DEVICES; Index++)
    {
        ZeroMemory(&Device, sizeof(Device));
        Device.cb = sizeof(Device);
        if (!EnumDisplayDevicesW(NULL, Index, &Device, 0))
            break;
        if (Device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
            Attached++;
    }
    ok_long(Attached, 1);
    ok_int(GetSystemMetrics(SM_CMONITORS), 1);
    if (Second && GetDevice(Second, &Device))
        ok(!HasCurrentMode(Second, NULL), "%ls has a current mode\n", Second);
    ok(FindDevice(TRUE, NULL, &Device), "No primary display device\n");
}

START_TEST(DisplayClone)
{
    CREATE_RESULT Result = { 0 };
    DISPLAY_DEVICEW Primary, Second, Source;
    DEVMODEW Before, After;
    HSWDEVICE SwDevice = NULL;
    DEVINST Root, DevInst;
    WNDCLASSW Class = { 0 };
    HWND hWnd;
    LONG Changes;

    Class.lpfnWndProc = WindowProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"DisplayCloneTest";
    RegisterClassW(&Class);
    hWnd = CreateWindowW(Class.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 10, 10, NULL, NULL, Class.hInstance, NULL);
    ok(hWnd != NULL, "CreateWindowW failed: %lu\n", GetLastError());
    Result.Event = CreateEventW(NULL, TRUE, FALSE, NULL);

    pSetDisplayConfig = (PFN_SET_DISPLAY_CONFIG)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetDisplayConfig");
    if (!pSetDisplayConfig)
    {
        skip("No SetDisplayConfig\n");
        goto cleanup;
    }
    if (!FindDevice(TRUE, NULL, &Primary) || !HasCurrentMode(Primary.DeviceName, &Before))
    {
        skip("No primary display device\n");
        goto cleanup;
    }

    if (!FindDevice(FALSE, NULL, &Second))
    {
        SwDevice = CreateRemoteDisplay(REMOTE_INSTANCE, &Result, &Second);
        if (!SwDevice)
        {
            skip("No second display adapter\n");
            goto cleanup;
        }
    }
    trace("Primary %ls '%ls', second %ls '%ls' 0x%lx\n", Primary.DeviceName, Primary.DeviceString,
          Second.DeviceName, Second.DeviceString, Second.StateFlags);

    if (Second.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
    {
        ok_long(StageDetach(Second.DeviceName), DISP_CHANGE_SUCCESSFUL);
        ok_long(ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL), DISP_CHANGE_SUCCESSFUL);
        ok(WaitForSingle(Second.DeviceName), "%ls is still attached\n", Second.DeviceName);
        Pump(1000);
    }

    TestDetachedDevice(&Primary, &Second);
    TestClone(&Primary, &Second, &Source);

    Changes = DisplayChanges;
    ok_long(pSetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | SDC_TOPOLOGY_INTERNAL), ERROR_SUCCESS);
    Pump(1000);
    ok(DisplayChanges - Changes == 1, "%ld WM_DISPLAYCHANGE for the internal topology\n", DisplayChanges - Changes);
    TestSingle(&Primary, NULL);

    ok_long(pSetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | SDC_TOPOLOGY_CLONE), ERROR_SUCCESS);
    Pump(1500);
    CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);
    if (SwDevice ? CM_Locate_DevNodeW(&DevInst, Result.InstanceId, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS :
                   FindDevNode(Root, Second.DeviceID, &DevInst))
    {
        PNP_VETO_TYPE VetoType = PNP_VetoTypeUnknown;
        WCHAR VetoName[MAX_PATH] = L"";

        ok_long(CM_Query_And_Remove_SubTreeW(DevInst, &VetoType, VetoName, ARRAYSIZE(VetoName), CM_REMOVE_NO_RESTART), CR_SUCCESS);
        ok(VetoName[0] == UNICODE_NULL, "Removal vetoed by %d '%ls'\n", VetoType, VetoName);
        ok(WaitForSingle(Second.DeviceName), "The clone survived the removal\n");
        Pump(1000);
        TestSingle(&Primary, NULL);
    }
    else
    {
        skip("No device node for %ls\n", Second.DeviceID);
    }

    if (SwDevice)
    {
        SwDeviceClose(SwDevice);
        ok(WaitForSecond(FALSE, &Second), "%ls is still listed\n", Second.DeviceName);
        SwDevice = CreateRemoteDisplay(REMOTE_INSTANCE2, &Result, &Second);
        ok(SwDevice != NULL, "The remote display did not come back\n");
        if (SwDevice)
        {
            ok_long(pSetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | SDC_TOPOLOGY_CLONE), ERROR_SUCCESS);
            Pump(1500);
            Changes = DisplayChanges;
            SwDeviceClose(SwDevice);
            SwDevice = NULL;
            ok(WaitForSecond(FALSE, &Second), "%ls is still listed after its surprise removal\n", Second.DeviceName);
            Pump(1000);
            ok(DisplayChanges - Changes >= 1, "No WM_DISPLAYCHANGE for the surprise removal\n");
            TestSingle(&Primary, NULL);
        }
    }

    ok(HasCurrentMode(Primary.DeviceName, &After) &&
       After.dmPelsWidth == Before.dmPelsWidth && After.dmPelsHeight == Before.dmPelsHeight,
       "%ls is %lux%lu, was %lux%lu\n", Primary.DeviceName, After.dmPelsWidth, After.dmPelsHeight,
       Before.dmPelsWidth, Before.dmPelsHeight);

cleanup:
    if (SwDevice)
        SwDeviceClose(SwDevice);
    if (CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)(L"SWD\\" REMOTE_ENUMERATOR L"\\" REMOTE_INSTANCE),
                           CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS)
    {
        CM_Uninstall_DevNode(DevInst, 0);
    }
    if (CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)(L"SWD\\" REMOTE_ENUMERATOR L"\\" REMOTE_INSTANCE2),
                           CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS)
    {
        CM_Uninstall_DevNode(DevInst, 0);
    }
    if (hWnd)
        DestroyWindow(hWnd);
    CloseHandle(Result.Event);
}
