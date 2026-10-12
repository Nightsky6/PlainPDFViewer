#pragma once

#include "resource.h"
#include <fpdfview.h>
#include <fpdf_text.h>
#include <string>
#include <vector>

// PDF 좌표계 rect 캐시 (스크롤 독립, 줌 변경 시 무효화)
struct PdfRectCache { double left, top, right, bottom; };

// ── 페이지 레이아웃 정보 (FindBar, Toolbar 등에서 공유) ──────────────────────
struct PageLayoutInfo {
    double pdfW, pdfH;      // PDF 단위
    int renderW, renderH;   // 현재 줌에서의 픽셀 크기
    int yOffset;            // 문서 시작부터의 누적 Y 오프셋 (px)
};

// ── PDF 좌표 → 화면 좌표 변환 (인라인 유틸리티) ──────────────────────────────
#include <algorithm>
static inline void PdfToScreen(double pdfX, double pdfY, double pdfW, double pdfH,
    int pgLeft, int pgTop, float zoom, int rotation,
    int* outSX, int* outSY)
{
    double sx, sy;
    switch (rotation) {
    default:
    case 0: sx = pdfX;         sy = pdfH - pdfY;   break;
    case 1: sx = pdfY;         sy = pdfX;           break;
    case 2: sx = pdfW - pdfX;  sy = pdfY;           break;
    case 3: sx = pdfH - pdfY;  sy = pdfW - pdfX;   break;
    }
    *outSX = pgLeft + (int)(sx * zoom);
    *outSY = pgTop  + (int)(sy * zoom);
}

// ── PlainPDFViewer.cpp 공유 전역 ─────────────────────────────────────────────
extern HINSTANCE           hInst;
extern FPDF_DOCUMENT       g_pdfDoc;
extern FPDF_PAGE           g_pdfPage;
extern FPDF_TEXTPAGE       g_textPage;
extern int                 g_currentPage;
extern int                 g_pageCount;
extern float               g_zoom;
extern int                 g_rotation;
extern int                 g_scrollX;
extern int                 g_scrollY;
extern float               g_scrollYf;
extern std::vector<PageLayoutInfo> g_pageLayouts;
extern int                 g_totalDocHeight;
extern int                 g_maxPageWidth;
// ── 하이라이트 ────────────────────────────────────────────────────────────────
struct HighlightItem {
    int pageIndex  = 0;
    int charStart  = 0;
    int charEnd    = 0;
    COLORREF color = 0;
    std::vector<PdfRectCache> cachedRects;
};
extern std::wstring        g_filePath;
FPDF_DOCUMENT OpenPdfByPathWithPrompt(HWND hWnd, const std::string& u8Path, std::string* pwOut);
extern std::vector<HighlightItem> g_highlights;
extern bool g_dirty;

extern bool                g_fullscreen;
extern DWORD               g_windowedStyle;
extern HMENU               g_hMainMenu;
extern HWND                g_hMainWnd;
extern bool                g_isSelecting;
extern bool                g_hasSelection;
extern int                 g_selPage;

// ── PlainPDFViewer.cpp 공유 함수 ─────────────────────────────────────────────
void LoadPage(HWND hWnd, int pageIndex);
RECT GetViewportRect(const RECT& clientRect);
void GetPageScreenPos(int pageIndex, const RECT& vp, int* outLeft, int* outTop);
void UpdateScrollBars(HWND hWnd);
int  GetBelowToolbarY();
void UpdateTitle(HWND hWnd);
void ZoomTo(HWND hWnd, float newZoom);
void FitToWidth(HWND hWnd);
void FitToPage(HWND hWnd);
void NavigatePage(HWND hWnd, int delta);
void HandleScroll(HWND hWnd, int bar, WPARAM wParam);
void EnterFullscreen(HWND hWnd);
void ExitFullscreen(HWND hWnd);
void ToggleSlideshow(HWND hWnd);
void RecalcPageLayouts();
int  GetMostVisiblePage(const RECT& viewport);
void UpdateActivePageFromScroll(HWND hWnd);
void ScrollToPage(HWND hWnd, int pageIndex);
void ClearPageCache();
void InitPageCache(int pageCount);
void AppendColorMenuItem(HMENU hMenu, UINT id, COLORREF color, const wchar_t* label, bool enabled);
void OpenFileFromPath(HWND hWnd, const std::wstring& path, bool showSpinner = true);
// 이미 열린 탭이 있으면 해당 탭으로 전환, 없으면 새 탭으로 열기
void OpenOrSwitchToTab(HWND hWnd, const std::wstring& path, bool showSpinner = false);
