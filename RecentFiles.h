#pragma once
#include "framework.h"
#include <vector>
#include <string>

#define MAX_RECENT_FILES 10  // 최대 허용 개수 (상한)

extern std::vector<std::wstring> g_recentFiles;
extern int                       g_maxRecentFiles;

void LoadRecentFiles();
void SaveRecentFiles();
void AddToRecentFiles(const std::wstring& path);
void UpdateRecentFilesMenu(HWND hWnd);
