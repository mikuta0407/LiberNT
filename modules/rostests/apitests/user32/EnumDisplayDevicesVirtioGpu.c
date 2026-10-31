/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The desktop adapter of a VM whose boot framebuffer has no PCI device
 * COPYRIGHT:   Copyright 2026 拓海 帆足 <flute.takkun0407@gmail.com>
 */

#include "precomp.h"

static BOOL IsVirtioGpu(PCWSTR DeviceId)
{
    return _wcsnicmp(DeviceId, L"PCI\\VEN_1AF4&DEV_1050", 21) == 0;
}

START_TEST(EnumDisplayDevicesVirtioGpu)
{
    DISPLAY_DEVICEW Device;
    DWORD Index;
    ULONG VirtioAdapters = 0;
    ULONG AttachedAdapters = 0;
    ULONG AttachedVirtio = 0;
    ULONG PrimaryVirtio = 0;

    for (Index = 0; ; Index++)
    {
        ZeroMemory(&Device, sizeof(Device));
        Device.cb = sizeof(Device);
        if (!EnumDisplayDevicesW(NULL, Index, &Device, 0))
            break;

        trace("%lu: %ls '%ls' flags 0x%lx id %ls\n", Index, Device.DeviceName,
              Device.DeviceString, Device.StateFlags, Device.DeviceID);

        if (IsVirtioGpu(Device.DeviceID))
            VirtioAdapters++;
        if (!(Device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;

        AttachedAdapters++;
        if (IsVirtioGpu(Device.DeviceID))
        {
            AttachedVirtio++;
            if (Device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
                PrimaryVirtio++;
        }
    }

    if (!VirtioAdapters)
    {
        skip("No VirtIO GPU adapter is present\n");
        return;
    }

    ok(AttachedVirtio == 1, "The VirtIO GPU adapter is not attached to the desktop\n");
    ok(PrimaryVirtio == 1, "The VirtIO GPU adapter is not the primary adapter\n");
    ok(AttachedAdapters == 1, "%lu adapters are attached to the desktop\n", AttachedAdapters);
}
