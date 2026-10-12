#pragma once
#include "framework.h"
#include "PlainPDFViewer.h"
#include "resource.h"
#include <fpdfview.h>
#include <fpdf_text.h>
#include <vector>

// ── FindBar 상태 전역 ─────────────────────────────────────────────────────────
extern HWND            g_hFindPanel;
extern HWND            g_hFindEdit;
extern HWND            g_hFindLabel;
extern HWND            g_hFindPrevBtn;
extern HWND            g_hFindNextBtn;
extern HWND            g_hFindCloseBtn;
extern HWND            g_hFindCaseChk;
extern bool            g_findCaseSensitive;
extern WCHAR           g_findText[256];
extern FPDF_SCHHANDLE  g_findHandle;
extern int             g_findPage;
extern int             g_findCharIndex;
extern int             g_findCharCount;
extern int             g_findResultIdx;
extern int             g_findResultTotal;
extern std::vector<int> g_findPageCounts;
extern WNDPROC          g_origEditProc;

// ── 함수 선언 ─────────────────────────────────────────────────────────────────
void FindClose();
void UpdateFindLabel();
void ScrollToFindResult(HWND hWnd);
void FindNext(HWND hWnd, bool forward);
void ShowFindBar(HWND hWnd);
void HideFindBar(HWND hWnd);
void RepositionFindPanel(HWND hWnd);
void CreateFindPanel(HWND hWnd);
void DrawFindHighlight(HDC hdc, const RECT& clientRect);

// 하위 호환 스텁
inline void DrawFindBar(HDC, const RECT&) {}
inline int  HitTestFindBar(int, int) { return -1; }
