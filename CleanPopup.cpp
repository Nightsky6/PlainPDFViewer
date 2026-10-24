// ---------------------------------------------------------------
// CleanPopup.cpp - 재사용 가능한 Fluent 스타일 팝업 구현
// ---------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include "CleanPopup.h"
#include "Lang.h"
#include "Strings.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------
// 내부 상수
// ---------------------------------------------------------------
namespace
{
    constexpr int POPUP_W  = 380;
    constexpr int POPUP_H  = 240;
    constexpr int BTN_W    = 100;
    constexpr int BTN_H    = 36;

    const wchar_t* POPUP_CLASS_NAME = L"CleanPopupClass";

    // GDI+ 토큰
    ULONG_PTR g_gdiplusToken = 0;
    HINSTANCE g_hInstance     = nullptr;
    bool      g_initialized  = false;

    // 캐싱된 커서 핸들
    HCURSOR g_curArrow = nullptr;
    HCURSOR g_curHand  = nullptr;

    // 버튼 상태
    struct PopupButton {
        RECT rc{};
        bool hovered = false;
        bool pressed = false;
        std::wstring label;
        COLORREF color      = 0;
        COLORREF hoverColor = 0;
    };

    struct PopupState {
        std::wstring title;
        std::wstring message;
        PopupButton  okBtn;
        PopupButton  cancelBtn;
        bool         hasCancel = false;
        bool         result    = false;
        bool         closed    = false;
    } g_popup;

    // 전방 선언
    LRESULT CALLBACK PopupProc(HWND, UINT, WPARAM, LPARAM);
    void DrawPopup(HWND hwnd, HDC hdc);
    void ClosePopup(HWND hWnd);

    // ---------------------------------------------------------------
    // GDI+ 헬퍼: 둥근 사각형 path 생성
    // ---------------------------------------------------------------
    Gdiplus::GraphicsPath* MakeRoundRectPath(const Gdiplus::RectF& r, float radius)
    {
        using namespace Gdiplus;
        auto* path = new GraphicsPath();
        float d = radius * 2.0f;
        path->AddArc(r.X, r.Y, d, d, 180, 90);
        path->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        path->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        path->CloseFigure();
        return path;
    }

    // ---------------------------------------------------------------
    // 팝업 렌더링
    // ---------------------------------------------------------------
    void DrawPopup(HWND hwnd, HDC hdc)
    {
        using namespace Gdiplus;

        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        int W = clientRc.right;
        int H = clientRc.bottom;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // 배경
        SolidBrush bgBrush(Color(255, 255, 255, 255));
        g.FillRectangle(&bgBrush, 0.0f, 0.0f, (float)W, (float)H);

        // 테두리
        Pen borderPen(Color(255, 218, 218, 218), 1.0f);
        g.DrawRectangle(&borderPen, 0.5f, 0.5f, (float)W - 1.0f, (float)H - 1.0f);

        FontFamily fontFamily(L"Segoe UI");

        // 제목
        Font titleFont(&fontFamily, 11, FontStyleBold, UnitPoint);
        SolidBrush darkBrush(Color(255, 20, 20, 20));
        RectF titleRect(20, 22, (float)(W - 40), 28);
        g.DrawString(g_popup.title.c_str(), -1, &titleFont,
            titleRect, nullptr, &darkBrush);

        // 메시지
        Font msgFont(&fontFamily, 9, FontStyleRegular, UnitPoint);
        SolidBrush grayBrush(Color(255, 95, 95, 95));
        RectF msgRect(20, 58, (float)(W - 40), (float)(H - 125));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentNear);
        sf.SetLineAlignment(StringAlignmentNear);
        g.DrawString(g_popup.message.c_str(), -1, &msgFont,
            msgRect, &sf, &grayBrush);

        // 버튼 그리기
        auto DrawBtn = [&](const PopupButton& btn) {
            if (btn.rc.left == 0 && btn.rc.right == 0) return;

            RectF btnRectF(
                (float)btn.rc.left + 0.5f,
                (float)btn.rc.top + 0.5f,
                (float)(btn.rc.right - btn.rc.left) - 1.0f,
                (float)(btn.rc.bottom - btn.rc.top) - 1.0f
            );

            bool isPrimary = (btn.color == RGB(0, 120, 212));
            GraphicsPath* btnPath = MakeRoundRectPath(btnRectF, 4.0f);

            if (isPrimary) {
                Color topColor = btn.hovered ? Color(255, 14, 148, 244) : Color(255, 0, 137, 230);
                Color botColor = btn.hovered ? Color(255, 0, 100, 185)  : Color(255, 0, 108, 196);
                LinearGradientBrush grad(
                    PointF(btnRectF.X, btnRectF.Y),
                    PointF(btnRectF.X, btnRectF.Y + btnRectF.Height),
                    topColor, botColor);
                g.FillPath(&grad, btnPath);
                Pen highlightPen(Color(55, 255, 255, 255), 1.0f);
                g.DrawLine(&highlightPen,
                    btnRectF.X + 5, btnRectF.Y + 1.5f,
                    btnRectF.X + btnRectF.Width - 5, btnRectF.Y + 1.5f);
            }
            else {
                Color topColor = btn.hovered ? Color(255, 252, 252, 252) : Color(255, 255, 255, 255);
                Color botColor = btn.hovered ? Color(255, 230, 230, 230) : Color(255, 242, 242, 242);
                LinearGradientBrush grad(
                    PointF(btnRectF.X, btnRectF.Y),
                    PointF(btnRectF.X, btnRectF.Y + btnRectF.Height),
                    topColor, botColor);
                g.FillPath(&grad, btnPath);
                Pen bdrPen(Color(255, 200, 200, 200), 1.0f);
                g.DrawPath(&bdrPen, btnPath);
            }

            delete btnPath;

            SolidBrush txtBrush(isPrimary ? Color(255, 255, 255, 255)
                : Color(255, 45, 45, 45));
            Font btnFont(&fontFamily, 9, FontStyleBold, UnitPoint);
            StringFormat btnSf;
            btnSf.SetAlignment(StringAlignmentCenter);
            btnSf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(btn.label.c_str(), -1, &btnFont,
                btnRectF, &btnSf, &txtBrush);
            };

