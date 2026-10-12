#include "framework.h"
#include "Lang.h"
#include "FindBar.h"
#include "Toolbar.h"
#include <fpdf_text.h>
#include <algorithm>
#include <string>

// ────────────────────────────────────────────────────────────────────────────
// 패널 색상 (툴바 색상 계열)
#define FP_BG         RGB(245, 245, 245)
#define FP_BORDER     RGB(200, 200, 204)
#define FP_EDIT_BG    RGB(255, 255, 255)
#define FP_EDIT_FG    RGB(30,  30,  30)
#define FP_LABEL_FG   RGB(80,  80,  90)
#define FP_CHK_FG     RGB(50,  50,  55)
#define FP_BTN_NORMAL RGB(230, 230, 232)
#define FP_BTN_HOVER  RGB(210, 210, 215)
#define FP_BTN_PRESS  RGB(190, 190, 196)
#define FP_SEP        RGB(200, 200, 204)

// 패널 크기
static const int FINDPANEL_W      = 316;
static const int FINDPANEL_H      = 68;
static const int FINDPANEL_MARGIN = 12;
static const int FP_PAD    = 10;
static const int FP_EDIT_W = 160;
static const int FP_LABEL_W = 50;
static const int FP_BTN_W  = 22;
static const int FP_BTN_H  = 22;
static const int FP_GAP    = 4;
static const int FP_ROW1_Y = 9;
static const int FP_ROW2_Y = 42;

// WM_CTLCOLOR용 브러시 (패널 수명과 동일)
static HBRUSH g_hFindPanelBrush = nullptr;
static HBRUSH g_hFindEditBrush  = nullptr;

// ────────────────────────────────────────────────────────────────────────────

void FindClose()
{
    if (g_findHandle) { FPDFText_FindClose(g_findHandle); g_findHandle = nullptr; }
    g_findPage = -1;
    g_findCharIndex = -1;
    g_findCharCount = 0;
    g_findResultIdx = 0;
    g_findResultTotal = 0;
    g_findPageCounts.clear();
}

// 특정 페이지에서 검색어가 몇 번 나오는지 카운트
static int CountOnPage(int pageIdx, const WCHAR* text)
{
    if (!g_pdfDoc || !text[0]) return 0;

    FPDF_PAGE page = FPDF_LoadPage(g_pdfDoc, pageIdx);
    if (!page) return 0;
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
    if (!tp) { FPDF_ClosePage(page); return 0; }

    int count = 0;
    FPDF_SCHHANDLE sh = FPDFText_FindStart(
        tp, reinterpret_cast<const unsigned short*>(text),
        g_findCaseSensitive ? FPDF_MATCHCASE : 0, 0);
    if (sh)
    {
        while (FPDFText_FindNext(sh)) ++count;
        FPDFText_FindClose(sh);
    }

    FPDFText_ClosePage(tp);
    FPDF_ClosePage(page);
    return count;
}

// 전체 문서에서 검색어 총 개수 (페이지별 캐시도 갱신)
static int CountTotalResults()
{
    if (!g_pdfDoc || !g_findText[0]) { g_findPageCounts.clear(); return 0; }
    g_findPageCounts.resize(g_pageCount, 0);
    int total = 0;
    for (int i = 0; i < g_pageCount; ++i)
    {
        g_findPageCounts[i] = CountOnPage(i, g_findText);
        total += g_findPageCounts[i];
    }
    return total;
}

// 현재 결과가 전체에서 몇 번째인지 계산
static int CalcResultIndex()
{
    if (g_findPage < 0 || !g_findText[0]) return 0;
    int idx = 0;
    for (int i = 0; i < g_findPage; ++i)
        idx += (i < (int)g_findPageCounts.size()) ? g_findPageCounts[i] : CountOnPage(i, g_findText);
    if (g_textPage)
    {
        FPDF_SCHHANDLE sh = FPDFText_FindStart(
            g_textPage, reinterpret_cast<const unsigned short*>(g_findText),
            g_findCaseSensitive ? FPDF_MATCHCASE : 0, 0);
        if (sh)
        {
            while (FPDFText_FindNext(sh))
            {
                ++idx;
                if (FPDFText_GetSchResultIndex(sh) == g_findCharIndex)
                    break;
            }
            FPDFText_FindClose(sh);
        }
    }
    return idx;
}

