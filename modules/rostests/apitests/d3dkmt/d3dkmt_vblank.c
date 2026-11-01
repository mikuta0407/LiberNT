/*
 * PROJECT:     LiberNT D3DKMT API Tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Vertical blank state of a source and the cadence of a synchronized flip chain
 * COPYRIGHT:   Copyright 2026 拓海 帆足 <flute.takkun0407@gmail.com>
 */

#define COBJMACROS
#include "precomp.h"
#include <d3d11.h>

typedef HRESULT (WINAPI *PFN_CREATE_DEVICE_AND_SWAP_CHAIN)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                            const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                            const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
                                                            ID3D11Device **, D3D_FEATURE_LEVEL *,
                                                            ID3D11DeviceContext **);

typedef struct _VBLANK_SOURCE
{
    PFN_D3DKMTGetDeviceState GetDeviceState;
    PFN_D3DKMTWaitForVerticalBlankEvent WaitForVerticalBlankEvent;
    PFN_D3DKMTDestroyDevice DestroyDevice;
    PFN_D3DKMTCloseAdapter CloseAdapter;
    D3DKMT_HANDLE hAdapter;
    D3DKMT_HANDLE hDevice;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
} VBLANK_SOURCE;

static BOOL OpenVBlankSource(VBLANK_SOURCE *Source)
{
    PFN_D3DKMTOpenAdapterFromHdc OpenAdapterFromHdc;
    PFN_D3DKMTCreateDevice CreateDevice;
    D3DKMT_OPENADAPTERFROMHDC Open;
    D3DKMT_CREATEDEVICE Create;
    NTSTATUS Status;

    ZeroMemory(Source, sizeof(*Source));
    OpenAdapterFromHdc = (PFN_D3DKMTOpenAdapterFromHdc)LoadD3DKMTProc("D3DKMTOpenAdapterFromHdc");
    CreateDevice = (PFN_D3DKMTCreateDevice)LoadD3DKMTProc("D3DKMTCreateDevice");
    Source->GetDeviceState = (PFN_D3DKMTGetDeviceState)LoadD3DKMTProc("D3DKMTGetDeviceState");
    Source->WaitForVerticalBlankEvent =
        (PFN_D3DKMTWaitForVerticalBlankEvent)LoadD3DKMTProc("D3DKMTWaitForVerticalBlankEvent");
    Source->DestroyDevice = (PFN_D3DKMTDestroyDevice)LoadD3DKMTProc("D3DKMTDestroyDevice");
    Source->CloseAdapter = (PFN_D3DKMTCloseAdapter)LoadD3DKMTProc("D3DKMTCloseAdapter");
    if (!OpenAdapterFromHdc || !CreateDevice || !Source->GetDeviceState ||
        !Source->WaitForVerticalBlankEvent || !Source->DestroyDevice || !Source->CloseAdapter)
    {
        skip("D3DKMT vertical blank entry points are not exported by gdi32.dll\n");
        return FALSE;
    }

    ZeroMemory(&Open, sizeof(Open));
    Open.hDc = GetDC(NULL);
    Status = OpenAdapterFromHdc(&Open);
    ReleaseDC(NULL, Open.hDc);
    if (!NT_SUCCESS(Status))
    {
        skip("D3DKMTOpenAdapterFromHdc failed with 0x%lx\n", Status);
        return FALSE;
    }
    Source->hAdapter = Open.hAdapter;
    Source->VidPnSourceId = Open.VidPnSourceId;

    ZeroMemory(&Create, sizeof(Create));
    Create.hAdapter = Source->hAdapter;
    Status = CreateDevice(&Create);
    if (!NT_SUCCESS(Status))
    {
        D3DKMT_CLOSEADAPTER Close = {Source->hAdapter};

        skip("D3DKMTCreateDevice failed with 0x%lx\n", Status);
        Source->CloseAdapter(&Close);
        return FALSE;
    }
    Source->hDevice = Create.hDevice;
    return TRUE;
}

static void CloseVBlankSource(VBLANK_SOURCE *Source)
{
    D3DKMT_DESTROYDEVICE Destroy;
    D3DKMT_CLOSEADAPTER Close;

    ZeroMemory(&Destroy, sizeof(Destroy));
    Destroy.hDevice = Source->hDevice;
    Source->DestroyDevice(&Destroy);
    ZeroMemory(&Close, sizeof(Close));
    Close.hAdapter = Source->hAdapter;
    Source->CloseAdapter(&Close);
}

static NTSTATUS QueryVBlank(const VBLANK_SOURCE *Source, D3DKMT_HANDLE hDevice,
                            D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId,
                            D3DKMT_PRESENT_STATS_DWM *Stats)
{
    D3DKMT_GETDEVICESTATE State;
    NTSTATUS Status;

    ZeroMemory(&State, sizeof(State));
    State.hDevice = hDevice;
    State.StateType = D3DKMT_DEVICESTATE_PRESENT_DWM;
    State.PresentStateDWM.VidPnSourceId = VidPnSourceId;
    Status = Source->GetDeviceState(&State);
    *Stats = State.PresentStateDWM.PresentStatsDWM;
    return Status;
}

