/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test reading the scanout of the remote display adapter
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include "precomp.h"
#include <ntstatus.h>
#include <winternl.h>
#include <winreg.h>
#include <cfgmgr32.h>
#include <swdevice.h>
#include <initguid.h>
#include <ntddvdeo.h>
#include <d3dkmthk.h>
#include <drivers/directx/rdpvdd.h>

#define REMOTE_HARDWARE_ID L"RdpVdd_RemoteDisplay"
#define REMOTE_ENUMERATOR  L"LiberNTDisplayTest"
#define REMOTE_INSTANCE    L"FrameDisplay0001"
#define FRAME_WIDTH        640
#define FRAME_HEIGHT       480
#define FRAME_WIDTH2       800
#define FRAME_HEIGHT2      600
#define FILL_COLOR         RGB(0x12, 0x34, 0x56)
#define FILL_PIXEL         0x00123456

typedef LONG (WINAPI *PFN_SET_DISPLAY_CONFIG)(UINT32, DISPLAYCONFIG_PATH_INFO *, UINT32, DISPLAYCONFIG_MODE_INFO *, UINT32);
typedef NTSTATUS (APIENTRY *PFN_OPEN_FROM_DEVICE_NAME)(D3DKMT_OPENADAPTERFROMDEVICENAME *);
typedef NTSTATUS (APIENTRY *PFN_ESCAPE)(const D3DKMT_ESCAPE *);
typedef NTSTATUS (APIENTRY *PFN_CLOSE_ADAPTER)(const D3DKMT_CLOSEADAPTER *);

typedef struct _CREATE_RESULT
{
    HANDLE Event;
    HRESULT Result;
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
} CREATE_RESULT, *PCREATE_RESULT;

static PFN_OPEN_FROM_DEVICE_NAME pOpenFromDeviceName;
static PFN_ESCAPE pEscape;
static PFN_CLOSE_ADAPTER pCloseAdapter;

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

static VOID
Pump(
    _In_ DWORD Milliseconds)
{
    DWORD Start = GetTickCount();
    MSG Msg;

    while (GetTickCount() - Start < Milliseconds)
    {
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
        Sleep(10);
    }
}

