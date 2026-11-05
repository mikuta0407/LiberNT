/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for changing the password of an existing local account
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <lm.h>
#include <strsafe.h>

#define TEST_USER L"LntSetPwd"
#define TEST_PASSWORD_ADD L"Lnt!Add#2026a"
#define TEST_PASSWORD_1003 L"Lnt!Set#2026b"
#define TEST_PASSWORD_4 L"Lnt!Set#2026c"
#define TEST_PASSWORD_NET L"Lnt!Net-2026d"

static
DWORD
Logon(
    _In_ PCWSTR Password,
    _In_ DWORD LogonType)
{
    HANDLE Token = NULL;
    DWORD Error;

    SetLastError(0xdeadbeef);
    if (LogonUserW(TEST_USER, L".", Password, LogonType, LOGON32_PROVIDER_DEFAULT, &Token))
    {
        CloseHandle(Token);
        return ERROR_SUCCESS;
    }
    Error = GetLastError();
    trace("logon type %lu with %ls failed %lu\n", LogonType, Password, Error);
    return Error;
}

static
VOID
CheckPassword(
    _In_ PCSTR Step,
    _In_ PCWSTR NewPassword,
    _In_ PCWSTR OldPassword)
{
    DWORD Error;

    Error = Logon(NewPassword, LOGON32_LOGON_NETWORK);
    ok(Error == ERROR_SUCCESS, "%s: network logon with the new password failed %lu\n", Step, Error);
    Error = Logon(NewPassword, LOGON32_LOGON_INTERACTIVE);
    ok(Error == ERROR_SUCCESS, "%s: interactive logon with the new password failed %lu\n", Step, Error);
    Error = Logon(OldPassword, LOGON32_LOGON_NETWORK);
    ok(Error == ERROR_LOGON_FAILURE, "%s: network logon with the old password returned %lu\n", Step, Error);
}

static
DWORD
RunNetUser(
    _In_ PCWSTR Password)
{
    WCHAR Command[MAX_PATH + 64], System[MAX_PATH];
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Process;
    DWORD ExitCode = 0xdeadbeef;

    GetSystemDirectoryW(System, ARRAYSIZE(System));
    StringCchPrintfW(Command, ARRAYSIZE(Command), L"\"%s\\net.exe\" user %s %s", System, TEST_USER, Password);
    if (!CreateProcessW(NULL, Command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &Startup, &Process))
        return GetLastError();
    WaitForSingleObject(Process.hProcess, 30000);
    GetExitCodeProcess(Process.hProcess, &ExitCode);
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
    return ExitCode;
}

START_TEST(NetUserSetInfo)
{
    USER_INFO_1 Info1 = { 0 };
    USER_INFO_1003 Info1003;
    PUSER_INFO_4 Info4 = NULL;
    NET_API_STATUS Status;
    DWORD ParmErr;

    NetUserDel(NULL, TEST_USER);
    Info1.usri1_name = TEST_USER;
    Info1.usri1_password = TEST_PASSWORD_ADD;
    Info1.usri1_priv = USER_PRIV_USER;
    Info1.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    Status = NetUserAdd(NULL, 1, (LPBYTE)&Info1, NULL);
    if (Status == ERROR_ACCESS_DENIED)
    {
        skip("Creating a user needs administrator rights\n");
        return;
    }
    ok(Status == NERR_Success, "NetUserAdd failed %lu\n", Status);
    if (Status != NERR_Success)
        return;

    Info1003.usri1003_password = TEST_PASSWORD_1003;
    ParmErr = 0xdeadbeef;
    Status = NetUserSetInfo(NULL, TEST_USER, 1003, (LPBYTE)&Info1003, &ParmErr);
    ok(Status == NERR_Success, "NetUserSetInfo level 1003 failed %lu (parm %lu)\n", Status, ParmErr);
    CheckPassword("level 1003", TEST_PASSWORD_1003, TEST_PASSWORD_ADD);

    Status = NetUserGetInfo(NULL, TEST_USER, 4, (LPBYTE *)&Info4);
    ok(Status == NERR_Success, "NetUserGetInfo level 4 failed %lu\n", Status);
    if (Status == NERR_Success)
    {
        ok(Info4->usri4_password == NULL, "usri4_password %p\n", Info4->usri4_password);
        Info4->usri4_password = TEST_PASSWORD_4;
        ParmErr = 0xdeadbeef;
        Status = NetUserSetInfo(NULL, TEST_USER, 4, (LPBYTE)Info4, &ParmErr);
        ok(Status == NERR_Success, "NetUserSetInfo level 4 failed %lu (parm %lu)\n", Status, ParmErr);
        NetApiBufferFree(Info4);
        CheckPassword("level 4", TEST_PASSWORD_4, TEST_PASSWORD_1003);
    }

    ok(RunNetUser(TEST_PASSWORD_NET) == 0, "net user did not succeed\n");
    CheckPassword("net user", TEST_PASSWORD_NET, TEST_PASSWORD_4);

    NetUserDel(NULL, TEST_USER);
}