static BOOL WaitForVBlank(const VBLANK_SOURCE *Source)
{
    D3DKMT_WAITFORVERTICALBLANKEVENT Wait;

    ZeroMemory(&Wait, sizeof(Wait));
    Wait.hAdapter = Source->hAdapter;
    Wait.hDevice = Source->hDevice;
    Wait.VidPnSourceId = Source->VidPnSourceId;
    return NT_SUCCESS(Source->WaitForVerticalBlankEvent(&Wait));
}

START_TEST(vblankstate)
{
    D3DKMT_PRESENT_STATS_DWM Stats, Previous;
    LARGE_INTEGER Frequency, Now;
    VBLANK_SOURCE Source;
    NTSTATUS Status;
    ULONG Index;
    HDC Screen;
    int Hz;

    if (!OpenVBlankSource(&Source))
        return;
    QueryPerformanceFrequency(&Frequency);

    Status = QueryVBlank(&Source, Source.hDevice, Source.VidPnSourceId, &Stats);
    ok(Status == STATUS_SUCCESS, "PRESENT_DWM state returned 0x%lx\n", Status);
    ok(Stats.PresentCount == 0, "PresentCount %u for a device that never presented\n", Stats.PresentCount);
    ok(Stats.PresentRefreshCount == 0, "PresentRefreshCount %u for a device that never presented\n",
       Stats.PresentRefreshCount);
    ok(Stats.PresentQPCTime.QuadPart == 0, "PresentQPCTime %I64d for a device that never presented\n",
       Stats.PresentQPCTime.QuadPart);
    ok(Stats.CustomPresentDuration == 0, "CustomPresentDuration %u\n", Stats.CustomPresentDuration);

    Screen = GetDC(NULL);
    Hz = GetDeviceCaps(Screen, VREFRESH);
    ReleaseDC(NULL, Screen);
    if (Hz < 24 || Hz > 480)
        Hz = 60;

    if (!WaitForVBlank(&Source))
    {
        skip("The source reports no vertical blank\n");
        CloseVBlankSource(&Source);
        return;
    }
    Status = QueryVBlank(&Source, Source.hDevice, Source.VidPnSourceId, &Previous);
    ok(Status == STATUS_SUCCESS, "PRESENT_DWM state returned 0x%lx\n", Status);
    for (Index = 0; Index < 3; ++Index)
    {
        ok(WaitForVBlank(&Source), "Vertical blank wait %lu failed\n", Index);
        Status = QueryVBlank(&Source, Source.hDevice, Source.VidPnSourceId, &Stats);
        QueryPerformanceCounter(&Now);
        ok(Status == STATUS_SUCCESS, "PRESENT_DWM state returned 0x%lx\n", Status);
        ok(Stats.SyncRefreshCount - Previous.SyncRefreshCount >= 1 &&
           Stats.SyncRefreshCount - Previous.SyncRefreshCount <= 2,
           "SyncRefreshCount went from %u to %u across one vertical blank wait\n",
           Previous.SyncRefreshCount, Stats.SyncRefreshCount);
        ok(Stats.SyncQPCTime.QuadPart > Previous.SyncQPCTime.QuadPart &&
           Stats.SyncQPCTime.QuadPart <= Now.QuadPart &&
           (Now.QuadPart - Stats.SyncQPCTime.QuadPart) * Hz < Frequency.QuadPart * 2,
           "SyncQPCTime %I64d after the vertical blank, previous %I64d, now %I64d\n",
           Stats.SyncQPCTime.QuadPart, Previous.SyncQPCTime.QuadPart, Now.QuadPart);
        Previous = Stats;
    }

    Sleep(200);
    Status = QueryVBlank(&Source, Source.hDevice, Source.VidPnSourceId, &Stats);
    ok(Status == STATUS_SUCCESS, "PRESENT_DWM state returned 0x%lx\n", Status);
    ok(Stats.SyncRefreshCount - Previous.SyncRefreshCount >= (UINT)Hz / 10 &&
       Stats.SyncRefreshCount - Previous.SyncRefreshCount <= (UINT)Hz / 2,
       "SyncRefreshCount advanced by %u in 200 ms at %d Hz\n",
       Stats.SyncRefreshCount - Previous.SyncRefreshCount, Hz);

    Status = QueryVBlank(&Source, Source.hDevice, 7, &Stats);
    ok(Status == STATUS_INVALID_PARAMETER, "PRESENT_DWM state of source 7 returned 0x%lx\n", Status);
    Status = QueryVBlank(&Source, 0x12345, Source.VidPnSourceId, &Stats);
    ok(Status == STATUS_INVALID_PARAMETER, "PRESENT_DWM state of an invalid device returned 0x%lx\n", Status);
    Status = QueryVBlank(&Source, 0, Source.VidPnSourceId, &Stats);
    ok(Status == STATUS_INVALID_PARAMETER, "PRESENT_DWM state of a null device returned 0x%lx\n", Status);

    CloseVBlankSource(&Source);
}