static BOOL
FindRemoteDisplay(
    _Out_ PDISPLAY_DEVICEW Found)
{
    DISPLAY_DEVICEW Device;
    ULONG Index;

    for (Index = 0; Index < 16; Index++)
    {
        ZeroMemory(&Device, sizeof(Device));
        Device.cb = sizeof(Device);
        if (!EnumDisplayDevicesW(NULL, Index, &Device, 0))
            break;
        if (!lstrcmpiW(Device.DeviceID, REMOTE_HARDWARE_ID))
        {
            *Found = Device;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL
WaitForMode(
    _In_ ULONG Width,
    _In_ ULONG Height)
{
    DISPLAY_DEVICEW Device;
    DEVMODEW Mode;
    DWORD Start = GetTickCount(), Index;

    do
    {
        if (FindRemoteDisplay(&Device))
        {
            for (Index = 0;; Index++)
            {
                ZeroMemory(&Mode, sizeof(Mode));
                Mode.dmSize = sizeof(Mode);
                if (!EnumDisplaySettingsW(Device.DeviceName, Index, &Mode))
                    break;
                if (Mode.dmPelsWidth == Width && Mode.dmPelsHeight == Height)
                    return TRUE;
            }
        }
        Sleep(100);
    } while (GetTickCount() - Start < 15000);
    return FALSE;
}

static BOOL
HasDriver(
    _In_ PCWSTR InstanceId)
{
    DEVINST DevInst;
    ULONG Status, Problem;
    DWORD Start = GetTickCount();

    do
    {
        if (CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)InstanceId, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
            CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0) == CR_SUCCESS)
        {
            if (Status & DN_STARTED)
                return TRUE;
            if ((Status & DN_HAS_PROBLEM) && Problem != CM_PROB_NOT_CONFIGURED)
                return FALSE;
        }
        Sleep(100);
    } while (GetTickCount() - Start < 15000);
    return FALSE;
}

static D3DKMT_HANDLE
OpenAdapter(
    _In_ PCWSTR InstanceId)
{
    D3DKMT_OPENADAPTERFROMDEVICENAME Open = { 0 };
    WCHAR Interfaces[512];
    DWORD Start = GetTickCount();

    do
    {
        if (CM_Get_Device_Interface_ListW((LPGUID)&GUID_DISPLAY_DEVICE_ARRIVAL, (DEVINSTID_W)InstanceId, Interfaces,
                                          ARRAYSIZE(Interfaces), CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS &&
            Interfaces[0])
        {
            Open.pDeviceName = Interfaces;
            if (NT_SUCCESS(pOpenFromDeviceName(&Open)))
                return Open.hAdapter;
        }
        Sleep(100);
    } while (GetTickCount() - Start < 15000);
    return 0;
}

static NTSTATUS
FrameEscape(
    _In_ D3DKMT_HANDLE Adapter,
    _Inout_ PVOID Data,
    _In_ UINT Size)
{
    D3DKMT_ESCAPE Request = { 0 };

    Request.hAdapter = Adapter;
    Request.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    Request.pPrivateDriverData = Data;
    Request.PrivateDriverDataSize = Size;
    return pEscape(&Request);
}

static NTSTATUS
SetMode(
    _In_ D3DKMT_HANDLE Adapter,
    _In_ ULONG Width,
    _In_ ULONG Height)
{
    RDPVDD_ESCAPE Request = { RDPVDD_ESCAPE_SET_MODE, Width, Height };

    return FrameEscape(Adapter, &Request, sizeof(Request));
}

static NTSTATUS
ReadFrame(
    _In_ D3DKMT_HANDLE Adapter,
    _In_opt_ PULONG Buffer,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _Out_ PRDPVDD_FRAME_ESCAPE Frame)
{
    ZeroMemory(Frame, sizeof(*Frame));
    Frame->Code = RDPVDD_ESCAPE_READ_FRAME;
    Frame->Buffer = (ULONG64)(ULONG_PTR)Buffer;
    Frame->BufferWidth = Width;
    Frame->BufferHeight = Height;
    return FrameEscape(Adapter, Frame, sizeof(*Frame));
}

static BOOL
WaitForFrameSize(
    _In_ D3DKMT_HANDLE Adapter,
    _In_ ULONG Width,
    _In_ ULONG Height)
{
    RDPVDD_FRAME_ESCAPE Frame;
    DWORD Start = GetTickCount();

    do
    {
        if (NT_SUCCESS(ReadFrame(Adapter, NULL, 0, 0, &Frame)) && Frame.Width == Width && Frame.Height == Height)
            return TRUE;
        Pump(100);
    } while (GetTickCount() - Start < 15000);
    trace("Frame %lux%lu, expected %lux%lu\n", Frame.Width, Frame.Height, Width, Height);
    return FALSE;
}

static BOOL
Covers(
    _In_ const RDPVDD_FRAME_ESCAPE *Frame,
    _In_ const RECT *Rect)
{
    HRGN Region = CreateRectRgn(0, 0, 0, 0), Part, Missing;
    ULONG Index;
    BOOL Result;

    for (Index = 0; Index < Frame->RectCount; Index++)
    {
        Part = CreateRectRgnIndirect(&Frame->Rects[Index]);
        CombineRgn(Region, Region, Part, RGN_OR);
        DeleteObject(Part);
    }
    Missing = CreateRectRgnIndirect(Rect);
    Result = CombineRgn(Missing, Missing, Region, RGN_DIFF) == NULLREGION;
    DeleteObject(Missing);
    DeleteObject(Region);
    return Result;
}

static VOID
GetView(
    _In_ ULONG Width,
    _In_ ULONG Height,
    _Out_ PRECT View)
{
    LONG SourceWidth = GetSystemMetrics(SM_CXSCREEN), SourceHeight = GetSystemMetrics(SM_CYSCREEN);
    LONG cx = Width, cy = Height;

    if ((LONGLONG)Width * SourceHeight > (LONGLONG)Height * SourceWidth)
        cx = (LONG)((LONGLONG)SourceWidth * Height / SourceHeight);
    else
        cy = (LONG)((LONGLONG)SourceHeight * Width / SourceWidth);
    View->left = ((LONG)Width - cx) / 2;
    View->top = ((LONG)Height - cy) / 2;
    View->right = View->left + cx;
    View->bottom = View->top + cy;
}

static VOID
TestRestrictedCaller(
    _In_ D3DKMT_HANDLE Adapter)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    SID_AND_ATTRIBUTES Deny;
    RDPVDD_FRAME_ESCAPE Frame;
    HANDLE Token, Restricted;
    PSID Admins;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE, &Token))
    {
        skip("OpenProcessToken failed: %lu\n", GetLastError());
        return;
    }
    AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &Admins);
    Deny.Sid = Admins;
    Deny.Attributes = 0;
    if (!CreateRestrictedToken(Token, DISABLE_MAX_PRIVILEGE, 1, &Deny, 0, NULL, 0, NULL, &Restricted))
    {
        skip("CreateRestrictedToken failed: %lu\n", GetLastError());
    }
    else
    {
        ok(ImpersonateLoggedOnUser(Restricted), "ImpersonateLoggedOnUser failed: %lu\n", GetLastError());
        ok_hex(ReadFrame(Adapter, NULL, 0, 0, &Frame), STATUS_ACCESS_DENIED);
        RevertToSelf();
        CloseHandle(Restricted);
    }
    FreeSid(Admins);
    CloseHandle(Token);
}