void UpdateFindLabel()
{
    if (!g_hFindLabel) return;
    if (g_findResultTotal > 0 && g_findResultIdx > 0)
    {
        WCHAR buf[64];
        swprintf_s(buf, L" %d / %d ", g_findResultIdx, g_findResultTotal);
        SetWindowTextW(g_hFindLabel, buf);
    }
    else if (g_findText[0])
        SetWindowTextW(g_hFindLabel, LS(STR_FIND_NO_RESULT));
    else
        SetWindowTextW(g_hFindLabel, L"");
}

// 검색 결과 위치로 스크롤
void ScrollToFindResult(HWND hWnd)
{
    if (!g_pdfPage || !g_textPage || g_findCharIndex < 0) return;
    if (g_pageLayouts.empty()) return;

    FPDFText_CountRects(g_textPage, g_findCharIndex, g_findCharCount);
    double rLeft, rTop, rRight, rBottom;
    if (!FPDFText_GetRect(g_textPage, 0, &rLeft, &rTop, &rRight, &rBottom))
        return;

    const double pageW = g_pageLayouts[g_currentPage].pdfW;
    const double pageH = g_pageLayouts[g_currentPage].pdfH;
    RECT cr; GetClientRect(hWnd, &cr);
    const RECT vp = GetViewportRect(cr);
    const int vpW = vp.right - vp.left;
    const int vpH = vp.bottom - vp.top;

    int sx1, sy1, sx2, sy2;
    PdfToScreen(rLeft, rTop, pageW, pageH, 0, 0, g_zoom, g_rotation, &sx1, &sy1);
    PdfToScreen(rRight, rBottom, pageW, pageH, 0, 0, g_zoom, g_rotation, &sx2, &sy2);
    if (sx1 > sx2) std::swap(sx1, sx2);
    if (sy1 > sy2) std::swap(sy1, sy2);

    int resultDocY = g_pageLayouts[g_currentPage].yOffset + sy1;
    g_scrollY = std::clamp(resultDocY - vpH / 3, 0, std::max(0, g_totalDocHeight - vpH));
    g_scrollYf = (float)g_scrollY;

    if (g_maxPageWidth > vpW)
        g_scrollX = std::clamp(sx1 - vpW / 3, 0, std::max(0, g_maxPageWidth - vpW));

    UpdateScrollBars(hWnd);
    RECT irScroll; GetClientRect(hWnd, &irScroll);
    irScroll.top = TOOLBAR_HEIGHT;
    InvalidateRect(hWnd, &irScroll, FALSE);
}