        if (g_popup.hasCancel) DrawBtn(g_popup.cancelBtn);
        DrawBtn(g_popup.okBtn);
    }

    // ---------------------------------------------------------------
    // 팝업 닫기
    // ---------------------------------------------------------------
    void ClosePopup(HWND hWnd)
    {
        HWND parent = GetParent(hWnd);
        // 수동 페이드아웃 (parent 무효화 방지)
        for (int alpha = 255; alpha >= 0; alpha -= 30)
        {
            SetLayeredWindowAttributes(hWnd, 0, (BYTE)alpha, LWA_ALPHA);
            UpdateWindow(hWnd);
            Sleep(10);
        }
        ShowWindow(hWnd, SW_HIDE);
        DestroyWindow(hWnd);
        EnableWindow(parent, TRUE);
        SetFocus(parent);
    }

    // ---------------------------------------------------------------
    // 팝업 WndProc
    // ---------------------------------------------------------------
    LRESULT CALLBACK PopupProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rc;
            GetClientRect(hWnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);

            FillRect(memDC, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
            DrawPopup(hWnd, memDC);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBM);
            DeleteObject(memBM);
            DeleteDC(memDC);

            EndPaint(hWnd, &ps);
        }
        break;

        case WM_MOUSEMOVE:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool changed = false;

            auto UpdateHover = [&](PopupButton& btn) {
                bool over = PtInRect(&btn.rc, pt);
                if (over != btn.hovered) {
                    btn.hovered = over;
                    changed = true;
                    SetCursor(over ? g_curHand : g_curArrow);
                }
                };

            UpdateHover(g_popup.okBtn);
            if (g_popup.hasCancel) UpdateHover(g_popup.cancelBtn);

            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);

            if (changed) {
                InvalidateRect(hWnd, &g_popup.okBtn.rc, FALSE);
                if (g_popup.hasCancel) InvalidateRect(hWnd, &g_popup.cancelBtn.rc, FALSE);
            }
        }
        break;

        case WM_MOUSELEAVE:
        {
            g_popup.okBtn.hovered = false;
            if (g_popup.hasCancel) g_popup.cancelBtn.hovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;

        case WM_LBUTTONDOWN:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PtInRect(&g_popup.okBtn.rc, pt))
                g_popup.okBtn.pressed = true;
            if (g_popup.hasCancel && PtInRect(&g_popup.cancelBtn.rc, pt))
                g_popup.cancelBtn.pressed = true;
        }
        break;

        case WM_LBUTTONUP:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            bool okClicked = g_popup.okBtn.pressed && PtInRect(&g_popup.okBtn.rc, pt);
            bool cancelClicked = g_popup.hasCancel && g_popup.cancelBtn.pressed
                && PtInRect(&g_popup.cancelBtn.rc, pt);

            if (okClicked || cancelClicked)
            {
                g_popup.result = okClicked;
                g_popup.closed = true;
                ClosePopup(hWnd);
            }

            g_popup.okBtn.pressed = false;
            g_popup.cancelBtn.pressed = false;
        }
        break;

        case WM_NCHITTEST:
        {
            LRESULT hit = DefWindowProc(hWnd, message, wParam, lParam);
            if (hit == HTCLIENT)
            {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hWnd, &pt);
                if (!PtInRect(&g_popup.okBtn.rc, pt) &&
                    !(g_popup.hasCancel && PtInRect(&g_popup.cancelBtn.rc, pt)))
                    return HTCAPTION;
            }
            return hit;
        }

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        return 0;
    }

} // anonymous namespace

// ---------------------------------------------------------------
// 공개 API 구현
// ---------------------------------------------------------------

bool CleanPopup::Init(HINSTANCE hInstance)
{
    if (g_initialized) return true;

    g_hInstance = hInstance;

    // GDI+ 초기화
    Gdiplus::GdiplusStartupInput gdiplusInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr);

    // 커서 캐싱
    g_curArrow = LoadCursor(nullptr, IDC_ARROW);
    g_curHand  = LoadCursor(nullptr, IDC_HAND);

    // 팝업 윈도우 클래스 등록
    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEX);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = PopupProc;
    wcex.hInstance      = hInstance;
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = nullptr;
    wcex.lpszClassName  = POPUP_CLASS_NAME;

    if (!RegisterClassExW(&wcex)) return false;

    g_initialized = true;
    return true;
}

bool CleanPopup::Show(HWND parent, const std::wstring& title,
                      const std::wstring& msg, bool hasCancel)
{
    if (!g_initialized) return false;

    // 상태 초기화
    g_popup.title     = title;
    g_popup.message   = msg;
    g_popup.hasCancel = hasCancel;
    g_popup.result    = false;
    g_popup.closed    = false;

    // 버튼 위치 계산
    int btnY   = POPUP_H - BTN_H - 20;
    int gap    = 12;
    int totalW = hasCancel ? (BTN_W * 2 + gap) : BTN_W;
    int startX = (POPUP_W - totalW) / 2;

    if (hasCancel) {
        g_popup.cancelBtn.rc         = { startX, btnY, startX + BTN_W, btnY + BTN_H };
        g_popup.cancelBtn.label      = LS(STR_CANCEL);
        g_popup.cancelBtn.color      = RGB(255, 255, 255);
        g_popup.cancelBtn.hoverColor = RGB(244, 244, 244);
        g_popup.cancelBtn.hovered    = false;
        g_popup.cancelBtn.pressed    = false;
    }

    int okX = startX + (hasCancel ? BTN_W + gap : 0);
    g_popup.okBtn.rc         = { okX, btnY, okX + BTN_W, btnY + BTN_H };
    g_popup.okBtn.label      = LS(STR_ABOUT_OK);
    g_popup.okBtn.color      = RGB(0, 120, 212);
    g_popup.okBtn.hoverColor = RGB(0, 100, 180);
    g_popup.okBtn.hovered    = false;
    g_popup.okBtn.pressed    = false;

    // 부모 중앙 계산
    RECT parentRc;
    GetWindowRect(parent, &parentRc);
    int x = parentRc.left + (parentRc.right  - parentRc.left - POPUP_W) / 2;
    int y = parentRc.top  + (parentRc.bottom - parentRc.top  - POPUP_H) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        POPUP_CLASS_NAME, nullptr, WS_POPUP,
        x, y, POPUP_W, POPUP_H,
        parent, nullptr, g_hInstance, nullptr
    );

    if (!hwnd) return false;

    // 둥근 모서리 (Windows 11)
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corner, sizeof(corner));

    // 그림자
    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // 페이드 인
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    AnimateWindow(hwnd, 180, AW_BLEND);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    // 모달 메시지 루프 (EnableWindow 없이 입력 필터링)
    MSG msg2;
    while (!g_popup.closed && GetMessage(&msg2, nullptr, 0, 0))
    {
        // parent로 가는 마우스/키보드 입력 차단
        if (msg2.hwnd == parent || IsChild(parent, msg2.hwnd))
        {
            if ((msg2.message >= WM_MOUSEFIRST && msg2.message <= WM_MOUSELAST) ||
                (msg2.message >= WM_KEYFIRST && msg2.message <= WM_KEYLAST) ||
                msg2.message == WM_NCLBUTTONDOWN || msg2.message == WM_NCRBUTTONDOWN)
            {
                // parent 클릭 시 팝업 깜빡임으로 알림
                if (msg2.message == WM_LBUTTONDOWN || msg2.message == WM_NCLBUTTONDOWN)
                    FlashWindow(hwnd, TRUE);
                continue;
            }
        }
        TranslateMessage(&msg2);
        DispatchMessage(&msg2);
    }

    return g_popup.result;
}

// ---------------------------------------------------------------
// About 팝업 전용 상태/렌더링
// ---------------------------------------------------------------
namespace
{
    constexpr int ABOUT_W   = 360;
    constexpr int ABOUT_H   = 310;
    constexpr int ABOUT_ICON = 64;

    constexpr UINT_PTR ABOUT_FADEIN_TIMER  = 401;
    constexpr UINT_PTR ABOUT_FADEOUT_TIMER = 402;
    constexpr int      ABOUT_FADE_INTERVAL = 16;   // ~60fps
    constexpr int      ABOUT_FADEIN_MS     = 220;
    constexpr int      ABOUT_FADEOUT_MS    = 300;

