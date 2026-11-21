// ToastDlg.cpp : ?좎뒪???뚮┝ (?먮룞 ?щ씪吏?
//

#include "framework.h"
#include "ToastDlg.h"
#include <dwmapi.h>
#include <string>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

static constexpr int TOAST_PADDING_X = 20;
static constexpr int TOAST_PADDING_Y = 10;
static constexpr int TOAST_BOTTOM_MARGIN = 48;
static constexpr int TOAST_CORNER_RADIUS = 18;
static constexpr UINT_PTR TOAST_TIMER_ID = 9001;
static constexpr UINT_PTR TOAST_FADE_TIMER_ID = 9002;
static constexpr UINT_PTR TOAST_FADEIN_TIMER_ID = 9003;
static constexpr int FADE_INTERVAL_MS = 16;     // ~60fps
static constexpr int FADE_IN_DURATION_MS  = 220;
static constexpr int FADE_OUT_DURATION_MS = 380;

// 색: 툴팁과 같게 평소에는 밝게, 야간 모드(g_darkMode)에서는 어둡게
extern bool g_darkMode;
static COLORREF ToastBg()     { return g_darkMode ? RGB(36, 36, 44)    : RGB(249, 249, 249); }
static COLORREF ToastFg()     { return g_darkMode ? RGB(230, 230, 240) : RGB(28, 28, 30); }
static COLORREF ToastFgSub()  { return g_darkMode ? RGB(190, 192, 206) : RGB(96, 96, 104); }
static COLORREF ToastBorder() { return g_darkMode ? RGB(60, 64, 80)    : RGB(214, 214, 218); }

static constexpr int TOAST_LINE_GAP = 4;                           // 첫째·둘째 줄 사이

struct ToastData
{
    HFONT   hFont;
    HFONT   hFontSub;      // 둘째 줄 글꼴 (보통 굵기, 조금 작게)
    BYTE    alpha;
    bool    fadingIn;
    bool    fadingOut;
    int     fadeElapsed;
    int     durationMs;
    wchar_t message[512];  // 첫째 줄
    wchar_t sub[512];      // 둘째 줄 (메시지에 '\n'이 있으면 그 뒤). 없으면 빈 문자열
    int     line1H;        // 첫째 줄 높이
    bool    ownBorder;     // DWM 둥근 모서리를 못 쓰는 환경(Windows 10)에서는 직접 둥글게 자르고 테두리를 그림
};

static const wchar_t* TOAST_CLASS = L"PlainPDFViewerToast";
static bool g_toastClassRegistered = false;

static void RegisterToastClass(HINSTANCE hInst);
static LRESULT CALLBACK ToastWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ??? 怨듦컻 API ?????????????????????????????????????????????????????????????????

void ShowToast(HWND hParent, const wchar_t* message, int durationMs,
               ToastPosition position)
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    RegisterToastClass(hInst);

    // 湲곗〈 ?좎뒪?멸? ?덉쑝硫??リ린
    HWND existing = FindWindowW(TOAST_CLASS, nullptr);
    if (existing) DestroyWindow(existing);

    // ?띿뒪???ш린 痢≪젙
    HDC hdc = GetDC(hParent);
    HFONT hFont = CreateFontW(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT hFontSub = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFont));

    // '\n'이 있으면 두 줄: 첫째 줄은 굵게, 둘째 줄은 보충 설명(보통 굵기·조금 작게·옅은 색)
    std::wstring line1 = message ? message : L"", line2;
    const size_t nl = line1.find(L'\n');
    if (nl != std::wstring::npos) { line2 = line1.substr(nl + 1); line1.resize(nl); }

    RECT textRc = {};
    DrawTextW(hdc, line1.c_str(), -1, &textRc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    const int line1H = textRc.bottom - textRc.top;
    int textW = textRc.right - textRc.left, textH = line1H;
    if (!line2.empty())
    {
        SelectObject(hdc, hFontSub);
        RECT subRc = {};
        DrawTextW(hdc, line2.c_str(), -1, &subRc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        textW = std::max(textW, (int)(subRc.right - subRc.left));
        textH += TOAST_LINE_GAP + (subRc.bottom - subRc.top);
    }
    SelectObject(hdc, oldFont);
    ReleaseDC(hParent, hdc);

    int w = textW + TOAST_PADDING_X * 2;
    int h = textH + TOAST_PADDING_Y * 2 + (line2.empty() ? 0 : 4);   // 두 줄이면 위아래 여백을 조금 더

    // ?꾩튂 怨꾩궛
    RECT parentRc;
    GetWindowRect(hParent, &parentRc);
    int x = parentRc.left + (parentRc.right - parentRc.left - w) / 2;
    int y;
    if (position == ToastPosition::Bottom)
        y = parentRc.bottom - TOAST_BOTTOM_MARGIN - h;
    else
        y = parentRc.top + (parentRc.bottom - parentRc.top - h) / 2;

    HWND hToast = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        TOAST_CLASS, nullptr,
        WS_POPUP,
        x, y, w, h,
        hParent, nullptr, hInst, nullptr);

    if (!hToast) { DeleteObject(hFont); DeleteObject(hFontSub); return; }

    auto* td = new ToastData{};
    td->hFont = hFont;
    td->hFontSub = hFontSub;
    td->line1H = line1H;
    wcsncpy_s(td->sub, line2.c_str(), _TRUNCATE);
    td->alpha = 0;
    td->fadingIn = true;
    td->fadingOut = false;
    td->fadeElapsed = 0;
    td->durationMs = durationMs;
    wcsncpy_s(td->message, line1.c_str(), _TRUNCATE);
    SetWindowLongPtrW(hToast, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(td));

    // ?κ렐 紐⑥꽌由?由ъ쟾
    // Windows 11: 시스템 둥근 모서리 + 그림자 + 테두리 (밝은 토스트가 흰 페이지 위에서도 떠 보이도록)
    // 그 외: 예전처럼 창을 둥글게 잘라 내고 테두리를 직접 그림
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    if (SUCCEEDED(DwmSetWindowAttribute(hToast, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner))))
    {
        MARGINS margins = { 1, 1, 1, 1 };
        DwmExtendFrameIntoClientArea(hToast, &margins);
    }
    else
    {
        td->ownBorder = true;
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, TOAST_CORNER_RADIUS, TOAST_CORNER_RADIUS);
        SetWindowRgn(hToast, rgn, TRUE);
    }

    SetLayeredWindowAttributes(hToast, 0, 0, LWA_ALPHA);
    ShowWindow(hToast, SW_SHOWNOACTIVATE);

    // ?섏씠?????쒖옉
    SetTimer(hToast, TOAST_FADEIN_TIMER_ID, FADE_INTERVAL_MS, nullptr);
}

