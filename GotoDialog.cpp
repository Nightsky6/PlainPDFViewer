// GotoDialog.cpp : 페이지 이동 팝업 다이얼로그 (Ctrl+G) - CleanPopup 스타일
//

#include "framework.h"
#include "GotoDialog.h"
#include "Lang.h"
#include <windowsx.h>
#include <objbase.h>
#include <dwmapi.h>
#include <gdiplus.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

// ── 내부 상태 ─────────────────────────────────────────────────────────────────
static HWND    g_gotoWnd              = nullptr;
static HWND    g_gotoEdit             = nullptr;
static int     g_gotoResult           = 0;
static bool    g_gotoBtnOkHover       = false;
static bool    g_gotoBtnCancelHover   = false;
static bool    g_gotoBtnOkPressed     = false;
static bool    g_gotoBtnCancelPressed = false;
static HFONT   g_gotoFontTitle        = nullptr;
static HFONT   g_gotoFontBody         = nullptr;
static WNDPROC g_gotoEditOrigProc     = nullptr;

// ── 레이아웃 상수 ─────────────────────────────────────────────────────────────
static constexpr int DLG_W   = 340;
static constexpr int DLG_H   = 196;
static constexpr int BTN_W   = 100;
static constexpr int BTN_H   = 34;
static constexpr int BTN_GAP = 10;

// ── GDI+ 둥근 사각형 경로 ─────────────────────────────────────────────────────
static Gdiplus::GraphicsPath* MakeRoundRectPath(const Gdiplus::RectF& r, float radius)
{
    using namespace Gdiplus;
    auto* path = new GraphicsPath();
    float d = radius * 2.0f;
    path->AddArc(r.X,             r.Y,              d, d, 180, 90);
    path->AddArc(r.X + r.Width-d, r.Y,              d, d, 270, 90);
    path->AddArc(r.X + r.Width-d, r.Y + r.Height-d, d, d,   0, 90);
    path->AddArc(r.X,             r.Y + r.Height-d, d, d,  90, 90);
    path->CloseFigure();
    return path;
}

// ── 버튼 영역 계산 ────────────────────────────────────────────────────────────
static RECT GetGotoOkBtnRect()
{
    int left = (DLG_W - (BTN_W * 2 + BTN_GAP)) / 2;
    int top  = DLG_H - 20 - BTN_H;
    return { left, top, left + BTN_W, top + BTN_H };
}
static RECT GetGotoCancelBtnRect()
{
    int left = (DLG_W - (BTN_W * 2 + BTN_GAP)) / 2 + BTN_W + BTN_GAP;
    int top  = DLG_H - 20 - BTN_H;
    return { left, top, left + BTN_W, top + BTN_H };
}

// ── 닫기 (페이드 아웃) ────────────────────────────────────────────────────────
static void CloseGotoWnd(HWND hWnd)
{
    for (int a = 255; a >= 0; a -= 40)
    {
        SetLayeredWindowAttributes(hWnd, 0, (BYTE)a, LWA_ALPHA);
        UpdateWindow(hWnd);
        Sleep(8);
    }
    DestroyWindow(hWnd);
}

// ── GDI+ 버튼 그리기 ──────────────────────────────────────────────────────────
static void DrawGotoButton(Gdiplus::Graphics& g, const RECT& rc,
                            bool isPrimary, bool hovered, const wchar_t* label)
{
    using namespace Gdiplus;
    FontFamily ff(L"Segoe UI");

    RectF btnRF((float)rc.left + 0.5f, (float)rc.top + 0.5f,
                (float)(rc.right - rc.left) - 1.0f,
                (float)(rc.bottom - rc.top) - 1.0f);
    GraphicsPath* path = MakeRoundRectPath(btnRF, 5.0f);

    if (isPrimary)
    {
        Color top = hovered ? Color(255, 14, 148, 244) : Color(255, 0, 137, 230);
        Color bot = hovered ? Color(255, 0, 100, 185)  : Color(255, 0, 108, 196);
        LinearGradientBrush grad(PointF(btnRF.X, btnRF.Y),
                                 PointF(btnRF.X, btnRF.Y + btnRF.Height), top, bot);
        g.FillPath(&grad, path);
        Pen hl(Color(55, 255, 255, 255), 1.0f);
        g.DrawLine(&hl, btnRF.X + 5, btnRF.Y + 1.5f,
                        btnRF.X + btnRF.Width - 5, btnRF.Y + 1.5f);
    }
    else
    {
        Color top = hovered ? Color(255, 252, 252, 252) : Color(255, 255, 255, 255);
        Color bot = hovered ? Color(255, 230, 230, 230) : Color(255, 242, 242, 242);
        LinearGradientBrush grad(PointF(btnRF.X, btnRF.Y),
                                 PointF(btnRF.X, btnRF.Y + btnRF.Height), top, bot);
        g.FillPath(&grad, path);
        Pen bdr(Color(255, 200, 200, 200), 1.0f);
        g.DrawPath(&bdr, path);
    }
    delete path;

    SolidBrush txtBr(isPrimary ? Color(255, 255, 255, 255) : Color(255, 45, 45, 45));
    Font btnFont(&ff, 9, FontStyleBold, UnitPoint);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(label, -1, &btnFont, btnRF, &sf, &txtBr);
}