    const wchar_t* ABOUT_CLASS_NAME = L"CleanPopupAboutClass";
    bool g_aboutClassRegistered = false;

    struct AboutState {
        std::wstring appName;
        std::wstring version;
        std::wstring copyright;
        int          pngResId    = 0;
        PopupButton  okBtn;
        PopupButton  privacyBtn;
        bool         closed      = false;
        int          fadeElapsed = 0;
        bool         fadingIn    = false;
        bool         fadingOut   = false;
    } g_about;

    LRESULT CALLBACK AboutProc(HWND, UINT, WPARAM, LPARAM);

    Gdiplus::Image* LoadPngFromResource(int resId)
    {
        HRSRC hRes = FindResourceW(g_hInstance, MAKEINTRESOURCE(resId), RT_RCDATA);
        if (!hRes) return nullptr;
        DWORD sz = SizeofResource(g_hInstance, hRes);
        if (!sz) return nullptr;
        HGLOBAL hMem = LoadResource(g_hInstance, hRes);
        if (!hMem) return nullptr;
        void* pData = LockResource(hMem);
        if (!pData) return nullptr;

        HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, sz);
        if (!hBuf) return nullptr;
        void* pBuf = GlobalLock(hBuf);
        if (!pBuf) { GlobalFree(hBuf); return nullptr; }
        memcpy(pBuf, pData, sz);
        GlobalUnlock(hBuf);

        IStream* pStream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(hBuf, TRUE, &pStream)) || !pStream) { GlobalFree(hBuf); return nullptr; }

        auto* img = new Gdiplus::Image(pStream);
        pStream->Release();
        return img;
    }

    void DrawAbout(HWND hwnd, HDC hdc)
    {
        using namespace Gdiplus;

        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        int W = clientRc.right;
        int H = clientRc.bottom;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // 배경
        SolidBrush bgBrush(Color(255, 255, 255, 255));
        g.FillRectangle(&bgBrush, 0.0f, 0.0f, (float)W, (float)H);

        // 테두리
        Pen borderPen(Color(255, 218, 218, 218), 1.0f);
        g.DrawRectangle(&borderPen, 0.5f, 0.5f, (float)W - 1.0f, (float)H - 1.0f);

        FontFamily fontFamily(L"Segoe UI");

        // 아이콘
        int iconX = (W - ABOUT_ICON) / 2;
        int iconY = 30;
        Image* pImg = LoadPngFromResource(g_about.pngResId);
        if (pImg) {
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            RectF iconRc((float)iconX, (float)iconY, (float)ABOUT_ICON, (float)ABOUT_ICON);
            g.DrawImage(pImg, iconRc);
            delete pImg;
        }

        // 앱 이름
        Font nameFont(&fontFamily, 14, FontStyleBold, UnitPoint);
        SolidBrush darkBrush(Color(255, 20, 20, 20));
        StringFormat cf;
        cf.SetAlignment(StringAlignmentCenter);
        cf.SetLineAlignment(StringAlignmentNear);
        RectF nameRect(0, (float)(iconY + ABOUT_ICON + 14), (float)W, 30);
        g.DrawString(g_about.appName.c_str(), -1, &nameFont, nameRect, &cf, &darkBrush);

        // 버전
        Font verFont(&fontFamily, 9, FontStyleRegular, UnitPoint);
        SolidBrush grayBrush(Color(255, 120, 120, 120));
        RectF verRect(0, nameRect.Y + 32, (float)W, 20);
        g.DrawString(g_about.version.c_str(), -1, &verFont, verRect, &cf, &grayBrush);

        // 구분선
        Pen linePen(Color(255, 230, 230, 230), 1.0f);
        float lineY = verRect.Y + 28;
        g.DrawLine(&linePen, 40.0f, lineY, (float)(W - 40), lineY);

        // 저작권
        Font cpFont(&fontFamily, 8, FontStyleRegular, UnitPoint);
        SolidBrush cpBrush(Color(255, 150, 150, 150));
        RectF cpRect(0, lineY + 10, (float)W, 20);
        g.DrawString(g_about.copyright.c_str(), -1, &cpFont, cpRect, &cf, &cpBrush);

        // 확인 버튼
        const PopupButton& btn = g_about.okBtn;
        RectF btnRectF(
            (float)btn.rc.left + 0.5f,
            (float)btn.rc.top + 0.5f,
            (float)(btn.rc.right - btn.rc.left) - 1.0f,
            (float)(btn.rc.bottom - btn.rc.top) - 1.0f
        );

        GraphicsPath* btnPath = MakeRoundRectPath(btnRectF, 4.0f);
        Color topColor = btn.hovered ? Color(255, 14, 148, 244) : Color(255, 0, 137, 230);
        Color botColor = btn.hovered ? Color(255, 0, 100, 185)  : Color(255, 0, 108, 196);
        LinearGradientBrush grad(
            PointF(btnRectF.X, btnRectF.Y),
            PointF(btnRectF.X, btnRectF.Y + btnRectF.Height),
            topColor, botColor);
        g.FillPath(&grad, btnPath);
        Pen highlightPen(Color(55, 255, 255, 255), 1.0f);
        g.DrawLine(&highlightPen,
            btnRectF.X + 5, btnRectF.Y + 1.5f,
            btnRectF.X + btnRectF.Width - 5, btnRectF.Y + 1.5f);
        delete btnPath;

        SolidBrush txtBrush(Color(255, 255, 255, 255));
        Font btnFont(&fontFamily, 9, FontStyleBold, UnitPoint);
        StringFormat btnSf;
        btnSf.SetAlignment(StringAlignmentCenter);
        btnSf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(btn.label.c_str(), -1, &btnFont, btnRectF, &btnSf, &txtBrush);

        // 개인정보처리방침 버튼
        const PopupButton& pbtn = g_about.privacyBtn;
        RectF pbtnRectF(
            (float)pbtn.rc.left  + 0.5f,
            (float)pbtn.rc.top   + 0.5f,
            (float)(pbtn.rc.right  - pbtn.rc.left) - 1.0f,
            (float)(pbtn.rc.bottom - pbtn.rc.top)  - 1.0f
        );
        GraphicsPath* pbtnPath = MakeRoundRectPath(pbtnRectF, 4.0f);
        Color pbgColor = pbtn.hovered ? Color(255, 220, 220, 235) : Color(255, 240, 240, 245);
        SolidBrush pbgBrush(pbgColor);
        g.FillPath(&pbgBrush, pbtnPath);
        Pen pbrdPen(Color(255, 180, 180, 210), 1.0f);
        g.DrawPath(&pbrdPen, pbtnPath);
        delete pbtnPath;
        SolidBrush ptxtBrush(Color(255, 50, 50, 160));
        Font pbtnFont(&fontFamily, 8, FontStyleRegular, UnitPoint);
        g.DrawString(pbtn.label.c_str(), -1, &pbtnFont, pbtnRectF, &btnSf, &ptxtBrush);
    }

    LRESULT CALLBACK AboutProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_TIMER:
        {
            if (wParam == ABOUT_FADEIN_TIMER)
            {
                g_about.fadeElapsed += ABOUT_FADE_INTERVAL;
                if (g_about.fadeElapsed >= ABOUT_FADEIN_MS)
                {
                    KillTimer(hWnd, ABOUT_FADEIN_TIMER);
                    g_about.fadingIn    = false;
                    g_about.fadeElapsed = 0;
                    SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
                    return 0;
                }
                float t = (float)g_about.fadeElapsed / ABOUT_FADEIN_MS;
                t = t * t * (3.0f - 2.0f * t);
                SetLayeredWindowAttributes(hWnd, 0, (BYTE)(255 * t), LWA_ALPHA);
                return 0;
            }
            if (wParam == ABOUT_FADEOUT_TIMER)
            {
                g_about.fadeElapsed += ABOUT_FADE_INTERVAL;
                if (g_about.fadeElapsed >= ABOUT_FADEOUT_MS)
                {
                    KillTimer(hWnd, ABOUT_FADEOUT_TIMER);
                    g_about.fadingOut = false;
                    g_about.closed    = true;
                    ShowWindow(hWnd, SW_HIDE);
                    HWND parent = GetParent(hWnd);
                    DestroyWindow(hWnd);
                    SetForegroundWindow(parent);
                    SetFocus(parent);
                    return 0;
                }
                float t = (float)g_about.fadeElapsed / ABOUT_FADEOUT_MS;
                t = t * t * (3.0f - 2.0f * t);
                SetLayeredWindowAttributes(hWnd, 0, (BYTE)(255 * (1.0f - t)), LWA_ALPHA);
                return 0;
            }
            break;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);
            FillRect(memDC, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
            DrawAbout(hWnd, memDC);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBM);
            DeleteObject(memBM);
            DeleteDC(memDC);
            EndPaint(hWnd, &ps);
        }
        break;

        case WM_MOUSEMOVE:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool over = PtInRect(&g_about.okBtn.rc, pt);
            if (over != g_about.okBtn.hovered) {
                g_about.okBtn.hovered = over;
                SetCursor(over ? g_curHand : g_curArrow);
                InvalidateRect(hWnd, &g_about.okBtn.rc, FALSE);
            }
            bool pover = PtInRect(&g_about.privacyBtn.rc, pt);
            if (pover != g_about.privacyBtn.hovered) {
                g_about.privacyBtn.hovered = pover;
                SetCursor(pover ? g_curHand : g_curArrow);
                InvalidateRect(hWnd, &g_about.privacyBtn.rc, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;

        case WM_MOUSELEAVE:
            g_about.okBtn.hovered = false;
            g_about.privacyBtn.hovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case WM_LBUTTONDOWN:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PtInRect(&g_about.okBtn.rc, pt))
                g_about.okBtn.pressed = true;
            if (PtInRect(&g_about.privacyBtn.rc, pt))
                g_about.privacyBtn.pressed = true;
        }
        break;

        case WM_LBUTTONUP:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (g_about.privacyBtn.pressed && PtInRect(&g_about.privacyBtn.rc, pt)) {
                ShellExecuteW(nullptr, L"open",
                    L"https://nightskyworks.app/plainpdfviewer/privacy-policy.html",
                    nullptr, nullptr, SW_SHOWNORMAL);
                // 브라우저가 위로 올라올 수 있도록 TOPMOST 해제 후 복원
                SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
            g_about.privacyBtn.pressed = false;
            if (g_about.okBtn.pressed && PtInRect(&g_about.okBtn.rc, pt)) {
                g_about.fadingOut   = true;
                g_about.fadeElapsed = 0;
                SetTimer(hWnd, ABOUT_FADEOUT_TIMER, ABOUT_FADE_INTERVAL, nullptr);
            }
            g_about.okBtn.pressed = false;
        }
        break;

        case WM_NCHITTEST:
        {
            LRESULT hit = DefWindowProc(hWnd, message, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hWnd, &pt);
                if (!PtInRect(&g_about.okBtn.rc, pt) && !PtInRect(&g_about.privacyBtn.rc, pt))
                    return HTCAPTION;
            }
            return hit;
        }

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        return 0;
    }
} // about namespace