// ??? ?대? 援ы쁽 ???????????????????????????????????????????????????????????????

static void RegisterToastClass(HINSTANCE hInst)
{
    if (g_toastClassRegistered) return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToastWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = TOAST_CLASS;
    RegisterClassExW(&wc);
    g_toastClassRegistered = true;
}

static LRESULT CALLBACK ToastWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* td = reinterpret_cast<ToastData*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_PAINT:
    {
        if (!td) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        // 諛곌꼍
        HBRUSH bgBr = CreateSolidBrush(ToastBg());
        FillRect(hdc, &rc, bgBr);
        DeleteObject(bgBr);

        // ?뚮몢由?
        if (td->ownBorder)
        {
            HPEN pen = CreatePen(PS_SOLID, 1, ToastBorder());
            HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
            HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
            RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom,
                TOAST_CORNER_RADIUS, TOAST_CORNER_RADIUS);
            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }

        // ?띿뒪??
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ToastFg());
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, td->hFont));
        if (!td->sub[0])
        {
            DrawTextW(hdc, td->message, -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        else
        {
            // 두 줄: 위아래 여백을 뺀 영역에 첫째 줄(굵게)과 둘째 줄(옅게)을 차례로
            const int top = TOAST_PADDING_Y + 2;
            RECT r1 = { rc.left, top, rc.right, top + td->line1H };
            DrawTextW(hdc, td->message, -1, &r1, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(hdc, td->hFontSub);
            SetTextColor(hdc, ToastFgSub());
            RECT r2 = { rc.left, r1.bottom + TOAST_LINE_GAP, rc.right, rc.bottom - TOAST_PADDING_Y };
            DrawTextW(hdc, td->sub, -1, &r2, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(hdc, oldFont);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (!td) break;
        if (wParam == TOAST_FADEIN_TIMER_ID)
        {
            td->fadeElapsed += FADE_INTERVAL_MS;
            if (td->fadeElapsed >= FADE_IN_DURATION_MS)
            {
                // ?섏씠?????꾨즺
                KillTimer(hWnd, TOAST_FADEIN_TIMER_ID);
                td->fadingIn = false;
                td->alpha = 255;
                td->fadeElapsed = 0;
                SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
                // ?쒖떆 ?쒓컙 ??대㉧ ?쒖옉
                SetTimer(hWnd, TOAST_TIMER_ID, td->durationMs, nullptr);
                return 0;
            }
            float t = static_cast<float>(td->fadeElapsed) / FADE_IN_DURATION_MS;
            t = t * t * (3.0f - 2.0f * t); // smoothstep
            td->alpha = static_cast<BYTE>(255 * t);
            SetLayeredWindowAttributes(hWnd, 0, td->alpha, LWA_ALPHA);
            return 0;
        }
        if (wParam == TOAST_TIMER_ID)
        {
            // ?쒖떆 ?쒓컙 ?꾨즺 ???섏씠???꾩썐 ?쒖옉
            KillTimer(hWnd, TOAST_TIMER_ID);
            td->fadingOut = true;
            td->fadeElapsed = 0;
            SetTimer(hWnd, TOAST_FADE_TIMER_ID, FADE_INTERVAL_MS, nullptr);
            return 0;
        }
        if (wParam == TOAST_FADE_TIMER_ID)
        {
            td->fadeElapsed += FADE_INTERVAL_MS;
            if (td->fadeElapsed >= FADE_OUT_DURATION_MS)
            {
                KillTimer(hWnd, TOAST_FADE_TIMER_ID);
                DestroyWindow(hWnd);
                return 0;
            }
            float t = static_cast<float>(td->fadeElapsed) / FADE_OUT_DURATION_MS;
            t = t * t * (3.0f - 2.0f * t); // smoothstep
            td->alpha = static_cast<BYTE>(255 * (1.0f - t));
            SetLayeredWindowAttributes(hWnd, 0, td->alpha, LWA_ALPHA);
            return 0;
        }
        break;

    // ?대┃?섎㈃ 利됱떆 ?リ린
    case WM_LBUTTONDOWN:
        DestroyWindow(hWnd);
        return 0;

    case WM_NCDESTROY:
        if (td)
        {
            KillTimer(hWnd, TOAST_FADEIN_TIMER_ID);
            KillTimer(hWnd, TOAST_TIMER_ID);
            KillTimer(hWnd, TOAST_FADE_TIMER_ID);
            DeleteObject(td->hFont);
            if (td->hFontSub) DeleteObject(td->hFontSub);
            delete td;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        break;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
