// ConfirmDialog.cpp : 확인/취소 팝업 (CleanPopup 스타일)
//

#include "framework.h"
#include "ConfirmDialog.h"
#include "Lang.h"
#include <windowsx.h>
#include <objbase.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

// ── 내부 상태 ─────────────────────────────────────────────────────────────────
static HWND  g_cfmWnd            = nullptr;
static bool  g_cfmResult         = false;
static bool  g_cfmModalDone      = false;
static bool  g_cfmOkHover        = false;
static bool  g_cfmCancelHover    = false;
static bool  g_cfmOkPressed      = false;
static bool  g_cfmCancelPressed  = false;
static std::wstring g_cfmMessage;

// ── 레이아웃 상수 ─────────────────────────────────────────────────────────────
static constexpr int CFM_W   = 340;
static constexpr int CFM_H   = 160;
static constexpr int CFM_BTN_W   = 100;
static constexpr int CFM_BTN_H   = 34;
static constexpr int CFM_BTN_GAP = 10;

// ── 버튼 영역 계산 ────────────────────────────────────────────────────────────
static RECT GetCfmOkBtnRect()
{
    int left = (CFM_W - (CFM_BTN_W * 2 + CFM_BTN_GAP)) / 2;
    int top  = CFM_H - 20 - CFM_BTN_H;
    return { left, top, left + CFM_BTN_W, top + CFM_BTN_H };
}
static RECT GetCfmCancelBtnRect()
{
    int left = (CFM_W - (CFM_BTN_W * 2 + CFM_BTN_GAP)) / 2 + CFM_BTN_W + CFM_BTN_GAP;
    int top  = CFM_H - 20 - CFM_BTN_H;
    return { left, top, left + CFM_BTN_W, top + CFM_BTN_H };
}

// ── 닫기 (페이드 아웃) ────────────────────────────────────────────────────────
static void CloseCfmWnd(HWND hWnd)
{
    for (int a = 255; a >= 0; a -= 40)
    {
        SetLayeredWindowAttributes(hWnd, 0, (BYTE)a, LWA_ALPHA);
        UpdateWindow(hWnd);
        Sleep(8);
    }
    DestroyWindow(hWnd);
}

// ── GDI+ 둥근 사각형 경로 ─────────────────────────────────────────────────────
static Gdiplus::GraphicsPath* CfmMakeRoundRectPath(const Gdiplus::RectF& r, float radius)
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

