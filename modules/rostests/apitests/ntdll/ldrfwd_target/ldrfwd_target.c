/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Target of the forwarded export used by LdrForwarderSearchPath
 * COPYRIGHT:   Copyright 2026 拓海 帆足 <flute.takkun0407@gmail.com>
 */

#include <windef.h>
#include <winbase.h>

ULONG WINAPI LdrFwdTestFunction(VOID)
{
    return 0x4C465754;
}

BOOL WINAPI DllMain(HINSTANCE hinstDll, DWORD dwReason, LPVOID reserved)
{
    return TRUE;
}
