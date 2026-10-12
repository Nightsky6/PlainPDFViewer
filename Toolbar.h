#pragma once
#include "framework.h"
#include "resource.h"
#include <fpdfview.h>
#include <string>

// ── 레이아웃 상수 ─────────────────────────────────────────────────────────────
#define TOOLBAR_FULL_HEIGHT 40
extern bool g_toolbarHidden;
#define TOOLBAR_HEIGHT   (g_toolbarHidden ? 0 : TOOLBAR_FULL_HEIGHT)
#define STATUSBAR_HEIGHT 32
#define TOOLBAR_BTN_W    32
#define TOOLBAR_BTN_H    24
#define TOOLBAR_MARGIN   4
#define TB_GAP           8    // 오른쪽 특수 버튼 사이 간격 (왼쪽 툴바와 동일)

// ── 툴바 버튼 정의 ────────────────────────────────────────────────────────────
struct ToolbarButton
{
    const wchar_t* label;
    const wchar_t* tooltip;
    int  commandId;
    bool needsDoc;   // PDF가 열려있어야 활성화
    int  pngResId;   // 0이면 텍스트, 아니면 PNG 리소스 ID
    int  iconSize;   // PNG 아이콘 크기 (0이면 기본 20)
    int  width;      // 버튼 너비 (0이면 기본 TOOLBAR_BTN_W)
};

extern const ToolbarButton g_toolbarButtons[];
extern const int           g_toolbarButtonCount;

// ── PlainPDFViewer.cpp 전역 참조 ─────────────────────────────────────────────
extern HINSTANCE     hInst;
extern FPDF_DOCUMENT g_pdfDoc;
extern int           g_currentPage;
extern int           g_pageCount;
extern float         g_zoom;
extern int           g_hoverButton;
extern int           g_statusHoverBtn;
extern bool          g_handMode;
extern bool          g_sidebarVisible;
extern bool          g_autoScrolling;
extern bool          g_slideshowActive;
extern bool          g_findBarVisible;
extern std::wstring  g_filePath;
extern bool          g_highlightMode;
extern COLORREF      g_highlightColor;
extern bool          g_inkMode;
extern COLORREF      g_inkColor;
extern bool          g_darkMode;
extern bool          g_screenLock;
extern bool          g_spotlightMode;
extern bool          g_laserMode;
extern bool          g_quickJumpVisible;

enum SidebarTab { TAB_BOOKMARKS = 0, TAB_THUMBNAILS = 1 };
extern SidebarTab    g_sidebarTab;

// ── 함수 선언
RECT GetToolbarButtonRect(int index);
RECT GetHighlightColorBtnRect();
RECT GetInkColorBtnRect();
RECT GetScreenLockToolbarBtnRect(const RECT& clientRect);
RECT GetDarkModeToolbarBtnRect(const RECT& clientRect);
RECT GetFindToolbarBtnRect(const RECT& clientRect);
RECT GetQuickJumpToolbarBtnRect(const RECT& clientRect);
RECT GetSpotlightToolbarBtnRect(const RECT& clientRect);
RECT GetLaserPointerToolbarBtnRect(const RECT& clientRect);
RECT GetStatusPrevBtnRect(const RECT& clientRect);
RECT GetStatusNextBtnRect(const RECT& clientRect);
RECT GetStatusFirstBtnRect(const RECT& clientRect);
RECT GetStatusLastBtnRect(const RECT& clientRect);

int  HitTestToolbar(int x, int y);
const wchar_t* GetToolbarTooltip(int index);

void DrawPngIcon(HDC hdc, int resId, const RECT& rc, int iconSize, bool enabled, bool darken = false);
void DrawGradientBackground(HDC hdc, const RECT& rect);
void DrawSoftShadow(HDC hdc, int x, int y, int w, int h);
void DrawToolbar(HDC hdc, const RECT& clientRect);
void DrawStatusBar(HDC hdc, const RECT& clientRect);
void DrawAlphaLine(HDC hdc, int x1, int y1, int x2, int y2,
                   COLORREF color, int thickness, BYTE alpha);
void InvalidateToolbarBaseCache(); // 상태 변경 시 툴바 베이스 버퍼 무효화
void InvalidateStatusBarCache();   // 상태 변경 시 상태바 베이스 버퍼 무효화