void FindNext(HWND hWnd, bool forward)
{
    if (!g_pdfDoc) return;

    WCHAR newText[256] = {};
    GetWindowTextW(g_hFindEdit, newText, _countof(newText));
    if (!newText[0]) { FindClose(); UpdateFindLabel(); RECT cr2; GetClientRect(hWnd, &cr2); cr2.top = TOOLBAR_HEIGHT; InvalidateRect(hWnd, &cr2, FALSE); return; }

    bool textChanged = (wcscmp(newText, g_findText) != 0);
    if (textChanged)
    {
        if (g_findHandle) { FPDFText_FindClose(g_findHandle); g_findHandle = nullptr; }
        g_findPage = -1;
        g_findCharIndex = -1;
        g_findCharCount = 0;
        g_findResultIdx = 0;
        g_findResultTotal = 0;
        wcscpy_s(g_findText, newText);
    }

    bool needRecount = (g_findResultTotal == 0 && g_findText[0]);
    int startPage = (g_findPage >= 0) ? g_findPage : g_currentPage;

    if (!g_findHandle)
    {
        if (g_currentPage != startPage)
            LoadPage(hWnd, startPage);
        g_findHandle = FPDFText_FindStart(
            g_textPage,
            reinterpret_cast<const unsigned short*>(g_findText),
            g_findCaseSensitive ? FPDF_MATCHCASE : 0, 0);
        g_findPage = startPage;
        needRecount = true;

        if (!forward && g_findHandle)
        {
            int lastIdx = -1, lastCount = 0;
            while (FPDFText_FindNext(g_findHandle))
            {
                lastIdx   = FPDFText_GetSchResultIndex(g_findHandle);
                lastCount = FPDFText_GetSchCount(g_findHandle);
            }
            if (lastIdx >= 0)
            {
                g_findCharIndex = lastIdx;
                g_findCharCount = lastCount;
                FPDFText_FindClose(g_findHandle);
                g_findHandle = FPDFText_FindStart(
                    g_textPage,
                    reinterpret_cast<const unsigned short*>(g_findText),
                    g_findCaseSensitive ? FPDF_MATCHCASE : 0, 0);
                if (g_findHandle)
                {
                    while (FPDFText_FindNext(g_findHandle))
                    {
                        if (FPDFText_GetSchResultIndex(g_findHandle) == lastIdx)
                            break;
                    }
                }
                if (needRecount) g_findResultTotal = CountTotalResults();
                g_findResultIdx = (needRecount || textChanged) ? CalcResultIndex()
                    : ((g_findResultIdx <= 1) ? g_findResultTotal : g_findResultIdx - 1);
                ScrollToFindResult(hWnd);
                UpdateFindLabel();
                RECT cr0; GetClientRect(hWnd, &cr0); cr0.top = TOOLBAR_HEIGHT;
                InvalidateRect(hWnd, &cr0, FALSE);
                return;
            }
        }
    }

    if (!g_findHandle) return;

    BOOL found = forward ? FPDFText_FindNext(g_findHandle) : FPDFText_FindPrev(g_findHandle);

    if (!found)
    {
        FPDFText_FindClose(g_findHandle);
        g_findHandle = nullptr;

        int pagesSearched = 0;
        int pg = g_findPage;

        while (pagesSearched < g_pageCount)
        {
            pg = forward ? (pg + 1) % g_pageCount : (pg - 1 + g_pageCount) % g_pageCount;
            ++pagesSearched;

            if (pg < (int)g_findPageCounts.size() && g_findPageCounts[pg] == 0)
                continue;

            FPDF_PAGE tmpPage = FPDF_LoadPage(g_pdfDoc, pg);
            if (!tmpPage) continue;
            FPDF_TEXTPAGE tmpText = FPDFText_LoadPage(tmpPage);
            if (!tmpText) { FPDF_ClosePage(tmpPage); continue; }

            FPDF_SCHHANDLE tmpHandle = FPDFText_FindStart(
                tmpText,
                reinterpret_cast<const unsigned short*>(g_findText),
                g_findCaseSensitive ? FPDF_MATCHCASE : 0, 0);

            if (tmpHandle)
            {
                if (forward)
                {
                    found = FPDFText_FindNext(tmpHandle);
                    if (found)
                    {
                        g_findCharIndex = FPDFText_GetSchResultIndex(tmpHandle);
                        g_findCharCount = FPDFText_GetSchCount(tmpHandle);
                        g_findPage = pg;
                    }
                }
                else
                {
                    int lastIdx = -1, lastCount = 0;
                    while (FPDFText_FindNext(tmpHandle))
                    {
                        lastIdx   = FPDFText_GetSchResultIndex(tmpHandle);
                        lastCount = FPDFText_GetSchCount(tmpHandle);
                    }
                    if (lastIdx >= 0)
                    {
                        found = TRUE;
                        g_findCharIndex = lastIdx;
                        g_findCharCount = lastCount;
                        g_findPage = pg;
                    }
                }
                FPDFText_FindClose(tmpHandle);
            }

            FPDFText_ClosePage(tmpText);
            FPDF_ClosePage(tmpPage);

            if (found) break;
        }

        if (found)
        {
            LoadPage(hWnd, g_findPage);
            g_findHandle = FPDFText_FindStart(
                g_textPage,
                reinterpret_cast<const unsigned short*>(g_findText),
                g_findCaseSensitive ? FPDF_MATCHCASE : 0, 0);
            if (g_findHandle)
            {
                while (FPDFText_FindNext(g_findHandle))
                {
                    if (FPDFText_GetSchResultIndex(g_findHandle) == g_findCharIndex)
                        break;
                }
            }
        }
    }
    else
    {
        g_findCharIndex = FPDFText_GetSchResultIndex(g_findHandle);
        g_findCharCount = FPDFText_GetSchCount(g_findHandle);
    }

    if (found && g_findHandle)
    {
        if (needRecount)
            g_findResultTotal = CountTotalResults();

        if (needRecount || textChanged)
            g_findResultIdx = CalcResultIndex();
        else if (forward)
            g_findResultIdx = (g_findResultIdx % g_findResultTotal) + 1;
        else
            g_findResultIdx = (g_findResultIdx <= 1) ? g_findResultTotal : g_findResultIdx - 1;

        ScrollToFindResult(hWnd);
    }
    else
    {
        g_findCharIndex = -1;
        g_findCharCount = 0;
        g_findResultIdx = 0;
        if (needRecount)
            g_findResultTotal = CountTotalResults();
    }

    UpdateFindLabel();
    RECT cr; GetClientRect(hWnd, &cr);
    cr.top = TOOLBAR_HEIGHT;
    InvalidateRect(hWnd, &cr, FALSE);
}