void CleanPopup::ShowAbout(HWND parent, int pngResourceId,
                           const std::wstring& appName,
                           const std::wstring& version,
                           const std::wstring& copyright)
{
    if (!g_initialized) return;

    // About 윈도우 클래스 등록 (최초 1회)
    if (!g_aboutClassRegistered) {
        WNDCLASSEXW wcex = {};
        wcex.cbSize        = sizeof(WNDCLASSEX);
        wcex.style         = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc   = AboutProc;
        wcex.hInstance      = g_hInstance;
        wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground  = nullptr;
        wcex.lpszClassName  = ABOUT_CLASS_NAME;
        RegisterClassExW(&wcex);
        g_aboutClassRegistered = true;
    }

    g_about.appName   = appName;
    g_about.version   = version;
    g_about.copyright = copyright;
    g_about.pngResId  = pngResourceId;
    g_about.closed    = false;

    int btnY = ABOUT_H - BTN_H - 20;
    int okX  = (ABOUT_W - BTN_W) / 2;
    g_about.okBtn.rc      = { okX, btnY, okX + BTN_W, btnY + BTN_H };
    g_about.okBtn.label   = LS(STR_ABOUT_OK);
    g_about.okBtn.color   = RGB(0, 120, 212);
    g_about.okBtn.hovered = false;
    g_about.okBtn.pressed = false;

    // 개인정보처리방침 버튼 (확인 버튼 위에 배치)
    constexpr int PB_W = 160, PB_H = 24;
    int pbX = (ABOUT_W - PB_W) / 2;
    int pbY = btnY - PB_H - 10;
    g_about.privacyBtn.rc      = { pbX, pbY, pbX + PB_W, pbY + PB_H };
    g_about.privacyBtn.label   = LS(STR_ABOUT_PRIVACY);
    g_about.privacyBtn.hovered = false;
    g_about.privacyBtn.pressed = false;

    RECT parentRc;
    GetWindowRect(parent, &parentRc);
    int x = parentRc.left + (parentRc.right  - parentRc.left - ABOUT_W) / 2;
    int y = parentRc.top  + (parentRc.bottom - parentRc.top  - ABOUT_H) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        ABOUT_CLASS_NAME, nullptr, WS_POPUP,
        x, y, ABOUT_W, ABOUT_H,
        parent, nullptr, g_hInstance, nullptr
    );

    if (!hwnd) return;

    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    g_about.fadingIn    = true;
    g_about.fadeElapsed = 0;
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    SetTimer(hwnd, ABOUT_FADEIN_TIMER, ABOUT_FADE_INTERVAL, nullptr);

    // 모달 메시지 루프 (EnableWindow 없이 입력 필터링)
    MSG msg2;
    while (!g_about.closed && GetMessage(&msg2, nullptr, 0, 0))
    {
        if (msg2.hwnd == parent || IsChild(parent, msg2.hwnd))
        {
            if ((msg2.message >= WM_MOUSEFIRST && msg2.message <= WM_MOUSELAST) ||
                (msg2.message >= WM_KEYFIRST && msg2.message <= WM_KEYLAST) ||
                msg2.message == WM_NCLBUTTONDOWN || msg2.message == WM_NCRBUTTONDOWN)
            {
                if (msg2.message == WM_LBUTTONDOWN || msg2.message == WM_NCLBUTTONDOWN)
                    FlashWindow(hwnd, TRUE);
                continue;
            }
        }
        TranslateMessage(&msg2);
        DispatchMessage(&msg2);
    }
}

