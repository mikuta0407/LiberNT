/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test how a second Remote Desktop connection of the same user ends the first one
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include <apitest.h>
#include <lm.h>
#include <wtsapi32.h>
#include "rdpclient.h"

#define USER "rdpsame"
#define ERRINFO_RPC_INITIATED_DISCONNECT 0x00000001

typedef BOOL (WINAPI *ENUMERATE_PROCESSES)(HANDLE, DWORD, DWORD, PWTS_PROCESS_INFOW *, DWORD *);
typedef BOOL (WINAPI *QUERY_SESSION)(HANDLE, DWORD, WTS_INFO_CLASS, LPWSTR *, DWORD *);
typedef VOID (WINAPI *FREE_MEMORY)(PVOID);

/* winpr links WTS functions of its own, so the system's come from wtsapi32.dll */
static BOOL
DesktopStarted(VOID)
{
    static ENUMERATE_PROCESSES EnumerateProcesses;
    static QUERY_SESSION QuerySession;
    static FREE_MEMORY FreeMemory;
    WTS_PROCESS_INFOW *Processes;
    HMODULE Wtsapi;
    WCHAR *Name;
    DWORD Count, Size, Index;
    BOOL Started = FALSE;

    if (!FreeMemory && (Wtsapi = LoadLibraryW(L"wtsapi32.dll")))
    {
        EnumerateProcesses = (ENUMERATE_PROCESSES)(void *)GetProcAddress(Wtsapi, "WTSEnumerateProcessesW");
        QuerySession = (QUERY_SESSION)(void *)GetProcAddress(Wtsapi, "WTSQuerySessionInformationW");
        FreeMemory = (FREE_MEMORY)(void *)GetProcAddress(Wtsapi, "WTSFreeMemory");
    }
    if (!EnumerateProcesses || !QuerySession || !FreeMemory ||
        !EnumerateProcesses(WTS_CURRENT_SERVER_HANDLE, 0, 1, &Processes, &Count))
        return FALSE;
    for (Index = 0; Index < Count && !Started; Index++)
    {
        if (!Processes[Index].SessionId || !Processes[Index].pProcessName ||
            _wcsicmp(Processes[Index].pProcessName, L"explorer.exe"))
            continue;
        if (QuerySession(WTS_CURRENT_SERVER_HANDLE, Processes[Index].SessionId, WTSUserName, &Name, &Size))
        {
            Started = !_wcsicmp(Name, L"" USER);
            FreeMemory(Name);
        }
    }
    FreeMemory(Processes);
    return Started;
}

START_TEST(SameUser)
{
    RDP_TEST_SETUP Setup;
    RDP_CLIENT First, Second;
    DWORD Wait;

    if (!RdpTestStart(&Setup))
        return;
    if (RdpTestAddUser(L"" USER))
    {
        RdpClientStart(&First, USER, NULL);
        ok(RdpClientWaitConnected(&First), "the first connection could not connect: 0x%08x\n", First.Error);
        if (First.Alive)
        {
            for (Wait = 0; Wait < 180 && !DesktopStarted(); Wait++)
                Sleep(1000);
            ok(DesktopStarted(), "the desktop of the first connection did not start\n");
            Sleep(5000);
            RdpClientStart(&Second, USER, NULL);
            ok(RdpClientWaitConnected(&Second), "the second connection could not connect: 0x%08x\n", Second.Error);
            for (Wait = 0; Wait < 300 && !First.Disconnected; Wait++)
                Sleep(100);
            ok(First.Disconnected, "the first connection was not disconnected\n");
            ok(First.ErrorInfo == ERRINFO_RPC_INITIATED_DISCONNECT, "the first connection ended with error info 0x%x\n",
               First.ErrorInfo);
            ok(!Second.Disconnected && Second.Alive, "the second connection ended: 0x%08x, error info 0x%x\n",
               Second.Error, Second.ErrorInfo);
            RdpClientStop(&Second);
        }
        RdpClientStop(&First);
    }
    NetUserDel(NULL, L"" USER);
    RdpTestEnd(&Setup);
}