// ─── 찾기 바 UI ──────────────────────────────────────────────────────────────

// ─── 버튼 서브클래스 (hover 갱신) ────────────────────────────────────────────
static WNDPROC g_origBtnProc = nullptr;
static LRESULT CALLBACK FindBtnProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_MOUSEMOVE)
    {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    else if (msg == WM_MOUSELEAVE)
    {
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    return CallWindowProcW(g_origBtnProc, hWnd, msg, wParam, lParam);
}

// 에디트 서브클래스 프로시저 (Enter/Esc 처리)
static LRESULT CALLBACK FindEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN)
    {
        if (wParam == VK_RETURN)
        {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            FindNext(GetAncestor(hWnd, GA_ROOT), !shift);
            return 0;
        }
        if (wParam == VK_ESCAPE)
        {
            HWND hMain = GetAncestor(hWnd, GA_ROOT);
            HideFindBar(hMain);
            return 0;
        }
    }
    if (msg == WM_CHAR && wParam == VK_RETURN)
        return 0;  // 비프음 방지
    return CallWindowProcW(g_origEditProc, hWnd, msg, wParam, lParam);
}

// ─── 슬라이드 애니메이션 ───
static const UINT_PTR FP_ANIM_TIMER = 0x4650;
static const ULONGLONG FP_ANIM_MS   = 160;
static bool       s_fpAnimating = false;
static bool       s_fpHiding    = false;
static int        s_fpFromX     = 0;
static ULONGLONG  s_fpStart     = 0;

static int FindPanelTargetX(HWND hParent, int* outY)
{
    RECT cr; GetClientRect(hParent, &cr);
    if (outY) *outY = GetBelowToolbarY() + FINDPANEL_MARGIN;
    return cr.right - FINDPANEL_W - FINDPANEL_MARGIN;
}

static void StartFindPanelAnim(HWND hParent, bool hiding)
{
    int y;
    int targetX = FindPanelTargetX(hParent, &y);
    RECT cr; GetClientRect(hParent, &cr);

    int fromX;
    if (IsWindowVisible(g_hFindPanel))
    {
        RECT wr; GetWindowRect(g_hFindPanel, &wr);
        POINT p = { wr.left, wr.top };
        ScreenToClient(hParent, &p);
        fromX = p.x;
    }
    else
        fromX = cr.right;

    s_fpHiding    = hiding;
    s_fpFromX     = fromX;
    s_fpStart     = GetTickCount64();
    s_fpAnimating = true;

    SetWindowPos(g_hFindPanel, nullptr, fromX, y, FINDPANEL_W, FINDPANEL_H,
        SWP_NOZORDER | SWP_NOACTIVATE | (hiding ? 0 : SWP_SHOWWINDOW));
    SetTimer(g_hFindPanel, FP_ANIM_TIMER, 15, nullptr);
}