// ---------------------------------------------------------------
// License 팝업 전용 상태/렌더링
// ---------------------------------------------------------------
namespace
{
    constexpr int LIC_W       = 640;
    constexpr int LIC_H       = 520;
    constexpr int LIC_HEADER  = 72;
    constexpr int LIC_FOOTER  = 56;
    constexpr int LIC_ICON    = 40;
    constexpr int LIC_MARGIN  = 20;

    constexpr UINT_PTR LIC_FADEIN_TIMER  = 501;
    constexpr UINT_PTR LIC_FADEOUT_TIMER = 502;
    constexpr int      LIC_FADE_INTERVAL = 16;   // ~60fps
    constexpr int      LIC_FADEIN_MS     = 220;
    constexpr int      LIC_FADEOUT_MS    = 300;

    const wchar_t* LICENSE_CLASS_NAME = L"CleanPopupLicenseClass";
    bool g_licClassRegistered = false;

    struct LicenseState {
        std::wstring title;
        std::wstring subtitle;
        int          pngResId = 0;
        PopupButton  okBtn;
        bool         closed      = false;
        HWND         hEdit       = nullptr;
        HFONT        hEditFont   = nullptr;
        int          fadeElapsed = 0;
        bool         fadingIn    = false;
        bool         fadingOut   = false;
    } g_lic;

    LRESULT CALLBACK LicenseProc(HWND, UINT, WPARAM, LPARAM);

    void DrawLicensePopup(HWND hwnd, HDC hdc)
    {
        using namespace Gdiplus;

        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        int W = clientRc.right;
        int H = clientRc.bottom;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // 배경
        SolidBrush bgBrush(Color(255, 255, 255, 255));
        g.FillRectangle(&bgBrush, 0.0f, 0.0f, (float)W, (float)H);

        // 테두리
        Pen borderPen(Color(255, 218, 218, 218), 1.0f);
        g.DrawRectangle(&borderPen, 0.5f, 0.5f, (float)W - 1.0f, (float)H - 1.0f);

        FontFamily fontFamily(L"Segoe UI");

        // 아이콘
        int iconX = LIC_MARGIN;
        int iconY = (LIC_HEADER - LIC_ICON) / 2;
        Image* pImg = LoadPngFromResource(g_lic.pngResId);
        if (pImg) {
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            RectF iconRc((float)iconX, (float)iconY, (float)LIC_ICON, (float)LIC_ICON);
            g.DrawImage(pImg, iconRc);
            delete pImg;
        }

        int textLeft = iconX + LIC_ICON + 12;

        // 제목
        Font titleFont(&fontFamily, 12, FontStyleBold, UnitPoint);
        SolidBrush darkBrush(Color(255, 20, 20, 20));
        RectF titleRect((float)textLeft, (float)(iconY + 2), (float)(W - textLeft - LIC_MARGIN), 22);
        g.DrawString(g_lic.title.c_str(), -1, &titleFont, titleRect, nullptr, &darkBrush);

        // 서브타이틀
        Font subFont(&fontFamily, 8, FontStyleRegular, UnitPoint);
        SolidBrush grayBrush(Color(255, 130, 130, 140));
        RectF subRect((float)textLeft, titleRect.Y + 22, titleRect.Width, 16);
        g.DrawString(g_lic.subtitle.c_str(), -1, &subFont, subRect, nullptr, &grayBrush);

        // 헤더 구분선
        Pen sepPen(Color(255, 230, 230, 230), 1.0f);
        g.DrawLine(&sepPen, (float)LIC_MARGIN, (float)LIC_HEADER - 1,
                   (float)(W - LIC_MARGIN), (float)LIC_HEADER - 1);

        // 푸터 구분선
        float footerY = (float)(H - LIC_FOOTER);
        g.DrawLine(&sepPen, (float)LIC_MARGIN, footerY,
                   (float)(W - LIC_MARGIN), footerY);

        // 닫기 버튼
        const PopupButton& btn = g_lic.okBtn;
        RectF btnRectF(
            (float)btn.rc.left + 0.5f,
            (float)btn.rc.top + 0.5f,
            (float)(btn.rc.right - btn.rc.left) - 1.0f,
            (float)(btn.rc.bottom - btn.rc.top) - 1.0f
        );

        GraphicsPath* btnPath = MakeRoundRectPath(btnRectF, 4.0f);
        Color topColor = btn.hovered ? Color(255, 14, 148, 244) : Color(255, 0, 137, 230);
        Color botColor = btn.hovered ? Color(255, 0, 100, 185)  : Color(255, 0, 108, 196);
        LinearGradientBrush grad(
            PointF(btnRectF.X, btnRectF.Y),
            PointF(btnRectF.X, btnRectF.Y + btnRectF.Height),
            topColor, botColor);
        g.FillPath(&grad, btnPath);
        Pen highlightPen(Color(55, 255, 255, 255), 1.0f);
        g.DrawLine(&highlightPen,
            btnRectF.X + 5, btnRectF.Y + 1.5f,
            btnRectF.X + btnRectF.Width - 5, btnRectF.Y + 1.5f);
        delete btnPath;

        SolidBrush txtBrush(Color(255, 255, 255, 255));
        Font btnFont(&fontFamily, 9, FontStyleBold, UnitPoint);
        StringFormat btnSf;
        btnSf.SetAlignment(StringAlignmentCenter);
        btnSf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(btn.label.c_str(), -1, &btnFont, btnRectF, &btnSf, &txtBrush);
    }

