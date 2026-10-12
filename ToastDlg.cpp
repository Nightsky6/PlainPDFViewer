// ToastDlg.cpp : ?좎뒪???뚮┝ (?먮룞 ?щ씪吏?
//

#include "framework.h"
#include "ToastDlg.h"
#include <dwmapi.h>

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

static constexpr COLORREF CLR_TOAST_BG = RGB(36, 36, 44);
static constexpr COLORREF CLR_TOAST_FG = RGB(230, 230, 240);
static constexpr COLORREF CLR_TOAST_BORDER = RGB(60, 64, 80);

struct ToastData
{
    HFONT   hFont;
    BYTE    alpha;
    bool    fadingIn;
    bool    fadingOut;
    int     fadeElapsed;
    int     durationMs;
    wchar_t message[256];
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
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFont));

    RECT textRc = {};
    DrawTextW(hdc, message, -1, &textRc, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    ReleaseDC(hParent, hdc);

    int w = (textRc.right - textRc.left) + TOAST_PADDING_X * 2;
    int h = (textRc.bottom - textRc.top) + TOAST_PADDING_Y * 2;

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

    if (!hToast) { DeleteObject(hFont); return; }

    auto* td = new ToastData{};
    td->hFont = hFont;
    td->alpha = 0;
    td->fadingIn = true;
    td->fadingOut = false;
    td->fadeElapsed = 0;
    td->durationMs = durationMs;
    wcsncpy_s(td->message, message, _TRUNCATE);
    SetWindowLongPtrW(hToast, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(td));

    // ?κ렐 紐⑥꽌由?由ъ쟾
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, TOAST_CORNER_RADIUS, TOAST_CORNER_RADIUS);
    SetWindowRgn(hToast, rgn, TRUE);

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
        HBRUSH bgBr = CreateSolidBrush(CLR_TOAST_BG);
        FillRect(hdc, &rc, bgBr);
        DeleteObject(bgBr);

        // ?뚮몢由?
        HPEN pen = CreatePen(PS_SOLID, 1, CLR_TOAST_BORDER);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
        HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom,
            TOAST_CORNER_RADIUS, TOAST_CORNER_RADIUS);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        // ?띿뒪??
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, CLR_TOAST_FG);
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, td->hFont));
        DrawTextW(hdc, td->message, -1, &rc,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
            delete td;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        break;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