// ── GDI+ 버튼 그리기 ──────────────────────────────────────────────────────────
static void DrawCfmButton(Gdiplus::Graphics& g, const RECT& rc,
                           bool isPrimary, bool hovered, const wchar_t* label)
{
    using namespace Gdiplus;
    FontFamily ff(L"Segoe UI");

    RectF btnRF((float)rc.left  + 0.5f, (float)rc.top + 0.5f,
                (float)(rc.right - rc.left) - 1.0f,
                (float)(rc.bottom - rc.top) - 1.0f);
    GraphicsPath* path = CfmMakeRoundRectPath(btnRF, 5.0f);

    if (isPrimary)
    {
        // 삭제 확인 버튼 → 빨간 계열
        Color top = hovered ? Color(255, 220, 50,  50)  : Color(255, 200, 30,  30);
        Color bot = hovered ? Color(255, 180, 20,  20)  : Color(255, 160, 10,  10);
        LinearGradientBrush grad(PointF(btnRF.X, btnRF.Y),
                                 PointF(btnRF.X, btnRF.Y + btnRF.Height), top, bot);
        g.FillPath(&grad, path);
        Pen hl(Color(55, 255, 200, 200), 1.0f);
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
static void DrawCfmDialog(HWND hWnd, HDC hdc)
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
    g.DrawString(LS(STR_CFM_TITLE), -1, &titleFont, titleRF, nullptr, &darkBr);

    // 구분선
    Pen linePen(Color(255, 235, 235, 235), 1.0f);
    g.DrawLine(&linePen, 20.0f, 54.0f, (float)(W - 20), 54.0f);

    // 본문 메시지
    Font bodyFont(&ff, 9, FontStyleRegular, UnitPoint);
    SolidBrush grayBr(Color(255, 60, 60, 60));
    StringFormat sfWrap;
    sfWrap.SetAlignment(StringAlignmentNear);
    sfWrap.SetLineAlignment(StringAlignmentNear);
    RectF msgRF(20, 64, (float)(W - 40), 46);
    g.DrawString(g_cfmMessage.c_str(), -1, &bodyFont, msgRF, &sfWrap, &grayBr);

    // 버튼
    RECT okRc = GetCfmOkBtnRect();
    RECT caRc = GetCfmCancelBtnRect();
    DrawCfmButton(g, caRc, false, g_cfmCancelHover, LS(STR_CFM_BTN_CANCEL));
    DrawCfmButton(g, okRc, true,  g_cfmOkHover,     LS(STR_CFM_BTN_OK));
}

// ── 팝업 창 프로시저 ──────────────────────────────────────────────────────────
static LRESULT CALLBACK CfmWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT cr; GetClientRect(hWnd, &cr);
        HDC memDC   = CreateCompatibleDC(hdc);
        HBITMAP bm  = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
        HBITMAP obm = (HBITMAP)SelectObject(memDC, bm);
        FillRect(memDC, &cr, (HBRUSH)GetStockObject(WHITE_BRUSH));
        DrawCfmDialog(hWnd, memDC);
        BitBlt(hdc, 0, 0, cr.right, cr.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, obm);
        DeleteObject(bm);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        RECT okRc = GetCfmOkBtnRect(), caRc = GetCfmCancelBtnRect();
        bool okH = PtInRect(&okRc, {x,y}) != 0;
        bool caH = PtInRect(&caRc, {x,y}) != 0;
        if (okH != g_cfmOkHover || caH != g_cfmCancelHover)
        {
            g_cfmOkHover = okH; g_cfmCancelHover = caH;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        SetCursor(LoadCursor(nullptr, (okH || caH) ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_cfmOkHover = g_cfmCancelHover = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        RECT okRc = GetCfmOkBtnRect(), caRc = GetCfmCancelBtnRect();
        g_cfmOkPressed     = PtInRect(&okRc, {x,y}) != 0;
        g_cfmCancelPressed = PtInRect(&caRc, {x,y}) != 0;
        return 0;
    }

    case WM_LBUTTONUP:
    {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        RECT okRc = GetCfmOkBtnRect(), caRc = GetCfmCancelBtnRect();
        if (g_cfmOkPressed     && PtInRect(&okRc, {x,y}))
            SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
        else if (g_cfmCancelPressed && PtInRect(&caRc, {x,y}))
            SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
        g_cfmOkPressed = g_cfmCancelPressed = false;
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_RETURN)
            SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
        else if (wParam == VK_ESCAPE)
            SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            g_cfmResult = true;
            CloseCfmWnd(hWnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            g_cfmResult = false;
            CloseCfmWnd(hWnd);
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
            RECT okRc = GetCfmOkBtnRect(), caRc = GetCfmCancelBtnRect();
            if (!PtInRect(&okRc, pt) && !PtInRect(&caRc, pt))
                return HTCAPTION;
        }
        return hit;
    }

    case WM_ACTIVATE:
        return 0;

    case WM_DESTROY:
        g_cfmWnd      = nullptr;
        g_cfmModalDone = true;
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ── ShowConfirmDialog ─────────────────────────────────────────────────────────
bool ShowConfirmDialog(HWND hWndParent, const wchar_t* message)
{
    g_cfmMessage  = message ? message : L"";
    g_cfmResult   = false;
    g_cfmModalDone = false;
    g_cfmOkHover = g_cfmCancelHover = false;
    g_cfmOkPressed = g_cfmCancelPressed = false;

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = CfmWndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"ConfirmPopup";
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
        registered = true;
    }

    RECT rcParent; GetWindowRect(hWndParent, &rcParent);
    int x = rcParent.left + (rcParent.right  - rcParent.left - CFM_W) / 2;
    int y = rcParent.top  + (rcParent.bottom - rcParent.top  - CFM_H) / 2;

    g_cfmWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"ConfirmPopup", L"", WS_POPUP,
        x, y, CFM_W, CFM_H,
        hWndParent, nullptr, hInst, nullptr);
    if (!g_cfmWnd) return false;

    // 둥근 모서리 (Windows 11)
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_cfmWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    // 그림자
    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(g_cfmWnd, &margins);

    // 페이드 인
    SetLayeredWindowAttributes(g_cfmWnd, 0, 0, LWA_ALPHA);
    ShowWindow(g_cfmWnd, SW_SHOW);
    UpdateWindow(g_cfmWnd);
    for (int a = 0; a <= 255; a += 30)
    {
        SetLayeredWindowAttributes(g_cfmWnd, 0, (BYTE)(a < 255 ? a : 255), LWA_ALPHA);
        UpdateWindow(g_cfmWnd);
        Sleep(8);
    }
    SetLayeredWindowAttributes(g_cfmWnd, 0, 255, LWA_ALPHA);
    SetFocus(g_cfmWnd);

    // 모달 메시지 루프
    MSG m;
    while (!g_cfmModalDone && GetMessage(&m, nullptr, 0, 0))
    {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE)
        {
            g_cfmResult = false;
            CloseCfmWnd(g_cfmWnd);
            continue;
        }
        // 부모 창으로 가는 입력 차단
        if (m.hwnd == hWndParent || IsChild(hWndParent, m.hwnd))
        {
            if ((m.message >= WM_MOUSEFIRST && m.message <= WM_MOUSELAST) ||
                (m.message >= WM_KEYFIRST   && m.message <= WM_KEYLAST)   ||
                m.message == WM_NCLBUTTONDOWN || m.message == WM_NCRBUTTONDOWN)
            {
                if (m.message == WM_LBUTTONDOWN || m.message == WM_NCLBUTTONDOWN)
                    FlashWindow(g_cfmWnd, TRUE);
                continue;
            }
        }
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    SetForegroundWindow(hWndParent);
    SetFocus(hWndParent);
    return g_cfmResult;
}