    LRESULT CALLBACK LicenseProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_TIMER:
        {
            if (wParam == LIC_FADEIN_TIMER)
            {
                g_lic.fadeElapsed += LIC_FADE_INTERVAL;
                if (g_lic.fadeElapsed >= LIC_FADEIN_MS)
                {
                    KillTimer(hWnd, LIC_FADEIN_TIMER);
                    g_lic.fadingIn    = false;
                    g_lic.fadeElapsed = 0;
                    SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
                    return 0;
                }
                float t = (float)g_lic.fadeElapsed / LIC_FADEIN_MS;
                t = t * t * (3.0f - 2.0f * t);
                SetLayeredWindowAttributes(hWnd, 0, (BYTE)(255 * t), LWA_ALPHA);
                return 0;
            }
            if (wParam == LIC_FADEOUT_TIMER)
            {
                g_lic.fadeElapsed += LIC_FADE_INTERVAL;
                if (g_lic.fadeElapsed >= LIC_FADEOUT_MS)
                {
                    KillTimer(hWnd, LIC_FADEOUT_TIMER);
                    g_lic.fadingOut = false;
                    g_lic.closed    = true;
                    ShowWindow(hWnd, SW_HIDE);
                    if (g_lic.hEditFont) { DeleteObject(g_lic.hEditFont); g_lic.hEditFont = nullptr; }
                    HWND parent = GetParent(hWnd);
                    DestroyWindow(hWnd);
                    SetForegroundWindow(parent);
                    SetFocus(parent);
                    return 0;
                }
                float t = (float)g_lic.fadeElapsed / LIC_FADEOUT_MS;
                t = t * t * (3.0f - 2.0f * t);
                SetLayeredWindowAttributes(hWnd, 0, (BYTE)(255 * (1.0f - t)), LWA_ALPHA);
                return 0;
            }
            break;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);
            FillRect(memDC, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
            DrawLicensePopup(hWnd, memDC);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBM);
            DeleteObject(memBM);
            DeleteDC(memDC);
            EndPaint(hWnd, &ps);
        }
        break;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdcEdit, RGB(248, 248, 250));
            SetTextColor(hdcEdit, RGB(50, 50, 60));
            static HBRUSH hEditBg = CreateSolidBrush(RGB(248, 248, 250));
            return reinterpret_cast<LRESULT>(hEditBg);
        }

        case WM_MOUSEMOVE:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool over = PtInRect(&g_lic.okBtn.rc, pt);
            if (over != g_lic.okBtn.hovered) {
                g_lic.okBtn.hovered = over;
                SetCursor(over ? g_curHand : g_curArrow);
                InvalidateRect(hWnd, &g_lic.okBtn.rc, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;

        case WM_MOUSELEAVE:
            g_lic.okBtn.hovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case WM_LBUTTONDOWN:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PtInRect(&g_lic.okBtn.rc, pt))
                g_lic.okBtn.pressed = true;
        }
        break;

        case WM_LBUTTONUP:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (g_lic.okBtn.pressed && PtInRect(&g_lic.okBtn.rc, pt)) {
                g_lic.fadingOut   = true;
                g_lic.fadeElapsed = 0;
                SetTimer(hWnd, LIC_FADEOUT_TIMER, LIC_FADE_INTERVAL, nullptr);
            }
            g_lic.okBtn.pressed = false;
        }
        break;

        case WM_NCHITTEST:
        {
            LRESULT hit = DefWindowProc(hWnd, message, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hWnd, &pt);
                if (!PtInRect(&g_lic.okBtn.rc, pt) && pt.y < LIC_HEADER)
                    return HTCAPTION;
            }
            return hit;
        }

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        return 0;
    }
} // license namespace

void CleanPopup::ShowLicense(HWND parent, int pngResourceId,
                             const std::wstring& title,
                             const std::wstring& subtitle,
                             const std::wstring& licenseText)
{
    if (!g_initialized) return;

    if (!g_licClassRegistered) {
        WNDCLASSEXW wcex = {};
        wcex.cbSize        = sizeof(WNDCLASSEX);
        wcex.style         = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc   = LicenseProc;
        wcex.hInstance      = g_hInstance;
        wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground  = nullptr;
        wcex.lpszClassName  = LICENSE_CLASS_NAME;
        RegisterClassExW(&wcex);
        g_licClassRegistered = true;
    }

    g_lic.title    = title;
    g_lic.subtitle = subtitle;
    g_lic.pngResId = pngResourceId;
    g_lic.closed   = false;

    int btnY = LIC_H - (LIC_FOOTER + BTN_H) / 2;
    int okX  = (LIC_W - BTN_W) / 2;
    g_lic.okBtn.rc      = { okX, btnY, okX + BTN_W, btnY + BTN_H };
    g_lic.okBtn.label   = LS(STR_LIC_CLOSE);
    g_lic.okBtn.color   = RGB(0, 120, 212);
    g_lic.okBtn.hovered = false;
    g_lic.okBtn.pressed = false;

    RECT parentRc;
    GetWindowRect(parent, &parentRc);
    int x = parentRc.left + (parentRc.right  - parentRc.left - LIC_W) / 2;
    int y = parentRc.top  + (parentRc.bottom - parentRc.top  - LIC_H) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        LICENSE_CLASS_NAME, nullptr, WS_POPUP | WS_CLIPCHILDREN,
        x, y, LIC_W, LIC_H,
        parent, nullptr, g_hInstance, nullptr
    );

    if (!hwnd) return;

    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Edit 컨트롤 (라이센스 텍스트)
    int editX = LIC_MARGIN;
    int editY = LIC_HEADER + 6;
    int editW = LIC_W - LIC_MARGIN * 2;
    int editH = LIC_H - LIC_HEADER - LIC_FOOTER - 12;

    g_lic.hEdit = CreateWindowExW(
        0, L"EDIT", licenseText.c_str(),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        editX, editY, editW, editH,
        hwnd, nullptr, g_hInstance, nullptr
    );

    g_lic.hEditFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
        FIXED_PITCH, L"Consolas");
    SendMessageW(g_lic.hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_lic.hEditFont), TRUE);

    RECT editRc;
    GetClientRect(g_lic.hEdit, &editRc);
    editRc.left += 10; editRc.top += 8; editRc.right -= 10; editRc.bottom -= 8;
    SendMessageW(g_lic.hEdit, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&editRc));

    SendMessageW(g_lic.hEdit, EM_SETSEL, 0, 0);
    SendMessageW(g_lic.hEdit, EM_SCROLLCARET, 0, 0);

    g_lic.fadingIn    = true;
    g_lic.fadeElapsed = 0;
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    SetTimer(hwnd, LIC_FADEIN_TIMER, LIC_FADE_INTERVAL, nullptr);

    // 모달 메시지 루프 (EnableWindow 없이 입력 필터링)
    MSG msg2;
    while (!g_lic.closed && GetMessage(&msg2, nullptr, 0, 0))
    {
        if (msg2.hwnd == parent || IsChild(parent, msg2.hwnd))
        {
            if ((msg2.message >= WM_MOUSEFIRST && msg2.message <= WM_MOUSELAST) ||
                (msg2.message >= WM_KEYFIRST && msg2.message <= WM_KEYLAST) ||
                msg2.message == WM_NCLBUTTONDOWN || msg2.message == WM_NCRBUTTONDOWN)
            {
                if (msg2.message == WM_LBUTTONDOWN || msg2.message == WM_NCLBUTTONDOWN)
                    FlashWindow(hwnd, TRUE);
                continue;
            }
        }
        TranslateMessage(&msg2);
        DispatchMessage(&msg2);
    }
}

void CleanPopup::Shutdown()
{
    if (!g_initialized) return;
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_initialized = false;
}

// ---------------------------------------------------------------
// NoteInput 팝업 구현 (메모 텍스트 입력)
// ---------------------------------------------------------------
namespace
{
    constexpr int NI_W      = 400;
    constexpr int NI_H      = 270;
    constexpr int NI_PAD    = 18;
    constexpr int NI_BTN_W  = 100;
    constexpr int NI_BTN_H  = 34;

    const wchar_t* NI_CLASS_NAME = L"CleanNoteInputClass";
    bool g_niClassRegistered = false;

    struct NoteInputState {
        std::wstring  title;
        std::wstring  result;
        bool          confirmed = false;
        bool          closed    = false;
        // 버튼
        RECT          okRc{};
        RECT          canRc{};
        bool          okHov  = false;
        bool          canHov = false;
        // 자식 EDIT 컨트롤
        HWND          hEdit  = nullptr;
        // 에디트 영역 (GDI+ 테두리용)
        RECT          editRc{};
    } g_ni;