// ── 팝업 전체 렌더링 ──────────────────────────────────────────────────────────
static void DrawGotoDialog(HWND hWnd, HDC hdc)
{
    using namespace Gdiplus;

    RECT cr; GetClientRect(hWnd, &cr);
    int W = cr.right, H = cr.bottom;

    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // 흰 배경
    SolidBrush bgBr(Color(255, 255, 255, 255));
    g.FillRectangle(&bgBr, 0.0f, 0.0f, (float)W, (float)H);

    // 테두리
    Pen borderPen(Color(255, 218, 218, 218), 1.0f);
    g.DrawRectangle(&borderPen, 0.5f, 0.5f, (float)W - 1.0f, (float)H - 1.0f);

    FontFamily ff(L"Segoe UI");

    // 제목
    Font titleFont(&ff, 11, FontStyleBold, UnitPoint);
    SolidBrush darkBr(Color(255, 20, 20, 20));
    RectF titleRF(20, 22, (float)(W - 40), 28);
    g.DrawString(LS(STR_GOTO_TITLE), -1, &titleFont, titleRF, nullptr, &darkBr);

    // 서브텍스트
    Font subFont(&ff, 9, FontStyleRegular, UnitPoint);
    SolidBrush grayBr(Color(255, 95, 95, 95));
    WCHAR sub[64];
    swprintf_s(sub, LS(STR_GOTO_PROMPT_FMT), g_pageCount);
    StringFormat sfLeft;
    sfLeft.SetAlignment(StringAlignmentNear);
    sfLeft.SetLineAlignment(StringAlignmentNear);
    RectF subRF(20, 52, (float)(W - 40), 22);
    g.DrawString(sub, -1, &subFont, subRF, &sfLeft, &grayBr);

    // 구분선
    Pen linePen(Color(255, 235, 235, 235), 1.0f);
    g.DrawLine(&linePen, 20.0f, 78.0f, (float)(W - 20), 78.0f);

    // 버튼
    RECT okRc = GetGotoOkBtnRect();
    RECT caRc = GetGotoCancelBtnRect();
    DrawGotoButton(g, caRc, false, g_gotoBtnCancelHover, LS(STR_GOTO_BTN_CANCEL));
    DrawGotoButton(g, okRc, true,  g_gotoBtnOkHover,     LS(STR_GOTO_BTN_OK));
}

// ── 에디트 서브클래스 (Enter→이동, Esc→취소) ─────────────────────────────────
static LRESULT CALLBACK GotoEditSubProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN)
    {
        if (wParam == VK_RETURN) { SendMessage(GetParent(hEdit), WM_COMMAND, MAKEWPARAM(IDOK, 0), 0); return 0; }
        if (wParam == VK_ESCAPE) { SendMessage(GetParent(hEdit), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0); return 0; }
    }
    if (msg == WM_CHAR && wParam == VK_RETURN) return 0;
    return CallWindowProc(g_gotoEditOrigProc, hEdit, msg, wParam, lParam);
}

