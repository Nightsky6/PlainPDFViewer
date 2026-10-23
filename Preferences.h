#pragma once
#include "framework.h"
#include <vector>
#include <string>

// ─── 바로가기 항목 ────────────────────────────────────────────────────────────

struct QuickJumpItem
{
    std::wstring label;   // 표시 이름 (사용자 입력)
    std::wstring keyword; // 검색 키워드
};

#define QUICKJUMP_MAX 10

// ─── 사용자 설정 구조체 ───────────────────────────────────────────────────────

struct AppPreferences
{
    COLORREF selectionColor = RGB(51, 144, 255); // 텍스트 선택 색상
    float    scrollSpeed    = 1.0f;              // 자동 스크롤 속도 (0.3 ~ 3.0 px/tick)
    int      maxRecentFiles = 5;                 // 최근 파일 목록 개수 (1 ~ 10)
    bool     showSelToolbar = false;             // 텍스트 선택 후 주석 도구 모음 표시
};

// ─── 전역 설정 인스턴스 ───────────────────────────────────────────────────────

extern AppPreferences g_prefs;
extern std::vector<QuickJumpItem> g_quickJumps;

// ─── 함수 선언 ────────────────────────────────────────────────────────────────

void    LoadPrefs();
void    SavePrefs();
void    LoadQuickJumps();
void    SaveQuickJumps();
INT_PTR ShowPreferencesDialog(HWND hWnd); // IDOK 이면 g_prefs 업데이트됨