static VOID
TestFrames(
    _In_ D3DKMT_HANDLE Adapter)
{
    RDPVDD_FRAME_ESCAPE Frame;
    RECT Fill, Target, View, Bar;
    PULONG Buffer;
    HANDLE Event;
    HWND hWnd;
    HBRUSH Brush;
    WNDCLASSW Class = { 0 };
    ULONG Size;
    POINT Center;

    ok(WaitForFrameSize(Adapter, FRAME_WIDTH, FRAME_HEIGHT), "The clone target is not %ux%u\n", FRAME_WIDTH, FRAME_HEIGHT);

    Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ZeroMemory(&Frame, sizeof(Frame));
    Frame.Code = RDPVDD_ESCAPE_OPEN_FRAME;
    Frame.Event = (ULONG64)(ULONG_PTR)INVALID_HANDLE_VALUE;
    ok_hex(FrameEscape(Adapter, &Frame, sizeof(Frame)), STATUS_OBJECT_TYPE_MISMATCH);
    Frame.Event = 0x12340;
    ok_hex(FrameEscape(Adapter, &Frame, sizeof(Frame)), STATUS_INVALID_HANDLE);
    Frame.Event = (ULONG64)(ULONG_PTR)Event;
    ok_hex(FrameEscape(Adapter, &Frame, sizeof(Frame)), STATUS_SUCCESS);
    ok(WaitForSingleObject(Event, 0) == WAIT_OBJECT_0, "No frame event after the open\n");

    ok_hex(ReadFrame(Adapter, NULL, 0, 0, &Frame), STATUS_SUCCESS);
    ok(Frame.Width == FRAME_WIDTH && Frame.Height == FRAME_HEIGHT, "Frame %lux%lu\n", Frame.Width, Frame.Height);
    ok_long(Frame.RectCount, 0);

    Size = FRAME_WIDTH * FRAME_HEIGHT * 4;
    Buffer = VirtualAlloc(NULL, Size, MEM_COMMIT, PAGE_READWRITE);
    ok_hex(ReadFrame(Adapter, (PULONG)(ULONG_PTR)0x1000, FRAME_WIDTH, FRAME_HEIGHT, &Frame), STATUS_ACCESS_VIOLATION);
    ok_hex(ReadFrame(Adapter, Buffer, FRAME_WIDTH - 2, FRAME_HEIGHT, &Frame), STATUS_SUCCESS);
    ok_long(Frame.RectCount, 0);
    ok_hex(ReadFrame(Adapter, Buffer, FRAME_WIDTH, FRAME_HEIGHT, &Frame), STATUS_SUCCESS);
    SetRect(&Target, 0, 0, FRAME_WIDTH, FRAME_HEIGHT);
    ok(Frame.RectCount >= 1 && Covers(&Frame, &Target), "%lu rectangles do not cover the frame\n", Frame.RectCount);
    ok_hex(ReadFrame(Adapter, Buffer, FRAME_WIDTH, FRAME_HEIGHT, &Frame), STATUS_SUCCESS);
    ok(Frame.RectCount == 0 || !Covers(&Frame, &Target), "The changes were not consumed\n");

    GetView(FRAME_WIDTH, FRAME_HEIGHT, &View);
    SetRect(&Fill, 100, 100, 300, 260);
    Brush = CreateSolidBrush(FILL_COLOR);
    Class.lpfnWndProc = DefWindowProcW;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.hbrBackground = Brush;
    Class.lpszClassName = L"RemoteDisplayFrameFill";
    RegisterClassW(&Class);
    hWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, Class.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
                           Fill.left, Fill.top, Fill.right - Fill.left, Fill.bottom - Fill.top, NULL, NULL,
                           Class.hInstance, NULL);
    ok(hWnd != NULL, "CreateWindowExW failed: %lu\n", GetLastError());
    UpdateWindow(hWnd);
    Pump(500);
    ok(WaitForSingleObject(Event, 5000) == WAIT_OBJECT_0, "No frame event for the window\n");
    Pump(500);

    Target.left = View.left + Fill.left * (View.right - View.left) / GetSystemMetrics(SM_CXSCREEN);
    Target.top = View.top + Fill.top * (View.bottom - View.top) / GetSystemMetrics(SM_CYSCREEN);
    Target.right = View.left + Fill.right * (View.right - View.left) / GetSystemMetrics(SM_CXSCREEN);
    Target.bottom = View.top + Fill.bottom * (View.bottom - View.top) / GetSystemMetrics(SM_CYSCREEN);
    InflateRect(&Target, -2, -2);
    ok_hex(ReadFrame(Adapter, Buffer, FRAME_WIDTH, FRAME_HEIGHT, &Frame), STATUS_SUCCESS);
    ok(Frame.RectCount >= 1 && Frame.RectCount <= RDPVDD_FRAME_MAX_RECTS, "%lu rectangles\n", Frame.RectCount);
    ok(Covers(&Frame, &Target), "The changes do not cover (%ld,%ld)-(%ld,%ld)\n",
       Target.left, Target.top, Target.right, Target.bottom);
    Center.x = (Target.left + Target.right) / 2;
    Center.y = (Target.top + Target.bottom) / 2;
    ok_hex(Buffer[Center.y * FRAME_WIDTH + Center.x] & 0xffffff, FILL_PIXEL);
    ok_hex(Buffer[Target.top * FRAME_WIDTH + Target.left] & 0xffffff, FILL_PIXEL);
    ok_hex(Buffer[(Target.bottom - 1) * FRAME_WIDTH + Target.right - 1] & 0xffffff, FILL_PIXEL);
    if (View.top > 0 || View.left > 0)
    {
        Bar.left = View.top > 0 ? FRAME_WIDTH / 2 : View.left / 2;
        Bar.top = View.top > 0 ? View.top / 2 : FRAME_HEIGHT / 2;
        ok_hex(Buffer[Bar.top * FRAME_WIDTH + Bar.left] & 0xffffff, 0);
    }
    else
    {
        skip("The desktop has the aspect ratio of the frame\n");
    }
    DestroyWindow(hWnd);
    UnregisterClassW(Class.lpszClassName, Class.hInstance);
    DeleteObject(Brush);

    TestRestrictedCaller(Adapter);

    ok_hex(SetMode(Adapter, FRAME_WIDTH2, FRAME_HEIGHT2), STATUS_SUCCESS);
    ok(WaitForSingleObject(Event, 15000) == WAIT_OBJECT_0, "No frame event for the mode change\n");
    ok(WaitForFrameSize(Adapter, FRAME_WIDTH2, FRAME_HEIGHT2), "The frame did not follow the mode\n");
    ok_hex(ReadFrame(Adapter, Buffer, FRAME_WIDTH, FRAME_HEIGHT, &Frame), STATUS_SUCCESS);
    ok_long(Frame.RectCount, 0);
    VirtualFree(Buffer, 0, MEM_RELEASE);
    Size = FRAME_WIDTH2 * FRAME_HEIGHT2 * 4;
    Buffer = VirtualAlloc(NULL, Size, MEM_COMMIT, PAGE_READWRITE);
    ok_hex(ReadFrame(Adapter, Buffer, FRAME_WIDTH2, FRAME_HEIGHT2, &Frame), STATUS_SUCCESS);
    SetRect(&Target, 0, 0, FRAME_WIDTH2, FRAME_HEIGHT2);
    ok(Covers(&Frame, &Target), "%lu rectangles do not cover the new mode\n", Frame.RectCount);
    VirtualFree(Buffer, 0, MEM_RELEASE);
    CloseHandle(Event);
}

