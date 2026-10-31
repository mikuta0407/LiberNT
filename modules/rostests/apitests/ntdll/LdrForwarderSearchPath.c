/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Search path used for a forwarder resolved by a later GetProcAddress
 * COPYRIGHT:   Copyright 2026 拓海 帆足 <flute.takkun0407@gmail.com>
 */

#include "precomp.h"

#include <winnls.h>
#include <winuser.h>

#define LDRFWD_SOURCE_NAME L"ldrfwd_source.dll"
#define LDRFWD_TARGET_NAME L"ldrfwd_target.dll"
#define LDRFWD_EXPORT_NAME "LdrFwdTestFunction"
#define LDRFWD_EXPORT_VALUE 0x4C465754

typedef ULONG (WINAPI *PFN_LDRFWD_TEST)(VOID);

static const DWORD LoadFlags[] =
{
    0,
    LOAD_WITH_ALTERED_SEARCH_PATH,
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
};

static BOOL ExtractResource(PCWSTR SavePath, PCWSTR ResourceName)
{
    HRSRC Resource;
    HGLOBAL Global;
    PVOID Data;
    DWORD Size, Written;
    HANDLE File;
    BOOL Success;

    Resource = FindResourceW(NULL, ResourceName, (PCWSTR)RT_RCDATA);
    if (!Resource)
        return FALSE;
    Size = SizeofResource(NULL, Resource);
    Global = LoadResource(NULL, Resource);
    Data = Global ? LockResource(Global) : NULL;
    if (!Size || !Data)
        return FALSE;

    File = CreateFileW(SavePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Success = WriteFile(File, Data, Size, &Written, NULL) && Written == Size;
    CloseHandle(File);
    return Success;
}

static VOID RunChild(DWORD Mode, PCWSTR Directory)
{
    WCHAR SourcePath[MAX_PATH];
    WCHAR LoadedPath[MAX_PATH];
    WCHAR ExpectedPath[MAX_PATH];
    HMODULE Source, Target;
    PFN_LDRFWD_TEST Function;
    DWORD Error;

    _snwprintf(SourcePath, ARRAYSIZE(SourcePath), L"%s\\%s", Directory, LDRFWD_SOURCE_NAME);
    _snwprintf(ExpectedPath, ARRAYSIZE(ExpectedPath), L"%s\\%s", Directory, LDRFWD_TARGET_NAME);

    Source = LoadLibraryExW(SourcePath, NULL, LoadFlags[Mode]);
    ok(Source != NULL, "Mode %lu: LoadLibraryExW failed with %lu\n", Mode, GetLastError());
    if (!Source)
        return;

    ok(GetModuleHandleW(LDRFWD_TARGET_NAME) == NULL,
       "Mode %lu: the forwarder target was loaded with the forwarding DLL\n", Mode);

    SetLastError(0xdeadbeef);
    Function = (PFN_LDRFWD_TEST)GetProcAddress(Source, LDRFWD_EXPORT_NAME);
    Error = GetLastError();
    Target = GetModuleHandleW(LDRFWD_TARGET_NAME);

    if (Mode == 0)
    {
        ok(Function == NULL, "Mode %lu: GetProcAddress resolved the forwarder\n", Mode);
        ok(Error == ERROR_PROC_NOT_FOUND, "Mode %lu: error %lu\n", Mode, Error);
        ok(Target == NULL, "Mode %lu: the forwarder target was loaded\n", Mode);
    }
    else
    {
        ok(Function != NULL, "Mode %lu: GetProcAddress failed with %lu\n", Mode, Error);
        ok(Target != NULL, "Mode %lu: the forwarder target is not loaded\n", Mode);
        if (Function)
        {
            ok(Function() == LDRFWD_EXPORT_VALUE, "Mode %lu: wrong function\n", Mode);
        }
        if (Target)
        {
            ok(GetModuleFileNameW(Target, LoadedPath, ARRAYSIZE(LoadedPath)) != 0,
               "Mode %lu: GetModuleFileNameW failed with %lu\n", Mode, GetLastError());
            ok(_wcsicmp(LoadedPath, ExpectedPath) == 0,
               "Mode %lu: target loaded from %ls\n", Mode, LoadedPath);
        }
    }

    FreeLibrary(Source);
    ok(GetModuleHandleW(LDRFWD_SOURCE_NAME) == NULL,
       "Mode %lu: the forwarding DLL is still loaded\n", Mode);
    ok(GetModuleHandleW(LDRFWD_TARGET_NAME) == NULL,
       "Mode %lu: the forwarder target is still loaded\n", Mode);
}

START_TEST(LdrForwarderSearchPath)
{
    WCHAR TempPath[MAX_PATH];
    WCHAR Directory[MAX_PATH];
    WCHAR SourcePath[MAX_PATH];
    WCHAR TargetPath[MAX_PATH];
    WCHAR ExePath[MAX_PATH];
    WCHAR ExeDirectory[MAX_PATH];
    WCHAR CommandLine[3 * MAX_PATH];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    PWSTR Separator;
    char **Argv;
    int Argc;
    DWORD Mode;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 4)
    {
        WCHAR ChildDirectory[MAX_PATH];

        Mode = strtoul(Argv[2], NULL, 10);
        MultiByteToWideChar(CP_ACP, 0, Argv[3], -1, ChildDirectory, ARRAYSIZE(ChildDirectory));
        if (Mode < ARRAYSIZE(LoadFlags))
            RunChild(Mode, ChildDirectory);
        return;
    }

    ok(GetTempPathW(ARRAYSIZE(TempPath), TempPath) != 0, "GetTempPathW failed with %lu\n", GetLastError());
    _snwprintf(Directory, ARRAYSIZE(Directory), L"%sldrfwd_%lu", TempPath, GetCurrentProcessId());
    ok(CreateDirectoryW(Directory, NULL), "CreateDirectoryW failed with %lu\n", GetLastError());
    _snwprintf(SourcePath, ARRAYSIZE(SourcePath), L"%s\\%s", Directory, LDRFWD_SOURCE_NAME);
    _snwprintf(TargetPath, ARRAYSIZE(TargetPath), L"%s\\%s", Directory, LDRFWD_TARGET_NAME);

    if (!ExtractResource(SourcePath, MAKEINTRESOURCEW(103)) ||
        !ExtractResource(TargetPath, MAKEINTRESOURCEW(104)))
    {
        skip("Cannot extract the test DLLs, error %lu\n", GetLastError());
        goto Cleanup;
    }

    GetModuleFileNameW(NULL, ExePath, ARRAYSIZE(ExePath));
    wcscpy(ExeDirectory, ExePath);
    Separator = wcsrchr(ExeDirectory, L'\\');
    if (Separator)
        *Separator = UNICODE_NULL;

    for (Mode = 0; Mode < ARRAYSIZE(LoadFlags); Mode++)
    {
        ZeroMemory(&StartupInfo, sizeof(StartupInfo));
        StartupInfo.cb = sizeof(StartupInfo);
        _snwprintf(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" LdrForwarderSearchPath %lu \"%s\"", ExePath, Mode, Directory);
        if (!CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, ExeDirectory,
                            &StartupInfo, &ProcessInfo))
        {
            ok(FALSE, "Mode %lu: CreateProcessW failed with %lu\n", Mode, GetLastError());
            continue;
        }
        winetest_wait_child_process(ProcessInfo.hProcess);
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);
    }

Cleanup:
    DeleteFileW(SourcePath);
    DeleteFileW(TargetPath);
    RemoveDirectoryW(Directory);
}