static void StepFindPanelAnim(HWND hPanel)
{
    HWND hParent = GetParent(hPanel);
    int y;
    int targetX = FindPanelTargetX(hParent, &y);
    RECT cr; GetClientRect(hParent, &cr);
    int endX = s_fpHiding ? cr.right : targetX;

    double t = (double)(GetTickCount64() - s_fpStart) / (double)FP_ANIM_MS;
    if (t > 1.0) t = 1.0;
    double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    int x = s_fpFromX + (int)((endX - s_fpFromX) * e);

    SetWindowPos(hPanel, nullptr, x, y, FINDPANEL_W, FINDPANEL_H,
        SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(hPanel);

    if (t >= 1.0)
    {
        KillTimer(hPanel, FP_ANIM_TIMER);
        s_fpAnimating = false;
        if (s_fpHiding) ShowWindow(hPanel, SW_HIDE);
    }
}

void ShowFindBar(HWND hWnd)
{
    if (!g_hFindPanel)
        CreateFindPanel(hWnd);

    g_findBarVisible = true;
    if (!IsWindowVisible(g_hFindPanel) || s_fpAnimating)
        StartFindPanelAnim(hWnd, false);
    else
        RepositionFindPanel(hWnd);
    SetFocus(g_hFindEdit);
    SendMessage(g_hFindEdit, EM_SETSEL, 0, -1);
}

void HideFindBar(HWND hWnd)
{
    g_findBarVisible = false;
    if (g_hFindPanel)
    {
        if (IsWindowVisible(g_hFindPanel))
            StartFindPanelAnim(hWnd, true);
        else
            ShowWindow(g_hFindPanel, SW_HIDE);
    }
    FindClose();
    g_findText[0] = 0;
    if (g_hFindEdit)
        SetWindowTextW(g_hFindEdit, L"");
    UpdateFindLabel();
    SetFocus(hWnd);
    InvalidateRect(hWnd, nullptr, TRUE);
}

// ─── 플로팅 검색 패널 ─────────────────────────────────────────────────────────

void RepositionFindPanel(HWND hWnd)
{
    if (!g_hFindPanel) return;
    if (s_fpAnimating) return;
    RECT cr; GetClientRect(hWnd, &cr);
    int panelX = cr.right - FINDPANEL_W - FINDPANEL_MARGIN;
    int panelY = GetBelowToolbarY() + FINDPANEL_MARGIN;
    MoveWindow(g_hFindPanel, panelX, panelY, FINDPANEL_W, FINDPANEL_H, TRUE);
}

static LRESULT CALLBACK FindPanelProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TIMER && wParam == FP_ANIM_TIMER)
    {
        StepFindPanelAnim(hWnd);
        return 0;
    }
    if (msg == WM_COMMAND)
    {
        HWND hMain = GetAncestor(hWnd, GA_ROOT);
        if ((HWND)lParam == g_hFindCaseChk)
        {
            g_findCaseSensitive = (SendMessage(g_hFindCaseChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (g_findHandle) { FPDFText_FindClose(g_findHandle); g_findHandle = nullptr; }
            g_findPage = -1; g_findCharIndex = -1; g_findCharCount = 0;
            g_findResultIdx = 0; g_findResultTotal = 0; g_findText[0] = 0;
            return 0;
        }
        SendMessageW(hMain, WM_COMMAND, wParam, lParam);
        return 0;
    }
    if (msg == WM_SETCURSOR)
    {
        if ((HWND)wParam == g_hFindEdit)
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    }
    if (msg == WM_CTLCOLOREDIT)
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, FP_EDIT_FG);
        SetBkColor(hdc, FP_EDIT_BG);
        return (LRESULT)g_hFindEditBrush;
    }
    if (msg == WM_CTLCOLORSTATIC)
    {
        HDC hdc = (HDC)wParam;
        HWND hCtl = (HWND)lParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, (hCtl == g_hFindLabel) ? FP_LABEL_FG : FP_CHK_FG);
        return (LRESULT)g_hFindPanelBrush;
    }
    if (msg == WM_DRAWITEM)
    {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lParam;
        bool pressed = (di->itemState & ODS_SELECTED) != 0;
        POINT cur; GetCursorPos(&cur); ScreenToClient(di->hwndItem, &cur);
        RECT rc; GetClientRect(di->hwndItem, &rc);
        bool hovered = PtInRect(&rc, cur) != 0;

        if (pressed || hovered)
        {
            COLORREF bgClr = pressed ? FP_BTN_PRESS : FP_BTN_HOVER;
            HBRUSH hBr = CreateSolidBrush(bgClr);
            FillRect(di->hDC, &rc, hBr);
            DeleteObject(hBr);
        }
        else
        {
            FillRect(di->hDC, &rc, g_hFindPanelBrush);
        }

        if (di->hwndItem == g_hFindPrevBtn)
            DrawPngIcon(di->hDC, IDB_FINDPREV_PNG, rc, 13, true);
        else if (di->hwndItem == g_hFindNextBtn)
            DrawPngIcon(di->hDC, IDB_FINDNEXT_PNG, rc, 13, true);
        else
        {
            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, RGB(175, 170, 190));
            HFONT hF = (HFONT)SendMessage(di->hwndItem, WM_GETFONT, 0, 0);
            HFONT oF = (HFONT)SelectObject(di->hDC, hF);
            DrawTextW(di->hDC, L"✕", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(di->hDC, oF);
        }
        return TRUE;
    }
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT cr; GetClientRect(hWnd, &cr);

        FillRect(hdc, &cr, g_hFindPanelBrush);

        // 외곽 테두리
        HPEN hPen = CreatePen(PS_SOLID, 1, FP_BORDER);
        HPEN oPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH oNullBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, cr.left, cr.top, cr.right - 1, cr.bottom - 1, 8, 8);
        SelectObject(hdc, oPen); SelectObject(hdc, oNullBr);
        DeleteObject(hPen);

        // 1·2줄 구분선
        HPEN hSep = CreatePen(PS_SOLID, 1, FP_SEP);
        HPEN oSep = (HPEN)SelectObject(hdc, hSep);
        MoveToEx(hdc, FP_PAD, FP_ROW2_Y - 5, nullptr);
        LineTo(hdc, cr.right - FP_PAD, FP_ROW2_Y - 5);
        SelectObject(hdc, oSep);
        DeleteObject(hSep);

        EndPaint(hWnd, &ps);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void CreateFindPanel(HWND hParent)
{
    if (g_hFindPanel) return;

    g_hFindPanelBrush = CreateSolidBrush(FP_BG);
    g_hFindEditBrush  = CreateSolidBrush(FP_EDIT_BG);

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = FindPanelProc;
        wc.hInstance     = hInst;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"FindPanel";
        RegisterClassExW(&wc);
        registered = true;
    }

    RECT cr; GetClientRect(hParent, &cr);
    int panelX = cr.right - FINDPANEL_W - FINDPANEL_MARGIN;
    int panelY = GetBelowToolbarY() + FINDPANEL_MARGIN;

    g_hFindPanel = CreateWindowExW(
        WS_EX_NOPARENTNOTIFY,
        L"FindPanel", nullptr,
        WS_CHILD | WS_CLIPSIBLINGS,
        panelX, panelY, FINDPANEL_W, FINDPANEL_H,
        hParent, nullptr, hInst, nullptr);

    HFONT hFont   = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT hFontSm = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    int x = FP_PAD;

    // 검색어 에디트
    g_hFindEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        x, FP_ROW1_Y, FP_EDIT_W, FP_BTN_H,
        g_hFindPanel, (HMENU)(INT_PTR)IDC_FINDBAR_EDIT, hInst, nullptr);
    SendMessage(g_hFindEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    x += FP_EDIT_W + FP_GAP + 2;

    // 결과 라벨
    g_hFindLabel = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_RIGHT,
        x, FP_ROW1_Y, FP_LABEL_W, FP_BTN_H,
        g_hFindPanel, (HMENU)(INT_PTR)IDC_FINDBAR_LABEL, hInst, nullptr);
    SendMessage(g_hFindLabel, WM_SETFONT, (WPARAM)hFontSm, TRUE);
    x += FP_LABEL_W + FP_GAP;

    // 이전 버튼
    g_hFindPrevBtn = CreateWindowExW(
        0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        x, FP_ROW1_Y, FP_BTN_W, FP_BTN_H,
        g_hFindPanel, (HMENU)(INT_PTR)IDM_FIND_PREV, hInst, nullptr);
    x += FP_BTN_W + FP_GAP;

    // 다음 버튼
    g_hFindNextBtn = CreateWindowExW(
        0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        x, FP_ROW1_Y, FP_BTN_W, FP_BTN_H,
        g_hFindPanel, (HMENU)(INT_PTR)IDM_FIND_NEXT, hInst, nullptr);
    x += FP_BTN_W + FP_GAP;

    // 닫기 버튼
    g_hFindCloseBtn = CreateWindowExW(
        0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        x, FP_ROW1_Y, FP_BTN_W, FP_BTN_H,
        g_hFindPanel, (HMENU)(INT_PTR)IDM_FIND, hInst, nullptr);
    SendMessage(g_hFindCloseBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

    // 대소문자 구분 체크박스
    g_hFindCaseChk = CreateWindowExW(
        0, L"BUTTON", LS(STR_FIND_CASE),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        FP_PAD, FP_ROW2_Y, 120, 17,
        g_hFindPanel, (HMENU)(INT_PTR)(IDC_FINDBAR_LABEL + 1), hInst, nullptr);
    SendMessage(g_hFindCaseChk, WM_SETFONT, (WPARAM)hFontSm, TRUE);
    SendMessage(g_hFindCaseChk, BM_SETCHECK, g_findCaseSensitive ? BST_CHECKED : BST_UNCHECKED, 0);

    // 에디트 서브클래싱
    g_origEditProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hFindEdit, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(FindEditProc)));

    // 버튼 서브클래싱 (hover 갱신)
    g_origBtnProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hFindPrevBtn, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(FindBtnProc)));
    SetWindowLongPtrW(g_hFindNextBtn,  GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(FindBtnProc));
    SetWindowLongPtrW(g_hFindCloseBtn, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(FindBtnProc));

    // 포커스 점선 제거
    WPARAM hideUIS = MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS);
    SendMessage(g_hFindPrevBtn,  WM_UPDATEUISTATE, hideUIS, 0);
    SendMessage(g_hFindNextBtn,  WM_UPDATEUISTATE, hideUIS, 0);
    SendMessage(g_hFindCloseBtn, WM_UPDATEUISTATE, hideUIS, 0);
    SendMessage(g_hFindCaseChk,  WM_UPDATEUISTATE, hideUIS, 0);
}

