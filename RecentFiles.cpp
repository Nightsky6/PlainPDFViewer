// RecentFiles.cpp : 최근 파일 목록 (레지스트리 I/O, 메뉴 갱신)
//

#include "framework.h"
#include "RecentFiles.h"
#include "resource.h"
#include <algorithm>

#define REGISTRY_KEY    L"Software\\PlainPDFViewer"
#define REGISTRY_RECENT L"Recent"

// ── 전역 변수 정의 ─────────────────────────────────────────────────────────────
std::vector<std::wstring> g_recentFiles;
int                       g_maxRecentFiles = 5; // AppPreferences::maxRecentFiles 기본값과 동일

// ── 레지스트리에서 불러오기 ────────────────────────────────────────────────────
void LoadRecentFiles()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return;
    g_recentFiles.clear();
    for (int i = 0; i < MAX_RECENT_FILES; i++)
    {
        WCHAR valName[32], data[MAX_PATH] = {};
        swprintf_s(valName, L"%s%d", REGISTRY_RECENT, i);
        DWORD cbData = sizeof(data);
        if (RegQueryValueExW(hKey, valName, nullptr, nullptr, (LPBYTE)data, &cbData) == ERROR_SUCCESS
            && data[0] != L'\0')
            g_recentFiles.push_back(data);
    }
    RegCloseKey(hKey);
}

// ── 레지스트리에 저장 ─────────────────────────────────────────────────────────
void SaveRecentFiles()
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;
    // 기존 항목 전부 삭제 후 재기록
    for (int i = 0; i < MAX_RECENT_FILES; i++)
    {
        WCHAR valName[32];
        swprintf_s(valName, L"%s%d", REGISTRY_RECENT, i);
        RegDeleteValueW(hKey, valName);
    }
    int count = std::min((int)g_recentFiles.size(), MAX_RECENT_FILES);
    for (int i = 0; i < count; i++)
    {
        WCHAR valName[32];
        swprintf_s(valName, L"%s%d", REGISTRY_RECENT, i);
        RegSetValueExW(hKey, valName, 0, REG_SZ,
            (const BYTE*)g_recentFiles[i].c_str(),
            (DWORD)((g_recentFiles[i].size() + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(hKey);
}

// ── 목록에 추가 (중복 제거 후 맨 앞 삽입) ─────────────────────────────────────
void AddToRecentFiles(const std::wstring& path)
{
    // 이미 있으면 맨 앞으로 이동
    for (auto it = g_recentFiles.begin(); it != g_recentFiles.end(); ++it)
    {
        if (_wcsicmp(it->c_str(), path.c_str()) == 0)
        {
            g_recentFiles.erase(it);
            break;
        }
    }
    g_recentFiles.insert(g_recentFiles.begin(), path);
    // 설정된 최대 개수 초과분 제거
    int maxCount = (g_maxRecentFiles > 0) ? g_maxRecentFiles : MAX_RECENT_FILES;
    if ((int)g_recentFiles.size() > maxCount)
        g_recentFiles.resize(maxCount);
    SaveRecentFiles();
}

// ── 파일 메뉴 갱신 ────────────────────────────────────────────────────────────
void UpdateRecentFilesMenu(HWND hWnd)
{
    HMENU hMenu = GetMenu(hWnd);
    if (!hMenu) return;

    HMENU hFileMenu = GetSubMenu(hMenu, 0);
    if (!hFileMenu) return;

    // 기존 최근 파일 항목 전부 제거
    static const int kRemoveIds[10] = {
        IDM_RECENT_FILE1, IDM_RECENT_FILE2, IDM_RECENT_FILE3, IDM_RECENT_FILE4, IDM_RECENT_FILE5,
        IDM_RECENT_FILE6, IDM_RECENT_FILE7, IDM_RECENT_FILE8, IDM_RECENT_FILE9, IDM_RECENT_FILE10 };
    for (int i = 0; i < MAX_RECENT_FILES; i++)
        RemoveMenu(hFileMenu, kRemoveIds[i], MF_BYCOMMAND);

    // IDM_EXIT 앞에 붙어 있는 구분선 모두 제거 (GetMenuItemInfo로 타입 직접 확인)
    {
        for (;;)
        {
            int n = GetMenuItemCount(hFileMenu);
            int exitIdx = -1;
            for (int i = 0; i < n; i++)
                if (GetMenuItemID(hFileMenu, i) == (UINT)IDM_EXIT) { exitIdx = i; break; }
            if (exitIdx < 0) break;

            // IDM_EXIT 바로 위 항목이 구분선이면 제거
            int above = exitIdx - 1;
            if (above < 0) break;
            MENUITEMINFOW mii = {};
            mii.cbSize = sizeof(mii);
            mii.fMask  = MIIM_FTYPE;
            if (!GetMenuItemInfoW(hFileMenu, above, TRUE, &mii)) break;
            if (!(mii.fType & MFT_SEPARATOR)) break;
            RemoveMenu(hFileMenu, above, MF_BYPOSITION);
            // 루프 반복하여 연속된 구분선도 모두 제거
        }
    }

    // 설정된 개수와 실제 목록 크기 중 작은 값
    int maxCount = (g_maxRecentFiles > 0) ? g_maxRecentFiles : MAX_RECENT_FILES;
    int showCount = std::min((int)g_recentFiles.size(), maxCount);
    if (showCount <= 0) return;

    int exitPos = -1;
    int count = GetMenuItemCount(hFileMenu);
    for (int i = 0; i < count; i++)
    {
        if (GetMenuItemID(hFileMenu, i) == IDM_EXIT)
        { exitPos = i; break; }
    }
    if (exitPos < 0) exitPos = count;
    int insertPos = exitPos;

    InsertMenuW(hFileMenu, insertPos, MF_BYPOSITION | MF_SEPARATOR, 0xFFFF, nullptr);
    insertPos++;

    for (int i = 0; i < showCount; i++)
    {
        const wchar_t* fullPath = g_recentFiles[i].c_str();
        const wchar_t* fileName = wcsrchr(fullPath, L'\\');
        if (!fileName) fileName = wcsrchr(fullPath, L'/');
        fileName = fileName ? fileName + 1 : fullPath;

        bool exists = (GetFileAttributesW(fullPath) != INVALID_FILE_ATTRIBUTES);

        WCHAR label[MAX_PATH + 10];
        swprintf_s(label, L"&%d  %s", i + 1, fileName);
        static const int kRecentIds[10] = {
            IDM_RECENT_FILE1, IDM_RECENT_FILE2, IDM_RECENT_FILE3, IDM_RECENT_FILE4, IDM_RECENT_FILE5,
            IDM_RECENT_FILE6, IDM_RECENT_FILE7, IDM_RECENT_FILE8, IDM_RECENT_FILE9, IDM_RECENT_FILE10 };
        InsertMenuW(hFileMenu, insertPos + i, MF_BYPOSITION | MF_STRING,
            kRecentIds[i], label);
        if (!exists)
            EnableMenuItem(hFileMenu, kRecentIds[i], MF_BYCOMMAND | MF_GRAYED);
    }
}
