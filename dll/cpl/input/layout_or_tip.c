/*
 * PROJECT:     LiberNT input.dll
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Keyboard layout and text service installation API
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include "input.h"

#define COBJMACROS
#include <objbase.h>
#include <msctf.h>

#define LOT_DEFAULT                     0x00000001
#define LOT_DISABLED                    0x00000002

#define LOTP_INPUTPROCESSOR             1
#define LOTP_KEYBOARDLAYOUT             2

#define ILOT_UNINSTALL                  0x00000001
#define ILOT_DEFPROFILE                 0x00000002
#define ILOT_NOAPPLYTOCURRENTSESSION    0x00000020
#define ILOT_CLEANINSTALL               0x00000040
#define ILOT_DISABLED                   0x00000080

#define SDLOT_NOAPPLYTOCURRENTSESSION   0x00000001
#define SDLOT_APPLYTOCURRENTTHREAD      0x00000002

typedef struct tagLAYOUTORTIPPROFILE
{
    DWORD dwProfileType;
    LANGID langid;
    CLSID clsid;
    GUID guidProfile;
    GUID catid;
    DWORD dwSubstituteLayout;
    DWORD dwFlags;
    WCHAR szId[MAX_PATH];
} LAYOUTORTIPPROFILE;

typedef struct
{
    LANGID langid;
    BOOL fTip;
    CLSID clsid;
    GUID guidProfile;
    DWORD dwKlid;
} LAYOUT_OR_TIP;

typedef HRESULT (WINAPI *FN_TF_CreateInputProcessorProfiles)(ITfInputProcessorProfiles **);

static const WCHAR s_szUserProfileKey[] = L"Control Panel\\International\\User Profile";
static const WCHAR s_szPreloadKey[] = L"Keyboard Layout\\Preload";

static BOOL
ParseGuid(_In_ LPCWSTR psz, _Out_ GUID *pGuid)
{
    WCHAR sz[39];
    if (psz[0] != L'{' || psz[37] != L'}')
        return FALSE;
    CopyMemory(sz, psz, 38 * sizeof(WCHAR));
    sz[38] = UNICODE_NULL;
    return SUCCEEDED(CLSIDFromString(sz, pGuid));
}

static LPCWSTR
ParseEntry(_In_ LPCWSTR psz, _Out_ LAYOUT_OR_TIP *pEntry)
{
    LPWSTR pszEnd;
    ULONG langid;

    ZeroMemory(pEntry, sizeof(*pEntry));

    while (*psz == L';' || *psz == L' ')
        ++psz;
    if (!*psz)
        return NULL;

    langid = wcstoul(psz, &pszEnd, 16);
    if (pszEnd == psz || *pszEnd != L':' || langid > 0xFFFF)
        return NULL;
    pEntry->langid = (LANGID)langid;
    psz = pszEnd + 1;

    if (*psz == L'{')
    {
        if (wcslen(psz) < 76 || !ParseGuid(psz, &pEntry->clsid) || !ParseGuid(psz + 38, &pEntry->guidProfile))
            return NULL;
        pEntry->fTip = TRUE;
        psz += 76;
    }
    else
    {
        pEntry->dwKlid = wcstoul(psz, &pszEnd, 16);
        if (pszEnd == psz)
            return NULL;
        psz = pszEnd;
    }

    if (*psz && *psz != L';')
        return NULL;
    return psz;
}

static void
FormatEntryName(_In_ const LAYOUT_OR_TIP *pEntry, _Out_writes_(cch) LPWSTR psz, _In_ SIZE_T cch)
{
    WCHAR szClsid[39], szProfile[39];

    if (!pEntry->fTip)
    {
        StringCchPrintfW(psz, cch, L"%04X:%08X", pEntry->langid, pEntry->dwKlid);
        return;
    }

    StringFromGUID2(&pEntry->clsid, szClsid, _countof(szClsid));
    StringFromGUID2(&pEntry->guidProfile, szProfile, _countof(szProfile));
    StringCchPrintfW(psz, cch, L"%04X:%s%s", pEntry->langid, szClsid, szProfile);
}

static ITfInputProcessorProfiles *
CreateProfiles(_Out_ HMODULE *phMsctf)
{
    FN_TF_CreateInputProcessorProfiles fnCreate;
    ITfInputProcessorProfiles *pProfiles = NULL;

    *phMsctf = LoadLibraryW(L"msctf.dll");
    if (!*phMsctf)
        return NULL;

    fnCreate = (FN_TF_CreateInputProcessorProfiles)GetProcAddress(*phMsctf, "TF_CreateInputProcessorProfiles");
    if (!fnCreate || FAILED(fnCreate(&pProfiles)))
    {
        FreeLibrary(*phMsctf);
        *phMsctf = NULL;
        return NULL;
    }
    return pProfiles;
}

static void
ReleaseProfiles(_In_opt_ ITfInputProcessorProfiles *pProfiles, _In_opt_ HMODULE hMsctf)
{
    if (pProfiles)
        ITfInputProcessorProfiles_Release(pProfiles);
    if (hMsctf)
        FreeLibrary(hMsctf);
}

static BOOL
FindUserProfileValue(_In_ HKEY hUserProfile, _In_ LPCWSTR pszName, _Out_writes_(cchSubKey) LPWSTR pszSubKey,
                     _In_ DWORD cchSubKey)
{
    DWORD dwIndex;

    for (dwIndex = 0;; ++dwIndex)
    {
        HKEY hLang;
        DWORD cch = cchSubKey;
        LSTATUS error;

        if (RegEnumKeyExW(hUserProfile, dwIndex, pszSubKey, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            return FALSE;
        if (RegOpenKeyExW(hUserProfile, pszSubKey, 0, KEY_QUERY_VALUE, &hLang) != ERROR_SUCCESS)
            continue;
        error = RegQueryValueExW(hLang, pszName, NULL, NULL, NULL, NULL);
        RegCloseKey(hLang);
        if (error == ERROR_SUCCESS)
            return TRUE;
    }
}

static BOOL
FindLanguageSubKey(_In_ HKEY hUserProfile, _In_ LANGID langid, _Out_writes_(cchSubKey) LPWSTR pszSubKey,
                   _In_ DWORD cchSubKey)
{
    WCHAR szPrefix[6];
    DWORD dwIndex;

    StringCchPrintfW(szPrefix, _countof(szPrefix), L"%04X:", langid);

    for (dwIndex = 0;; ++dwIndex)
    {
        HKEY hLang;
        DWORD cch = cchSubKey, dwValue;
        BOOL bFound = FALSE;

        if (RegEnumKeyExW(hUserProfile, dwIndex, pszSubKey, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExW(hUserProfile, pszSubKey, 0, KEY_QUERY_VALUE, &hLang) != ERROR_SUCCESS)
            continue;
        for (dwValue = 0; !bFound; ++dwValue)
        {
            WCHAR szName[MAX_PATH];
            DWORD cchName = _countof(szName);
            if (RegEnumValueW(hLang, dwValue, szName, &cchName, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            bFound = !_wcsnicmp(szName, szPrefix, 5);
        }
        RegCloseKey(hLang);
        if (bFound)
            return TRUE;
    }

    if (!LCIDToLocaleName(MAKELCID(langid, SORT_DEFAULT), pszSubKey, cchSubKey, 0))
        StringCchPrintfW(pszSubKey, cchSubKey, L"%04X", langid);
    return TRUE;
}

static BOOL
AddUserProfileEntry(_In_ const LAYOUT_OR_TIP *pEntry, _In_ BOOL bDefault)
{
    WCHAR szName[MAX_PATH], szSubKey[LOCALE_NAME_MAX_LENGTH + 1];
    HKEY hUserProfile, hLang;
    DWORD dwIndex, dwOrder = 0;
    LSTATUS error;

    FormatEntryName(pEntry, szName, _countof(szName));

    error = RegCreateKeyExW(HKEY_CURRENT_USER, s_szUserProfileKey, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL,
                            &hUserProfile, NULL);
    if (error != ERROR_SUCCESS)
        return FALSE;

    if (!FindUserProfileValue(hUserProfile, szName, szSubKey, _countof(szSubKey)))
        FindLanguageSubKey(hUserProfile, pEntry->langid, szSubKey, _countof(szSubKey));

    error = RegCreateKeyExW(hUserProfile, szSubKey, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &hLang, NULL);
    if (error != ERROR_SUCCESS)
    {
        RegCloseKey(hUserProfile);
        return FALSE;
    }

    if (RegQueryValueExW(hLang, szName, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
    {
        for (dwIndex = 0;; ++dwIndex)
        {
            WCHAR szValue[MAX_PATH];
            DWORD cchValue = _countof(szValue), dwType, dwData, cbData = sizeof(dwData);
            LSTATUS status = RegEnumValueW(hLang, dwIndex, szValue, &cchValue, NULL, &dwType, (LPBYTE)&dwData,
                                           &cbData);
            if (status == ERROR_MORE_DATA)
                continue;
            if (status != ERROR_SUCCESS)
                break;
            if (dwType == REG_DWORD && dwData > dwOrder)
                dwOrder = dwData;
        }
        ++dwOrder;
        error = RegSetValueExW(hLang, szName, 0, REG_DWORD, (const BYTE *)&dwOrder, sizeof(dwOrder));
    }

    if (error == ERROR_SUCCESS && bDefault)
    {
        error = RegSetValueExW(hUserProfile, L"InputMethodOverride", 0, REG_SZ, (const BYTE *)szName,
                               (DWORD)(wcslen(szName) + 1) * sizeof(WCHAR));
    }

    RegCloseKey(hLang);
    RegCloseKey(hUserProfile);
    return (error == ERROR_SUCCESS);
}

static void
RemoveUserProfileEntry(_In_ const LAYOUT_OR_TIP *pEntry)
{
    WCHAR szName[MAX_PATH], szSubKey[LOCALE_NAME_MAX_LENGTH + 1], szOverride[MAX_PATH];
    HKEY hUserProfile, hLang;
    DWORD cbOverride = sizeof(szOverride);

    FormatEntryName(pEntry, szName, _countof(szName));

    if (RegOpenKeyExW(HKEY_CURRENT_USER, s_szUserProfileKey, 0, KEY_READ | KEY_WRITE, &hUserProfile) != ERROR_SUCCESS)
        return;

    while (FindUserProfileValue(hUserProfile, szName, szSubKey, _countof(szSubKey)))
    {
        LSTATUS error;
        if (RegOpenKeyExW(hUserProfile, szSubKey, 0, KEY_SET_VALUE, &hLang) != ERROR_SUCCESS)
            break;
        error = RegDeleteValueW(hLang, szName);
        RegCloseKey(hLang);
        if (error != ERROR_SUCCESS)
            break;
    }

    if (RegQueryValueExW(hUserProfile, L"InputMethodOverride", NULL, NULL, (LPBYTE)szOverride,
                         &cbOverride) == ERROR_SUCCESS &&
        !_wcsicmp(szOverride, szName))
    {
        RegDeleteValueW(hUserProfile, L"InputMethodOverride");
    }

    RegCloseKey(hUserProfile);
}

static BOOL
AddPreloadLayout(_In_ DWORD dwKlid, _In_ BOOL bLanguageOnly)
{
    WCHAR szName[CCH_ULONG_DEC + 1], szKlid[CCH_LAYOUT_ID + 1];
    DWORD dwIndex, dwMax = 0;
    HKEY hPreload;
    LSTATUS error;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, s_szPreloadKey, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &hPreload,
                        NULL) != ERROR_SUCCESS)
        return FALSE;

    for (dwIndex = 0;; ++dwIndex)
    {
        WCHAR szValue[MAX_PATH];
        DWORD cchName = _countof(szName), cbValue = sizeof(szValue), dwType;
        DWORD dwNumber, dwValue;

        if (RegEnumValueW(hPreload, dwIndex, szName, &cchName, NULL, &dwType, (LPBYTE)szValue,
                          &cbValue) != ERROR_SUCCESS)
            break;
        dwNumber = wcstoul(szName, NULL, 10);
        if (dwNumber > dwMax)
            dwMax = dwNumber;
        if (dwType != REG_SZ)
            continue;
        dwValue = wcstoul(szValue, NULL, 16);
        if (dwValue == dwKlid || (bLanguageOnly && LOWORD(dwValue) == LOWORD(dwKlid)))
        {
            RegCloseKey(hPreload);
            return TRUE;
        }
    }

    StringCchPrintfW(szName, _countof(szName), L"%lu", dwMax + 1);
    StringCchPrintfW(szKlid, _countof(szKlid), L"%08lX", dwKlid);
    error = RegSetValueExW(hPreload, szName, 0, REG_SZ, (const BYTE *)szKlid,
                           (DWORD)(wcslen(szKlid) + 1) * sizeof(WCHAR));
    RegCloseKey(hPreload);
    return (error == ERROR_SUCCESS);
}

static BOOL
InstallEntry(_In_opt_ ITfInputProcessorProfiles *pProfiles, _In_ const LAYOUT_OR_TIP *pEntry, _In_ DWORD dwFlags)
{
    BOOL bEnable = !(dwFlags & (ILOT_UNINSTALL | ILOT_DISABLED));

    if (pEntry->fTip)
    {
        if (!pProfiles)
            return FALSE;
        if (FAILED(ITfInputProcessorProfiles_EnableLanguageProfile(pProfiles, &pEntry->clsid, pEntry->langid,
                                                                    &pEntry->guidProfile, bEnable)))
            return FALSE;
        if (bEnable && (dwFlags & ILOT_DEFPROFILE))
        {
            ITfInputProcessorProfiles_SetDefaultLanguageProfile(pProfiles, pEntry->langid, &pEntry->clsid,
                                                                &pEntry->guidProfile);
        }
    }

    if (dwFlags & ILOT_UNINSTALL)
    {
        RemoveUserProfileEntry(pEntry);
        return TRUE;
    }

    if (!AddUserProfileEntry(pEntry, !!(dwFlags & ILOT_DEFPROFILE)))
        return FALSE;

    if (pEntry->fTip)
        return AddPreloadLayout(pEntry->langid, TRUE);
    return AddPreloadLayout(pEntry->dwKlid, FALSE);
}

BOOL WINAPI
InstallLayoutOrTipUserReg(
    _In_opt_ LPCWSTR pszUserReg,
    _In_opt_ LPCWSTR pszSystemReg,
    _In_opt_ LPCWSTR pszSoftwareReg,
    _In_ LPCWSTR psz,
    _In_ DWORD dwFlags)
{
    ITfInputProcessorProfiles *pProfiles;
    HMODULE hMsctf;
    LAYOUT_OR_TIP entry;
    LPCWSTR pszNext;
    BOOL bResult = TRUE, bAny = FALSE;

    if (!psz)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (pszUserReg || pszSystemReg || pszSoftwareReg)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    pProfiles = CreateProfiles(&hMsctf);

    for (pszNext = ParseEntry(psz, &entry); pszNext; pszNext = ParseEntry(pszNext, &entry))
    {
        bAny = TRUE;
        if (!InstallEntry(pProfiles, &entry, dwFlags))
            bResult = FALSE;
        dwFlags &= ~ILOT_DEFPROFILE;
    }

    ReleaseProfiles(pProfiles, hMsctf);

    if (!bAny)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return bResult;
}

BOOL WINAPI
InstallLayoutOrTip(_In_ LPCWSTR psz, _In_ DWORD dwFlags)
{
    return InstallLayoutOrTipUserReg(NULL, NULL, NULL, psz, dwFlags);
}

BOOL WINAPI
SetDefaultLayoutOrTip(_In_ LPCWSTR psz, _In_ DWORD dwFlags)
{
    ITfInputProcessorProfiles *pProfiles = NULL;
    WCHAR szName[MAX_PATH], szSubKey[LOCALE_NAME_MAX_LENGTH + 1];
    HMODULE hMsctf = NULL;
    LAYOUT_OR_TIP entry;
    HKEY hUserProfile;
    LSTATUS error;

    if (!psz || !ParseEntry(psz, &entry))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    FormatEntryName(&entry, szName, _countof(szName));

    if (RegOpenKeyExW(HKEY_CURRENT_USER, s_szUserProfileKey, 0, KEY_READ | KEY_WRITE, &hUserProfile) != ERROR_SUCCESS)
        return FALSE;

    if (!FindUserProfileValue(hUserProfile, szName, szSubKey, _countof(szSubKey)))
    {
        RegCloseKey(hUserProfile);
        SetLastError(ERROR_NOT_FOUND);
        return FALSE;
    }

    error = RegSetValueExW(hUserProfile, L"InputMethodOverride", 0, REG_SZ, (const BYTE *)szName,
                           (DWORD)(wcslen(szName) + 1) * sizeof(WCHAR));
    RegCloseKey(hUserProfile);
    if (error != ERROR_SUCCESS)
        return FALSE;

    if (entry.fTip)
    {
        pProfiles = CreateProfiles(&hMsctf);
        if (pProfiles)
        {
            ITfInputProcessorProfiles_SetDefaultLanguageProfile(pProfiles, entry.langid, &entry.clsid,
                                                                &entry.guidProfile);
        }
        ReleaseProfiles(pProfiles, hMsctf);
    }

    return TRUE;
}

UINT WINAPI
EnumEnabledLayoutOrTip(
    _In_opt_ LPCWSTR pszUserReg,
    _In_opt_ LPCWSTR pszSystemReg,
    _In_opt_ LPCWSTR pszSoftwareReg,
    _Out_writes_opt_(uBufLength) LAYOUTORTIPPROFILE *pLayoutOrTipProfile,
    _In_ UINT uBufLength)
{
    WCHAR szSubKey[LOCALE_NAME_MAX_LENGTH + 1], szOverride[MAX_PATH] = L"";
    DWORD dwSubKey, cbOverride = sizeof(szOverride);
    HKEY hUserProfile;
    UINT cFound = 0;

    if (pszUserReg || pszSystemReg || pszSoftwareReg)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, s_szUserProfileKey, 0, KEY_READ, &hUserProfile) != ERROR_SUCCESS)
        return 0;

    RegQueryValueExW(hUserProfile, L"InputMethodOverride", NULL, NULL, (LPBYTE)szOverride, &cbOverride);

    for (dwSubKey = 0;; ++dwSubKey)
    {
        DWORD cchSubKey = _countof(szSubKey), dwValue;
        HKEY hLang;

        if (RegEnumKeyExW(hUserProfile, dwSubKey, szSubKey, &cchSubKey, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExW(hUserProfile, szSubKey, 0, KEY_QUERY_VALUE, &hLang) != ERROR_SUCCESS)
            continue;

        for (dwValue = 0;; ++dwValue)
        {
            WCHAR szName[MAX_PATH];
            DWORD cchName = _countof(szName), dwType;
            LAYOUT_OR_TIP entry;

            if (RegEnumValueW(hLang, dwValue, szName, &cchName, NULL, &dwType, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (dwType != REG_DWORD || !ParseEntry(szName, &entry))
                continue;

            if (pLayoutOrTipProfile && cFound < uBufLength)
            {
                LAYOUTORTIPPROFILE *pProfile = &pLayoutOrTipProfile[cFound];
                ZeroMemory(pProfile, sizeof(*pProfile));
                pProfile->dwProfileType = entry.fTip ? LOTP_INPUTPROCESSOR : LOTP_KEYBOARDLAYOUT;
                pProfile->langid = entry.langid;
                pProfile->clsid = entry.clsid;
                pProfile->guidProfile = entry.guidProfile;
                if (entry.fTip)
                    pProfile->catid = GUID_TFCAT_TIP_KEYBOARD;
                pProfile->dwFlags = !_wcsicmp(szName, szOverride) ? LOT_DEFAULT : 0;
                StringCchCopyW(pProfile->szId, _countof(pProfile->szId), szName);
            }
            else if (pLayoutOrTipProfile)
            {
                RegCloseKey(hLang);
                RegCloseKey(hUserProfile);
                return cFound;
            }
            ++cFound;
        }
        RegCloseKey(hLang);
    }

    RegCloseKey(hUserProfile);
    return cFound;
}