START_TEST(flipcadence)
{
    static const FLOAT Colors[2][4] = {{0.1f, 0.2f, 0.3f, 1.0f}, {0.3f, 0.2f, 0.1f, 1.0f}};
    PFN_CREATE_DEVICE_AND_SWAP_CHAIN CreateDeviceAndSwapChain;
    D3DKMT_PRESENT_STATS_DWM Start = {0}, End = {0};
    NTSTATUS StartStatus = STATUS_UNSUCCESSFUL, EndStatus;
    DXGI_SWAP_CHAIN_DESC Desc;
    IDXGISwapChain *SwapChain = NULL;
    ID3D11Device *Device = NULL;
    ID3D11DeviceContext *Context = NULL;
    ID3D11Texture2D *BackBuffer = NULL;
    ID3D11RenderTargetView *View = NULL;
    VBLANK_SOURCE Source;
    HMODULE D3d11;
    HWND Window;
    HRESULT hr = S_OK;
    UINT Frames = 0, VBlanks;
    MSG Msg;

    D3d11 = LoadLibraryW(L"d3d11.dll");
    CreateDeviceAndSwapChain = D3d11 ?
        (PFN_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(D3d11, "D3D11CreateDeviceAndSwapChain") : NULL;
    if (!CreateDeviceAndSwapChain)
    {
        skip("D3D11CreateDeviceAndSwapChain is unavailable\n");
        return;
    }
    if (!OpenVBlankSource(&Source))
        return;
    if (!WaitForVBlank(&Source))
    {
        skip("The source reports no vertical blank\n");
        CloseVBlankSource(&Source);
        return;
    }

    Window = CreateWindowExW(0, L"STATIC", L"flipcadence", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             50, 50, 320, 320, NULL, NULL, NULL, NULL);
    ok(Window != NULL, "CreateWindowExW failed with %lu\n", GetLastError());
    if (!Window)
    {
        CloseVBlankSource(&Source);
        return;
    }

    ZeroMemory(&Desc, sizeof(Desc));
    Desc.BufferDesc.Width = 256;
    Desc.BufferDesc.Height = 256;
    Desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Desc.SampleDesc.Count = 1;
    Desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    Desc.BufferCount = 2;
    Desc.OutputWindow = Window;
    Desc.Windowed = TRUE;
    Desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    hr = CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                  &Desc, &SwapChain, &Device, NULL, &Context);
    if (FAILED(hr))
    {
        skip("No hardware Direct3D 11 flip swap chain, 0x%lx\n", hr);
        goto Cleanup;
    }
    hr = IDXGISwapChain_GetBuffer(SwapChain, 0, &IID_ID3D11Texture2D, (void **)&BackBuffer);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateRenderTargetView(Device, (ID3D11Resource *)BackBuffer, NULL, &View);
    ok(hr == S_OK, "Back buffer view creation returned 0x%lx\n", hr);
    if (FAILED(hr))
        goto Cleanup;

    for (Frames = 0; Frames < 150 && SUCCEEDED(hr); ++Frames)
    {
        if (Frames == 30)
            StartStatus = QueryVBlank(&Source, Source.hDevice, Source.VidPnSourceId, &Start);
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&Msg);
        ID3D11DeviceContext_OMSetRenderTargets(Context, 1, &View, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(Context, View, Colors[Frames & 1]);
        hr = IDXGISwapChain_Present(SwapChain, 1, 0);
    }
    ok(hr == S_OK, "Present returned 0x%lx at frame %u\n", hr, Frames);
    EndStatus = QueryVBlank(&Source, Source.hDevice, Source.VidPnSourceId, &End);
    ok(StartStatus == STATUS_SUCCESS && EndStatus == STATUS_SUCCESS,
       "PRESENT_DWM state returned 0x%lx and 0x%lx\n", StartStatus, EndStatus);
    if (FAILED(hr) || StartStatus != STATUS_SUCCESS || EndStatus != STATUS_SUCCESS)
        goto Cleanup;
    VBlanks = End.SyncRefreshCount - Start.SyncRefreshCount;
    ok(VBlanks != 0, "No vertical blank passed while presenting\n");
    ok(VBlanks != 0 && 120 * 10 >= VBlanks * 9,
       "A sync interval 1 flip chain presented 120 frames in %u vertical blanks\n", VBlanks);

Cleanup:
    if (View)
        ID3D11RenderTargetView_Release(View);
    if (BackBuffer)
        ID3D11Texture2D_Release(BackBuffer);
    if (Context)
        ID3D11DeviceContext_Release(Context);
    if (SwapChain)
        IDXGISwapChain_Release(SwapChain);
    if (Device)
        ID3D11Device_Release(Device);
    DestroyWindow(Window);
    CloseVBlankSource(&Source);
}