START_TEST(RemoteDisplayFrame)
{
    static const WCHAR HardwareIds[] = REMOTE_HARDWARE_ID L"\0";
    PFN_SET_DISPLAY_CONFIG pSetDisplayConfig;
    SW_DEVICE_CREATE_INFO Info;
    CREATE_RESULT Result = { 0 };
    DISPLAY_DEVICEW Device;
    RDPVDD_FRAME_ESCAPE Frame;
    D3DKMT_CLOSEADAPTER Close;
    D3DKMT_HANDLE Adapter;
    HSWDEVICE SwDevice = NULL;
    DEVINST DevInst;
    ULONG Code;
    HRESULT hr;

    pSetDisplayConfig = (PFN_SET_DISPLAY_CONFIG)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetDisplayConfig");
    pOpenFromDeviceName = (PFN_OPEN_FROM_DEVICE_NAME)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTOpenAdapterFromDeviceName");
    pEscape = (PFN_ESCAPE)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTEscape");
    pCloseAdapter = (PFN_CLOSE_ADAPTER)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTCloseAdapter");
    if (!pSetDisplayConfig || !pOpenFromDeviceName || !pEscape || !pCloseAdapter)
    {
        skip("No display configuration or D3DKMT functions\n");
        return;
    }
    if (FindRemoteDisplay(&Device))
    {
        skip("A remote display adapter already exists as %ls\n", Device.DeviceName);
        return;
    }

    Result.Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ZeroMemory(&Info, sizeof(Info));
    Info.cbSize = sizeof(Info);
    Info.pszInstanceId = REMOTE_INSTANCE;
    Info.pszzHardwareIds = HardwareIds;
    Info.pszzCompatibleIds = HardwareIds;
    Info.CapabilityFlags = SWDeviceCapabilitiesRemovable | SWDeviceCapabilitiesSilentInstall |
                           SWDeviceCapabilitiesDriverRequired;
    Info.pszDeviceDescription = L"LiberNT remote display frame test";
    hr = SwDeviceCreate(REMOTE_ENUMERATOR, L"HTREE\\ROOT\\0", &Info, 0, NULL, CreateCallback, &Result, &SwDevice);
    if (FAILED(hr))
    {
        skip("SwDeviceCreate: 0x%08lx\n", hr);
        goto cleanup;
    }
    if (WaitForSingleObject(Result.Event, 15000) != WAIT_OBJECT_0 || FAILED(Result.Result) || !HasDriver(Result.InstanceId))
    {
        skip("No started driver for %ls\n", REMOTE_HARDWARE_ID);
        goto cleanup;
    }
    Adapter = OpenAdapter(Result.InstanceId);
    ok(Adapter != 0, "Cannot open the remote display adapter\n");
    if (!Adapter)
        goto cleanup;

    Code = RDPVDD_ESCAPE_READ_FRAME;
    ok_hex(FrameEscape(Adapter, &Code, sizeof(Code)), STATUS_INVALID_PARAMETER);
    Code = 0x12345678;
    ok_hex(FrameEscape(Adapter, &Code, sizeof(Code)), STATUS_NOT_SUPPORTED);
    ok_hex(ReadFrame(Adapter, NULL, 0, 0, &Frame), STATUS_SUCCESS);
    ok_long(Frame.RectCount, 0);

    ok_hex(SetMode(Adapter, FRAME_WIDTH, FRAME_HEIGHT), STATUS_SUCCESS);
    ok(WaitForMode(FRAME_WIDTH, FRAME_HEIGHT), "The remote display does not list %ux%u\n", FRAME_WIDTH, FRAME_HEIGHT);
    ok_long(pSetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | SDC_TOPOLOGY_CLONE), ERROR_SUCCESS);
    Pump(1500);
    TestFrames(Adapter);
    ok_long(pSetDisplayConfig(0, NULL, 0, NULL, SDC_APPLY | SDC_TOPOLOGY_INTERNAL), ERROR_SUCCESS);
    Pump(1000);

    SwDeviceClose(SwDevice);
    SwDevice = NULL;
    Pump(2000);
    ok(ReadFrame(Adapter, NULL, 0, 0, &Frame) != STATUS_SUCCESS, "The removed adapter still reads\n");
    Close.hAdapter = Adapter;
    pCloseAdapter(&Close);

cleanup:
    if (SwDevice)
        SwDeviceClose(SwDevice);
    if (CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)(L"SWD\\" REMOTE_ENUMERATOR L"\\" REMOTE_INSTANCE),
                           CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS)
    {
        CM_Uninstall_DevNode(DevInst, 0);
    }
    CloseHandle(Result.Event);
}
