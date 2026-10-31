/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     DLL whose only export forwards to ldrfwd_target.dll
 * COPYRIGHT:   Copyright 2026 拓海 帆足 <flute.takkun0407@gmail.com>
 */

#include <windef.h>
#include <winbase.h>

BOOL WINAPI DllMain(HINSTANCE hinstDll, DWORD dwReason, LPVOID reserved)
{
    return TRUE;
}
