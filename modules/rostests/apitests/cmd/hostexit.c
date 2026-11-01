/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test that an interactive cmd.exe ends with its console host
 * COPYRIGHT:   Copyright 2026 拓海 帆足 <flute.takkun0407@gmail.com>
 */

#include "precomp.h"
#include <tlhelp32.h>

static DWORD FindConsoleHost(DWORD ParentId)
{
    PROCESSENTRY32W Entry;
    HANDLE Snapshot;
    DWORD Found = 0;

    Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
        return 0;
    Entry.dwSize = sizeof(Entry);
    if (Process32FirstW(Snapshot, &Entry))
    {
        do
        {
            if (Entry.th32ParentProcessID == ParentId && !_wcsicmp(Entry.szExeFile, L"conhost.exe"))
            {
                Found = Entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(Snapshot, &Entry));
    }
    CloseHandle(Snapshot);
    return Found;
}

START_TEST(hostexit)
{
    WCHAR CommandLine[] = L"cmd.exe";
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Info;
    HANDLE Host;
    DWORD HostId = 0, Wait, Tries;

    Startup.dwFlags = STARTF_USESHOWWINDOW;
    Startup.wShowWindow = SW_SHOWMINNOACTIVE;
    if (!CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &Startup, &Info))
    {
        skip("CreateProcessW failed with %lu\n", GetLastError());
        return;
    }

    for (Tries = 0; Tries < 50 && !HostId; Tries++)
    {
        Sleep(100);
        HostId = FindConsoleHost(Info.dwProcessId);
    }
    ok(HostId != 0, "No console host found for cmd.exe\n");
    if (HostId)
    {
        Sleep(1000);
        Host = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, HostId);
        ok(Host != NULL, "OpenProcess failed with %lu\n", GetLastError());
        if (Host)
        {
            ok(TerminateProcess(Host, 0), "TerminateProcess failed with %lu\n", GetLastError());
            WaitForSingleObject(Host, 5000);
            CloseHandle(Host);
        }

        Wait = WaitForSingleObject(Info.hProcess, 10000);
        ok(Wait == WAIT_OBJECT_0, "cmd.exe is still running after its console host ended (%lu)\n", Wait);
    }

    if (WaitForSingleObject(Info.hProcess, 0) != WAIT_OBJECT_0)
        TerminateProcess(Info.hProcess, 1);
    CloseHandle(Info.hThread);
    CloseHandle(Info.hProcess);
}
