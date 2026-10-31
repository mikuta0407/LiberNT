/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SetupDiInstallDevice without a selected driver and raw devices
 * COPYRIGHT:   Copyright 2026 拓海 帆足 <flute.takkun0407@gmail.com>
 */

#include <apitest.h>
#include <winuser.h>
#include <winreg.h>
#include <prsht.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <regstr.h>

static const GUID TestClassGuid =
    {0x4d36e97d, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

static VOID TestRootDeviceWithoutDriver(VOID)
{
    static const WCHAR HardwareId[] = L"ROOT\\LIBERNT_NULL_DRIVER_TEST\0";
    SP_DEVINFO_DATA DeviceInfoData;
    HDEVINFO DeviceInfoSet;
    DWORD Capabilities, RegType, Error;
    BOOL Result;

    DeviceInfoSet = SetupDiCreateDeviceInfoList(&TestClassGuid, NULL);
    ok(DeviceInfoSet != INVALID_HANDLE_VALUE, "SetupDiCreateDeviceInfoList failed with %lu\n", GetLastError());
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        return;

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    if (!SetupDiCreateDeviceInfoW(DeviceInfoSet, L"LIBERNT_NULL_DRIVER_TEST", &TestClassGuid,
                                  NULL, NULL, DICD_GENERATE_ID, &DeviceInfoData))
    {
        ok(FALSE, "SetupDiCreateDeviceInfoW failed with %lu\n", GetLastError());
        goto Cleanup;
    }
    ok(SetupDiSetDeviceRegistryPropertyW(DeviceInfoSet, &DeviceInfoData, SPDRP_HARDWAREID,
                                         (const BYTE *)HardwareId, sizeof(HardwareId)),
       "SetupDiSetDeviceRegistryPropertyW failed with %lu\n", GetLastError());

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, DeviceInfoSet, &DeviceInfoData))
    {
        Error = GetLastError();
        if (Error == ERROR_ACCESS_DENIED)
            skip("Registering a device needs an administrator\n");
        else
            ok(FALSE, "DIF_REGISTERDEVICE failed with %lu\n", Error);
        goto Cleanup;
    }

    Capabilities = 0;
    if (SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, &DeviceInfoData, SPDRP_CAPABILITIES,
                                          &RegType, (PBYTE)&Capabilities, sizeof(Capabilities), NULL))
    {
        ok(!(Capabilities & CM_DEVCAP_RAWDEVICEOK), "The test device is raw, capabilities 0x%lx\n", Capabilities);
    }

    SetLastError(0xdeadbeef);
    Result = SetupDiInstallDevice(DeviceInfoSet, &DeviceInfoData);
    Error = GetLastError();
    ok(Result == FALSE, "SetupDiInstallDevice succeeded without a driver\n");
    ok(Error == ERROR_NO_ASSOCIATED_SERVICE, "Error %lu\n", Error);

    ok(SetupDiCallClassInstaller(DIF_REMOVE, DeviceInfoSet, &DeviceInfoData),
       "DIF_REMOVE failed with %lu\n", GetLastError());

Cleanup:
    SetupDiDestroyDeviceInfoList(DeviceInfoSet);
}

static VOID TestRawDevicesWithoutService(VOID)
{
    SP_DEVINFO_DATA DeviceInfoData;
    HDEVINFO DeviceInfoSet;
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    WCHAR Service[64];
    DWORD Index, Capabilities, ConfigFlags, RegType;
    ULONG Status, Problem;
    ULONG Found = 0;

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    ok(DeviceInfoSet != INVALID_HANDLE_VALUE, "SetupDiGetClassDevsW failed with %lu\n", GetLastError());
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        return;

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData); Index++)
    {
        if (!SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, &DeviceInfoData, SPDRP_CAPABILITIES,
                                               &RegType, (PBYTE)&Capabilities, sizeof(Capabilities), NULL) ||
            !(Capabilities & CM_DEVCAP_RAWDEVICEOK))
        {
            continue;
        }
        if (SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, &DeviceInfoData, SPDRP_SERVICE,
                                              &RegType, (PBYTE)Service, sizeof(Service), NULL))
        {
            continue;
        }

        Found++;
        if (!SetupDiGetDeviceInstanceIdW(DeviceInfoSet, &DeviceInfoData, InstanceId, ARRAYSIZE(InstanceId), NULL))
            InstanceId[0] = UNICODE_NULL;

        ok(CM_Get_DevNode_Status(&Status, &Problem, DeviceInfoData.DevInst, 0) == CR_SUCCESS,
           "CM_Get_DevNode_Status failed for %ls\n", InstanceId);
        ok((Status & DN_STARTED) != 0, "%ls is not started, status 0x%lx\n", InstanceId, Status);
        ok((Status & DN_HAS_PROBLEM) == 0, "%ls has problem %lu\n", InstanceId, Problem);

        ConfigFlags = 0;
        SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, &DeviceInfoData, SPDRP_CONFIGFLAGS,
                                          &RegType, (PBYTE)&ConfigFlags, sizeof(ConfigFlags), NULL);
        ok((ConfigFlags & (CONFIGFLAG_FAILEDINSTALL | CONFIGFLAG_REINSTALL)) == 0,
           "%ls config flags 0x%lx\n", InstanceId, ConfigFlags);
    }

    if (!Found)
        skip("No raw device without a function service is present\n");
    else
        trace("%lu raw device(s) without a function service\n", Found);

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);
}

START_TEST(SetupDiInstallDeviceNullDriver)
{
    TestRootDeviceWithoutDriver();
    TestRawDevicesWithoutService();
}
