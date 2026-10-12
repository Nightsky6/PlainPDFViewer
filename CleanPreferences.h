#pragma once
// ---------------------------------------------------------------
// CleanPreferences - Fluent 스타일 환경 설정 창 (Win32 + GDI+)
//
// 사용법:
//   CleanPreferences::Show(hParent, g_prefs)
//   반환 true  → 설정 변경됨 (g_prefs 갱신 완료)
//   반환 false → 취소
// ---------------------------------------------------------------
#ifndef CLEAN_PREFERENCES_H
#define CLEAN_PREFERENCES_H

#include <windows.h>
#include "Preferences.h"

namespace CleanPreferences
{
    // 설정 창 표시. 확인 시 prefs 를 갱신하고 true 반환.
    bool Show(HWND parent, AppPreferences& prefs);
}

#endif // CLEAN_PREFERENCES_H