void DrawFindHighlight(HDC hdc, const RECT& clientRect)
{
    if (!g_textPage || !g_pdfPage || g_findCharIndex < 0 || g_findCharCount <= 0) return;
    if (g_pageLayouts.empty()) return;

    const RECT   vp = GetViewportRect(clientRect);
    const double pageW = g_pageLayouts[g_currentPage].pdfW;
    const double pageH = g_pageLayouts[g_currentPage].pdfH;
    int          pgLeft, pgTop;
    GetPageScreenPos(g_currentPage, vp, &pgLeft, &pgTop);

    const int nRects = FPDFText_CountRects(g_textPage, g_findCharIndex, g_findCharCount);
    for (int ri = 0; ri < nRects; ++ri)
    {
        double rLeft, rTop, rRight, rBottom;
        FPDFText_GetRect(g_textPage, ri, &rLeft, &rTop, &rRight, &rBottom);

        int sx1, sy1, sx2, sy2;
        PdfToScreen(rLeft, rTop, pageW, pageH, pgLeft, pgTop, g_zoom, g_rotation, &sx1, &sy1);
        PdfToScreen(rRight, rBottom, pageW, pageH, pgLeft, pgTop, g_zoom, g_rotation, &sx2, &sy2);
        if (sx1 > sx2) std::swap(sx1, sx2);
        if (sy1 > sy2) std::swap(sy1, sy2);

        int padY = std::max(2, (sy2 - sy1) * 15 / 100);
        sy1 -= padY;
        sy2 += padY;

        int padX = std::max(2, (sy2 - sy1) * 8 / 100);
        sx1 -= padX;
        sx2 += padX;

        int cx1 = std::max(sx1, (int)vp.left);
        int cy1 = std::max(sy1, (int)vp.top);
        int cx2 = std::min(sx2, (int)vp.right);
        int cy2 = std::min(sy2, (int)vp.bottom);
        if (cx2 <= cx1 || cy2 <= cy1) continue;

        const int bw = cx2 - cx1, bh = cy2 - cy1;
        HDC     memDC = CreateCompatibleDC(hdc);
        HBITMAP hBmp  = CreateCompatibleBitmap(hdc, bw, bh);
        HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, hBmp));
        HBRUSH  hBr = CreateSolidBrush(RGB(80, 180, 255));
        RECT    fr = { 0, 0, bw, bh };
        FillRect(memDC, &fr, hBr);
        DeleteObject(hBr);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 140, 0 };
        AlphaBlend(hdc, cx1, cy1, bw, bh, memDC, 0, 0, bw, bh, bf);
        SelectObject(memDC, oldBmp);
        DeleteObject(hBmp);
        DeleteDC(memDC);

        HPEN pen = CreatePen(PS_SOLID, 2, RGB(50, 150, 225));
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
        Rectangle(hdc, sx1, sy1, sx2, sy2);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);
    }
}