// ── 팝업 창 프로시저 ──────────────────────────────────────────────────────────
static LRESULT CALLBACK GotoWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_gotoFontTitle = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_gotoFontBody  = CreateFontW(-15, 0, 0, 0, FW_NORMAL,   FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        int editX = 20, editY = 86, editW = DLG_W - 40, editH = 34;
        g_gotoEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER | WS_TABSTOP,
            editX, editY, editW, editH,
            hWnd, (HMENU)102, hInst, nullptr);
        SendMessage(g_gotoEdit, WM_SETFONT, (WPARAM)g_gotoFontBody, TRUE);

        // 텍스트 세로 중앙 정렬
        {
            HDC hdc = GetDC(g_gotoEdit);
            HFONT hOldF = (HFONT)SelectObject(hdc, g_gotoFontBody);
            TEXTMETRIC tm; GetTextMetrics(hdc, &tm);
            SelectObject(hdc, hOldF);
            ReleaseDC(g_gotoEdit, hdc);
            // WS_EX_CLIENTEDGE 는 위아래 각 2px 테두리를 차지하므로 실제 클라이언트 높이는 editH - 4
            int clientH = editH - 4;
            int textH   = tm.tmHeight - tm.tmInternalLeading;
            int topPad  = (clientH - textH) / 2;
            RECT fmtRc = { 4, topPad, editW - 4, topPad + textH };
            SendMessage(g_gotoEdit, EM_SETRECTNP, 0, (LPARAM)&fmtRc);
        }

        WCHAR buf[16]; swprintf_s(buf, L"%d", g_currentPage + 1);
        SetWindowTextW(g_gotoEdit, buf);
        SendMessage(g_gotoEdit, EM_SETSEL, 0, -1);

        g_gotoEditOrigProc = (WNDPROC)SetWindowLongPtr(g_gotoEdit, GWLP_WNDPROC, (LONG_PTR)GotoEditSubProc);
        SetFocus(g_gotoEdit);
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT cr; GetClientRect(hWnd, &cr);
        HDC memDC   = CreateCompatibleDC(hdc);
        HBITMAP bm  = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
        HBITMAP obm = (HBITMAP)SelectObject(memDC, bm);
        FillRect(memDC, &cr, (HBRUSH)GetStockObject(WHITE_BRUSH));
        DrawGotoDialog(hWnd, memDC);
        BitBlt(hdc, 0, 0, cr.right, cr.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, obm);
        DeleteObject(bm);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT:
    {
        HDC editDC = (HDC)wParam;
        SetTextColor(editDC, RGB(20, 20, 20));
        SetBkColor(editDC, RGB(255, 255, 255));
        static HBRUSH wBr = CreateSolidBrush(RGB(255, 255, 255));
        return (LRESULT)wBr;
    }

    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        RECT okRc = GetGotoOkBtnRect(), caRc = GetGotoCancelBtnRect();
        bool okH = PtInRect(&okRc, {x,y}) != 0;
        bool caH = PtInRect(&caRc, {x,y}) != 0;
        if (okH != g_gotoBtnOkHover || caH != g_gotoBtnCancelHover)
        {
            g_gotoBtnOkHover = okH; g_gotoBtnCancelHover = caH;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        SetCursor(LoadCursor(nullptr, (okH || caH) ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_gotoBtnOkHover = g_gotoBtnCancelHover = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        RECT okRc = GetGotoOkBtnRect(), caRc = GetGotoCancelBtnRect();
        g_gotoBtnOkPressed     = PtInRect(&okRc, {x,y}) != 0;
        g_gotoBtnCancelPressed = PtInRect(&caRc, {x,y}) != 0;
        return 0;
    }

    case WM_LBUTTONUP:
    {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        RECT okRc = GetGotoOkBtnRect(), caRc = GetGotoCancelBtnRect();
        if (g_gotoBtnOkPressed     && PtInRect(&okRc, {x,y}))
            SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
        else if (g_gotoBtnCancelPressed && PtInRect(&caRc, {x,y}))
            SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
        g_gotoBtnOkPressed = g_gotoBtnCancelPressed = false;
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            WCHAR buf[16] = {}; GetWindowTextW(g_gotoEdit, buf, 16);
            int page = _wtoi(buf);
            if (page >= 1 && page <= g_pageCount)
            {
                g_gotoResult = page;
                CloseGotoWnd(hWnd);
            }
            else
            {
                MessageBeep(MB_ICONWARNING);
                SetFocus(g_gotoEdit);
                SendMessage(g_gotoEdit, EM_SETSEL, 0, -1);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            g_gotoResult = 0;
            CloseGotoWnd(hWnd);
            return 0;
        }
        break;

    case WM_NCHITTEST:
    {
        LRESULT hit = DefWindowProc(hWnd, msg, wParam, lParam);
        if (hit == HTCLIENT)
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            RECT okRc = GetGotoOkBtnRect(), caRc = GetGotoCancelBtnRect();
            if (!PtInRect(&okRc, pt) && !PtInRect(&caRc, pt) &&
                !(g_gotoEdit && ChildWindowFromPoint(hWnd, pt) == g_gotoEdit))
                return HTCAPTION;
        }
        return hit;
    }

    case WM_ACTIVATE:
        // EnableWindow 없이 메시지 루프로 모달 처리하므로 포커스 이동 시 닫지 않음
        return 0;

    case WM_DESTROY:
        if (g_gotoFontTitle) { DeleteObject(g_gotoFontTitle); g_gotoFontTitle = nullptr; }
        if (g_gotoFontBody)  { DeleteObject(g_gotoFontBody);  g_gotoFontBody  = nullptr; }
        g_gotoEdit = nullptr;
        g_gotoWnd  = nullptr;
        g_gotoModalDone = true;
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ── ShowGotoPageDialog ────────────────────────────────────────────────────────
void ShowGotoPageDialog(HWND hWnd)
{
    if (!g_pdfDoc) return;
    if (g_gotoWnd) { SetFocus(g_gotoWnd); return; }

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = GotoWndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"GotoPagePopup";
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
        registered = true;
    }

    RECT rcParent; GetWindowRect(hWnd, &rcParent);
    int x = rcParent.left + (rcParent.right  - rcParent.left - DLG_W) / 2;
    int y = rcParent.top  + (rcParent.bottom - rcParent.top  - DLG_H) / 2;

    g_gotoResult = 0;
    g_gotoBtnOkHover = g_gotoBtnCancelHover = false;
    g_gotoBtnOkPressed = g_gotoBtnCancelPressed = false;

    g_gotoWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"GotoPagePopup", L"", WS_POPUP,
        x, y, DLG_W, DLG_H,
        hWnd, nullptr, hInst, nullptr);

    // 둥근 모서리 (Windows 11)
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_gotoWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    // 그림자
    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(g_gotoWnd, &margins);

    // 페이드 인
    SetLayeredWindowAttributes(g_gotoWnd, 0, 0, LWA_ALPHA);
    ShowWindow(g_gotoWnd, SW_SHOW);
    UpdateWindow(g_gotoWnd);
    for (int a = 0; a <= 255; a += 30)
    {
        SetLayeredWindowAttributes(g_gotoWnd, 0, (BYTE)(a < 255 ? a : 255), LWA_ALPHA);
        UpdateWindow(g_gotoWnd);
        Sleep(8);
    }
    SetLayeredWindowAttributes(g_gotoWnd, 0, 255, LWA_ALPHA);

    g_gotoModalDone = false;
    MSG m;
    while (!g_gotoModalDone && GetMessage(&m, nullptr, 0, 0))
    {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE)
        {
            g_gotoResult = 0;
            CloseGotoWnd(g_gotoWnd);
            continue;
        }
        // 메인 창으로 가는 마우스/키 입력 차단 (EnableWindow 없이)
        if (m.hwnd == hWnd || IsChild(hWnd, m.hwnd))
        {
            if ((m.message >= WM_MOUSEFIRST && m.message <= WM_MOUSELAST) ||
                (m.message >= WM_KEYFIRST   && m.message <= WM_KEYLAST)   ||
                m.message == WM_NCLBUTTONDOWN || m.message == WM_NCRBUTTONDOWN)
            {
                if (m.message == WM_LBUTTONDOWN || m.message == WM_NCLBUTTONDOWN)
                    FlashWindow(g_gotoWnd, TRUE);
                continue;
            }
        }
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

    if (g_gotoResult >= 1 && g_gotoResult <= g_pageCount)
        ScrollToPage(hWnd, g_gotoResult - 1);
}