    bool         g_niPwd = false;
    int          g_niH   = NI_H;
    std::wstring g_niPrompt;

    // ── 렌더 ─────────────────────────────────────────────────────
    void DrawNoteInput(HWND hwnd, HDC hdc)
    {
        using namespace Gdiplus;
        RECT cr; GetClientRect(hwnd, &cr);
        int W = cr.right, H = cr.bottom;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // 배경
        SolidBrush bgBr(Color(255,255,255,255));
        g.FillRectangle(&bgBr, 0.f, 0.f, (float)W, (float)H);

        // 외곽선
        Pen border(Color(255,218,218,218), 1.f);
        g.DrawRectangle(&border, 0.5f, 0.5f, (float)W-1.f, (float)H-1.f);

        FontFamily ff(L"Segoe UI");

        // 제목
        Font titleFont(&ff, 10, FontStyleBold, UnitPoint);
        SolidBrush darkBr(Color(255,20,20,20));
        g.DrawString(g_ni.title.c_str(), -1, &titleFont,
            RectF((float)NI_PAD, 18.f, (float)(W - NI_PAD*2), 26.f),
            nullptr, &darkBr);

        // 구분선
        Pen sepPen(Color(255,230,230,230), 1.f);
        g.DrawLine(&sepPen, (float)NI_PAD, 50.f, (float)(W - NI_PAD), 50.f);

        if (g_niPwd) {
            Font msgFont(&ff, 9, FontStyleRegular, UnitPoint);
            SolidBrush msgBr(Color(255,80,80,80));
            g.DrawString(g_niPrompt.c_str(), -1, &msgFont,
                RectF((float)NI_PAD, 58.f, (float)(W - NI_PAD*2), 22.f),
                nullptr, &msgBr);
        }

        // 에디트 테두리 (rounded, Fluent)
        const RECT& er = g_ni.editRc;
        RectF erF((float)er.left - 1, (float)er.top - 1,
                  (float)(er.right - er.left) + 2,
                  (float)(er.bottom - er.top) + 2);
        auto* rp = MakeRoundRectPath(erF, 4.f);
        SolidBrush editBg(Color(255,250,250,250));
        g.FillPath(&editBg, rp);
        Pen editBorder(Color(255,0,120,212), 1.5f);
        g.DrawPath(&editBorder, rp);
        delete rp;

        // ── 버튼 ──
        auto DrawBtn = [&](const RECT& rc, bool hov, bool primary, const wchar_t* lbl) {
            RectF rf((float)rc.left+.5f, (float)rc.top+.5f,
                     (float)(rc.right-rc.left)-1.f,
                     (float)(rc.bottom-rc.top)-1.f);
            auto* bp = MakeRoundRectPath(rf, 4.f);
            if (primary) {
                Color top = hov ? Color(255,14,148,244) : Color(255,0,120,212);
                Color bot = hov ? Color(255,0,100,185)  : Color(255,0,100,190);
                LinearGradientBrush grad(PointF(rf.X,rf.Y),PointF(rf.X,rf.Y+rf.Height),top,bot);
                g.FillPath(&grad, bp);
            } else {
                Color top = hov ? Color(255,245,245,245) : Color(255,255,255,255);
                Color bot = hov ? Color(255,230,230,230) : Color(255,242,242,242);
                LinearGradientBrush grad(PointF(rf.X,rf.Y),PointF(rf.X,rf.Y+rf.Height),top,bot);
                g.FillPath(&grad, bp);
                Pen bdr(Color(255,200,200,200), 1.f);
                g.DrawPath(&bdr, bp);
            }
            delete bp;
            SolidBrush txtBr(primary ? Color(255,255,255,255) : Color(255,45,45,45));
            Font btnFont(&ff, 9, FontStyleBold, UnitPoint);
            StringFormat sf; sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(lbl, -1, &btnFont, rf, &sf, &txtBr);
        };

        DrawBtn(g_ni.canRc, g_ni.canHov, false, LS(STR_CANCEL));
        DrawBtn(g_ni.okRc,  g_ni.okHov,  true,  LS(STR_ABOUT_OK));
    }

    // ── 서브클래스: EDIT에서 Escape/Ctrl+Enter 가로채기 ──────────
    WNDPROC g_niEditOrigProc = nullptr;

    LRESULT CALLBACK NoteEditSubclassProc(HWND hEdit, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_KEYDOWN)
        {
            if (wParam == VK_ESCAPE) {
                SendMessage(GetParent(hEdit), WM_COMMAND, IDCANCEL, 0);
                return 0;
            }
            if (wParam == VK_RETURN && (g_niPwd || (GetKeyState(VK_CONTROL) & 0x8000))) {
                SendMessage(GetParent(hEdit), WM_COMMAND, IDOK, 0);
                return 0;
            }
        }
        return CallWindowProcW(g_niEditOrigProc, hEdit, msg, wParam, lParam);
    }

    // ── WndProc ──────────────────────────────────────────────────
    LRESULT CALLBACK NoteInputProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            // EDIT 컨트롤 생성
            int ex = NI_PAD, ey = g_niPwd ? 86 : 60;
            int ew = NI_W - NI_PAD*2;
            int eh = g_niPwd ? 34 : (g_niH - 60 - NI_BTN_H - NI_PAD*2 - 10);
            g_ni.editRc = { ex, ey, ex+ew, ey+eh };
            g_ni.hEdit = CreateWindowExW(
                0, L"EDIT", g_ni.result.c_str(),
                g_niPwd ? (WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL)
                        : (WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                           ES_WANTRETURN | WS_VSCROLL),
                ex+4, ey+4, ew-8, eh-8,
                hWnd, (HMENU)(INT_PTR)100,
                g_hInstance, nullptr);
            // 폰트 설정
            HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            if (hFont) SendMessage(g_ni.hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            if (g_niPwd && hFont) {
                HDC hdcM = GetDC(hWnd);
                HGDIOBJ oldF = SelectObject(hdcM, hFont);
                TEXTMETRICW tm{};
                GetTextMetricsW(hdcM, &tm);
                SelectObject(hdcM, oldF);
                ReleaseDC(hWnd, hdcM);
                int th = tm.tmHeight;
                int ch = th + 2;
                SetWindowPos(g_ni.hEdit, nullptr, ex + 6, ey + (eh - ch) / 2, ew - 12, ch,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            // 초기 텍스트 커서 끝으로
            SendMessage(g_ni.hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
            // 서브클래스로 Esc/Ctrl+Enter 처리
            g_niEditOrigProc = (WNDPROC)SetWindowLongPtrW(
                g_ni.hEdit, GWLP_WNDPROC, (LONG_PTR)NoteEditSubclassProc);
            SetFocus(g_ni.hEdit);
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            HDC mdc = CreateCompatibleDC(hdc);
            HBITMAP mbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldbm = (HBITMAP)SelectObject(mdc, mbm);
            DrawNoteInput(hWnd, mdc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, oldbm);
            DeleteObject(mbm); DeleteDC(mdc);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_COMMAND:
        {
            if (LOWORD(wParam) == IDOK) {
                int len = GetWindowTextLengthW(g_ni.hEdit);
                if (len > 0) {
                    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
                    GetWindowTextW(g_ni.hEdit, buf.data(), len + 1);
                    g_ni.result.assign(buf.data(), static_cast<size_t>(len));
                } else {
                    g_ni.result.clear();
                }
                g_ni.confirmed = true;
                g_ni.closed    = true;
                // 페이드아웃
                HWND par = GetParent(hWnd);
                for (int a = 255; a >= 0; a -= 40) {
                    SetLayeredWindowAttributes(hWnd, 0, (BYTE)a, LWA_ALPHA);
                    UpdateWindow(hWnd); Sleep(8);
                }
                ShowWindow(hWnd, SW_HIDE);
                DestroyWindow(hWnd);
                EnableWindow(par, TRUE); SetFocus(par);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                g_ni.confirmed = false;
                g_ni.closed    = true;
                HWND par = GetParent(hWnd);
                for (int a = 255; a >= 0; a -= 40) {
                    SetLayeredWindowAttributes(hWnd, 0, (BYTE)a, LWA_ALPHA);
                    UpdateWindow(hWnd); Sleep(8);
                }
                ShowWindow(hWnd, SW_HIDE);
                DestroyWindow(hWnd);
                EnableWindow(par, TRUE); SetFocus(par);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool changed = false;
            auto upd = [&](RECT& rc, bool& hov) {
                bool ov = PtInRect(&rc, pt);
                if (ov != hov) { hov = ov; changed = true; }
            };
            upd(g_ni.okRc,  g_ni.okHov);
            upd(g_ni.canRc, g_ni.canHov);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            // 버튼 영역만 갱신 (전체 갱신 시 EDIT 영역까지 다시 그려져 깜빡임)
            if (changed) {
                InvalidateRect(hWnd, &g_ni.okRc,  FALSE);
                InvalidateRect(hWnd, &g_ni.canRc, FALSE);
            }
            SetCursor((g_ni.okHov || g_ni.canHov) ? g_curHand : g_curArrow);
            break;
        }
        case WM_MOUSELEAVE:
            // EDIT 위로 이동할 때도 발생하므로 호버 상태가 있을 때만 버튼 갱신
            if (g_ni.okHov || g_ni.canHov) {
                g_ni.okHov = g_ni.canHov = false;
                InvalidateRect(hWnd, &g_ni.okRc,  FALSE);
                InvalidateRect(hWnd, &g_ni.canRc, FALSE);
            }
            break;
        case WM_LBUTTONUP:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PtInRect(&g_ni.okRc, pt))  SendMessage(hWnd, WM_COMMAND, IDOK,     0);
            if (PtInRect(&g_ni.canRc, pt)) SendMessage(hWnd, WM_COMMAND, IDCANCEL, 0);
            break;
        }
        case WM_NCHITTEST:
        {
            LRESULT hit = DefWindowProc(hWnd, message, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hWnd, &pt);
                if (!PtInRect(&g_ni.okRc, pt) && !PtInRect(&g_ni.canRc, pt) &&
                    !PtInRect(&g_ni.editRc, pt))
                    return HTCAPTION;
            }
            return hit;
        }
        case WM_CTLCOLOREDIT:
        {
            HDC hdcEdit = (HDC)wParam;
            SetBkColor(hdcEdit, RGB(250,250,250));
            SetTextColor(hdcEdit, RGB(20,20,20));
            static HBRUSH hbr = CreateSolidBrush(RGB(250,250,250));
            return (LRESULT)hbr;
        }
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        return 0;
    }
} // anonymous namespace (NoteInput)

static bool RunNoteInput(HWND parent, const std::wstring& title, std::wstring& text)
{
    if (!g_initialized) return false;

    // 클래스 등록 (최초 1회)
    if (!g_niClassRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize       = sizeof(wc);
        wc.style        = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc  = NoteInputProc;
        wc.hInstance    = g_hInstance;
        wc.hCursor      = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground= nullptr;
        wc.lpszClassName= NI_CLASS_NAME;
        if (!RegisterClassExW(&wc)) return false;
        g_niClassRegistered = true;
    }

    // 상태 초기화
    g_ni.title     = title;
    g_ni.result    = text;
    g_ni.confirmed = false;
    g_ni.closed    = false;
    g_ni.okHov = g_ni.canHov = false;

    // 버튼 위치
    int btnY  = g_niH - NI_BTN_H - NI_PAD;
    int gap   = 10;
    int totalW= NI_BTN_W*2 + gap;
    int startX= (NI_W - totalW) / 2;
    g_ni.canRc = { startX,            btnY, startX + NI_BTN_W,       btnY + NI_BTN_H };
    g_ni.okRc  = { startX+NI_BTN_W+gap, btnY, startX+NI_BTN_W*2+gap, btnY + NI_BTN_H };

    // 부모 중앙
    RECT pr; GetWindowRect(parent, &pr);
    int x = pr.left + (pr.right  - pr.left - NI_W) / 2;
    int y = pr.top  + (pr.bottom - pr.top  - g_niH) / 2;

    EnableWindow(parent, FALSE);
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        NI_CLASS_NAME, nullptr, WS_POPUP | WS_CLIPCHILDREN,
        x, y, NI_W, g_niH,
        parent, nullptr, g_hInstance, nullptr);

    if (!hwnd) { EnableWindow(parent, TRUE); return false; }

    // DWM 둥근 모서리 + 그림자
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    MARGINS margins = { 1,1,1,1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // 페이드인
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    AnimateWindow(hwnd, 180, AW_BLEND);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    // 모달 메시지 루프
    MSG msg2;
    while (!g_ni.closed && GetMessage(&msg2, nullptr, 0, 0))
    {
        if (msg2.hwnd == parent || IsChild(parent, msg2.hwnd)) {
            if ((msg2.message >= WM_MOUSEFIRST && msg2.message <= WM_MOUSELAST) ||
                (msg2.message >= WM_KEYFIRST   && msg2.message <= WM_KEYLAST)   ||
                msg2.message == WM_NCLBUTTONDOWN || msg2.message == WM_NCRBUTTONDOWN)
            {
                if (msg2.message == WM_LBUTTONDOWN || msg2.message == WM_NCLBUTTONDOWN)
                    FlashWindow(hwnd, TRUE);
                continue;
            }
        }
        TranslateMessage(&msg2);
        DispatchMessage(&msg2);
    }

    if (g_ni.confirmed) text = g_ni.result;
    return g_ni.confirmed;
}

bool CleanPopup::ShowNoteInput(HWND parent, const std::wstring& title, std::wstring& text)
{
    g_niPwd = false;
    g_niH   = NI_H;
    return RunNoteInput(parent, title, text);
}

bool CleanPopup::ShowPasswordInput(HWND parent, const std::wstring& title,
                                   const std::wstring& prompt, std::wstring& password)
{
    g_niPwd    = true;
    g_niH      = 190;
    g_niPrompt = prompt;
    password.clear();
    bool ok = RunNoteInput(parent, title, password);
    SecureZeroMemory(g_ni.result.data(), g_ni.result.size() * sizeof(wchar_t));
    g_ni.result.clear();
    g_niPwd = false;
    g_niH   = NI_H;
    return ok;
}
