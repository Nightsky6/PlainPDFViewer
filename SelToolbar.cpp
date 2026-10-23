// ---------------------------------------------------------------
// SelToolbar.cpp - 텍스트 선택 후 표시되는 주석 도구 모음 구현
// ---------------------------------------------------------------
#include "framework.h"
#include <objbase.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <string>
#include <vector>
#include "SelToolbar.h"
#include "Lang.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

#undef min
#undef max

namespace
{
    // 레이아웃
    constexpr int PAD    = 6;    // 테두리 안쪽 여백
    constexpr int BTN    = 30;   // 동작 버튼 크기
    constexpr int SWATCH = 20;   // 색상 칸 너비
    constexpr int SEP    = 9;    // 구분선 칸 너비
    constexpr int TB_H   = PAD * 2 + BTN;

    // 등장 애니메이션: 페이드인 + 아래에서 살짝 떠오름
    constexpr UINT_PTR ANIM_TIMER = 1;
    constexpr UINT     ANIM_TICK  = 10;    // ms
    constexpr double   ANIM_MS    = 150.0;
    constexpr int      SLIDE_PX   = 6;

    // 색상
    constexpr COLORREF C_BG      = RGB(255, 255, 255);
    constexpr COLORREF C_BORDER  = RGB(218, 220, 226);
    constexpr COLORREF C_HOT     = RGB(235, 238, 243);
    constexpr COLORREF C_PRESSED = RGB(220, 224, 232);
    constexpr COLORREF C_ICON    = RGB( 45,  48,  55);
    constexpr COLORREF C_ACCENT  = RGB(  0, 120, 212);

    const wchar_t* TB_CLASS = L"PlainSelToolbarClass";

    enum class ItemKind { Action, Swatch };

    struct Item
    {
        ItemKind kind;
        SelToolbar::Action action;   // kind == Action
        int  swatch;                 // kind == Swatch
        RECT rc;
    };

    struct State
    {
        HWND hWnd    = nullptr;
        HWND owner   = nullptr;
        HWND hTip    = nullptr;
        std::vector<COLORREF> palette;
        std::vector<Item>     items;
        int  colorIdx = 0;           // 마지막으로 고른 색 (다음 표시 때도 유지)
        int  hot      = -1;
        int  pressed  = -1;
        bool tracking = false;
        int  x = 0, y = 0;           // 최종 표시 위치 (화면 좌표)
        ULONGLONG animStart = 0;
        SelToolbar::ActionFn onAction = nullptr;
        std::vector<std::wstring> tipTexts;
    } g;

    Gdiplus::Color GdipColor(COLORREF c, BYTE a = 255)
    {
        return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
    }

    Gdiplus::GraphicsPath* RoundRectPath(const Gdiplus::RectF& r, float rad)
    {
        auto* p = new Gdiplus::GraphicsPath();
        float d = rad * 2.0f;
        p->AddArc(r.X,               r.Y,                d, d, 180, 90);
        p->AddArc(r.X + r.Width - d, r.Y,                d, d, 270, 90);
        p->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d,   0, 90);
        p->AddArc(r.X,               r.Y + r.Height - d, d, d,  90, 90);
        p->CloseFigure();
        return p;
    }

    // 메뉴 문자열 "하이라이트(&H)\tCtrl+H" → "하이라이트 (Ctrl+H)"
    std::wstring TipFromMenuLabel(const wchar_t* label)
    {
        std::wstring name(label), key;
        const size_t tab = name.find(L'\t');
        if (tab != std::wstring::npos) { key = name.substr(tab + 1); name.resize(tab); }
        const size_t mn = name.find(L"(&");
        if (mn != std::wstring::npos)
        {
            const size_t close = name.find(L')', mn);
            if (close != std::wstring::npos) name.erase(mn, close - mn + 1);
        }
        name.erase(std::remove(name.begin(), name.end(), L'&'), name.end());
        while (!name.empty() && name.back() == L' ') name.pop_back();
        return key.empty() ? name : name + L" (" + key + L")";
    }

    const wchar_t* ActionLabel(SelToolbar::Action a)
    {
        switch (a)
        {
        case SelToolbar::Action::Highlight: return LS(STR_CTX_HIGHLIGHT);
        case SelToolbar::Action::Underline: return LS(STR_CTX_UNDERLINE);
        case SelToolbar::Action::Strikeout: return LS(STR_CTX_STRIKEOUT);
        case SelToolbar::Action::Squiggly:  return LS(STR_CTX_SQUIGGLY);
        default:                            return LS(STR_CTX_COPY);
        }
    }

    // 항목 배치: [하이라이트][밑줄][취소선][물결] | 색상… | [복사]
    int BuildItems()
    {
        g.items.clear();
        int x = PAD;
        auto addAction = [&](SelToolbar::Action a) {
            g.items.push_back({ ItemKind::Action, a, -1, { x, PAD, x + BTN, PAD + BTN } });
            x += BTN;
        };
        addAction(SelToolbar::Action::Highlight);
        addAction(SelToolbar::Action::Underline);
        addAction(SelToolbar::Action::Strikeout);
        addAction(SelToolbar::Action::Squiggly);
        x += SEP;
        for (int i = 0; i < (int)g.palette.size(); ++i)
        {
            g.items.push_back({ ItemKind::Swatch, SelToolbar::Action::Copy, i, { x, PAD, x + SWATCH, PAD + BTN } });
            x += SWATCH;
        }
        x += SEP;
        addAction(SelToolbar::Action::Copy);
        return x + PAD;
    }

    int HitTest(POINT pt)
    {
        for (int i = 0; i < (int)g.items.size(); ++i)
            if (PtInRect(&g.items[i].rc, pt)) return i;
        return -1;
    }

    void UpdateTooltips()
    {
        if (!g.hTip) return;
        // 기존 도구 제거 후 다시 등록 (언어가 바뀌었을 수 있음)
        while (SendMessageW(g.hTip, TTM_GETTOOLCOUNT, 0, 0) > 0)
        {
            TOOLINFOW ti = { sizeof(ti) };
            if (!SendMessageW(g.hTip, TTM_ENUMTOOLSW, 0, (LPARAM)&ti)) break;
            SendMessageW(g.hTip, TTM_DELTOOLW, 0, (LPARAM)&ti);
        }
        g.tipTexts.assign(g.items.size(), std::wstring());
        for (int i = 0; i < (int)g.items.size(); ++i)
        {
            if (g.items[i].kind != ItemKind::Action) continue;
            g.tipTexts[i] = TipFromMenuLabel(ActionLabel(g.items[i].action));
            TOOLINFOW ti = { sizeof(ti) };
            ti.uFlags   = TTF_SUBCLASS;
            ti.hwnd     = g.hWnd;
            ti.uId      = (UINT_PTR)(i + 1);
            ti.rect     = g.items[i].rc;
            ti.lpszText = const_cast<LPWSTR>(g.tipTexts[i].c_str());
            SendMessageW(g.hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }

    // 동작 아이콘: "ab" 글자에 각 주석 모양을 현재 색으로 덧그린다
    void DrawActionIcon(Gdiplus::Graphics& gr, const RECT& rc, SelToolbar::Action a, COLORREF color)
    {
        using namespace Gdiplus;
        const float cx = (rc.left + rc.right) * 0.5f;
        const float cy = (rc.top + rc.bottom) * 0.5f;

        if (a == SelToolbar::Action::Copy)
        {
            Pen pen(GdipColor(C_ICON), 1.4f);
            RectF back(cx - 6.0f, cy - 7.0f, 9.0f, 11.0f);
            RectF front(cx - 3.0f, cy - 4.0f, 9.0f, 11.0f);
            GraphicsPath* pb = RoundRectPath(back, 1.5f);
            gr.DrawPath(&pen, pb);
            delete pb;
            SolidBrush bg(GdipColor(C_BG));
            GraphicsPath* pf = RoundRectPath(front, 1.5f);
            gr.FillPath(&bg, pf);
            gr.DrawPath(&pen, pf);
            delete pf;
            return;
        }

        if (a == SelToolbar::Action::Highlight)
        {
            SolidBrush hl(GdipColor(color, 210));
            GraphicsPath* p = RoundRectPath(RectF(cx - 10.0f, cy - 8.0f, 20.0f, 16.0f), 2.5f);
            gr.FillPath(&hl, p);
            delete p;
        }

        FontFamily ff(L"Segoe UI");
        Font font(ff.IsAvailable() ? &ff : FontFamily::GenericSansSerif(), 11.0f, FontStyleRegular, UnitPixel);
        SolidBrush txt(GdipColor(C_ICON));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        gr.DrawString(L"ab", -1, &font, RectF(cx - 12.0f, cy - 9.0f, 24.0f, 16.0f), &sf, &txt);

        Pen line(GdipColor(color), 1.8f);
        switch (a)
        {
        case SelToolbar::Action::Underline:
            gr.DrawLine(&line, cx - 8.0f, cy + 7.0f, cx + 8.0f, cy + 7.0f);
            break;
        case SelToolbar::Action::Strikeout:
            gr.DrawLine(&line, cx - 8.0f, cy, cx + 8.0f, cy);
            break;
        case SelToolbar::Action::Squiggly:
        {
            PointF pts[9];
            for (int i = 0; i < 9; ++i)
                pts[i] = PointF(cx - 8.0f + i * 2.0f, cy + 7.0f + ((i % 2) ? -1.5f : 1.5f));
            gr.DrawLines(&line, pts, 9);
            break;
        }
        default:
            break;
        }
    }

    void Paint(HDC hdc)
    {
        using namespace Gdiplus;
        RECT cr; GetClientRect(g.hWnd, &cr);

        Graphics gr(hdc);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        SolidBrush bg(GdipColor(C_BG));
        gr.FillRectangle(&bg, 0, 0, cr.right, cr.bottom);
        Pen border(GdipColor(C_BORDER), 1.0f);
        gr.DrawRectangle(&border, 0.5f, 0.5f, (float)cr.right - 1.0f, (float)cr.bottom - 1.0f);

        const COLORREF cur = g.palette.empty() ? RGB(255, 255, 0) : g.palette[g.colorIdx];

        // 구분선
        Pen sepPen(GdipColor(C_BORDER), 1.0f);
        int prevRight = -1;
        ItemKind prevKind = ItemKind::Action;
        for (const Item& it : g.items)
        {
            if (prevRight >= 0 && it.kind != prevKind)
            {
                const float sx = prevRight + SEP * 0.5f;
                gr.DrawLine(&sepPen, sx, (float)PAD + 5.0f, sx, (float)(PAD + BTN) - 5.0f);
            }
            prevRight = it.rc.right;
            prevKind  = it.kind;
        }

        for (int i = 0; i < (int)g.items.size(); ++i)
        {
            const Item& it = g.items[i];
            if (it.kind == ItemKind::Action)
            {
                if (i == g.hot || i == g.pressed)
                {
                    SolidBrush hb(GdipColor((i == g.pressed && i == g.hot) ? C_PRESSED : C_HOT));
                    RectF r((float)it.rc.left + 1.0f, (float)it.rc.top + 1.0f,
                            (float)(it.rc.right - it.rc.left) - 2.0f, (float)(it.rc.bottom - it.rc.top) - 2.0f);
                    GraphicsPath* p = RoundRectPath(r, 4.0f);
                    gr.FillPath(&hb, p);
                    delete p;
                }
                DrawActionIcon(gr, it.rc, it.action, cur);
            }
            else
            {
                const float cx = (it.rc.left + it.rc.right) * 0.5f;
                const float cy = (it.rc.top + it.rc.bottom) * 0.5f;
                const float r  = (i == g.hot) ? 7.5f : 7.0f;
                if (it.swatch == g.colorIdx)
                {
                    Pen ring(GdipColor(C_ACCENT), 2.0f);
                    gr.DrawEllipse(&ring, cx - 9.0f, cy - 9.0f, 18.0f, 18.0f);
                }
                SolidBrush sw(GdipColor(g.palette[it.swatch]));
                gr.FillEllipse(&sw, cx - r, cy - r, r * 2.0f, r * 2.0f);
                Pen swPen(GdipColor(RGB(0, 0, 0), 40), 1.0f);
                gr.DrawEllipse(&swPen, cx - r, cy - r, r * 2.0f, r * 2.0f);
            }
        }
    }

    LRESULT CALLBACK TbProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            // 클릭해도 메인 창의 활성/포커스 상태를 유지
            return MA_NOACTIVATE;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            HDC mdc = CreateCompatibleDC(hdc);
            HBITMAP mbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP obm = (HBITMAP)SelectObject(mdc, mbm);
            Paint(mdc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, obm); DeleteObject(mbm); DeleteDC(mdc);
            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;

        case WM_TIMER:
            if (wParam == ANIM_TIMER)
            {
                const double t = std::min(1.0, (double)(GetTickCount64() - g.animStart) / ANIM_MS);
                const double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);   // ease-out cubic
                SetLayeredWindowAttributes(hWnd, 0, (BYTE)(255.0 * e + 0.5), LWA_ALPHA);
                SetWindowPos(hWnd, nullptr, g.x, g.y + (int)(SLIDE_PX * (1.0 - e) + 0.5), 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (t >= 1.0) KillTimer(hWnd, ANIM_TIMER);
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int h = HitTest(pt);
            if (h != g.hot) { g.hot = h; InvalidateRect(hWnd, nullptr, FALSE); }
            if (!g.tracking)
            {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
                g.tracking = TrackMouseEvent(&tme) != FALSE;
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            g.tracking = false;
            if (g.hot != -1) { g.hot = -1; InvalidateRect(hWnd, nullptr, FALSE); }
            return 0;

        case WM_LBUTTONDOWN:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            g.pressed = HitTest(pt);
            if (g.pressed >= 0) SetCapture(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const int hit = HitTest(pt);
            const int pressed = g.pressed;
            g.pressed = -1;
            if (GetCapture() == hWnd) ReleaseCapture();
            InvalidateRect(hWnd, nullptr, FALSE);
            if (hit < 0 || hit != pressed) return 0;

            const Item& it = g.items[hit];
            if (it.kind == ItemKind::Swatch)
            {
                g.colorIdx = it.swatch;
                return 0;
            }

            // 콜백 안에서 Show/Hide가 다시 불릴 수 있으므로 값을 먼저 복사해 둔다
            const SelToolbar::ActionFn cb = g.onAction;
            const HWND owner = g.owner;
            const SelToolbar::Action action = it.action;
            const COLORREF color = g.palette.empty() ? RGB(255, 255, 0) : g.palette[g.colorIdx];
            SelToolbar::Hide();
            if (cb) cb(owner, action, color);
            return 0;
        }

        case WM_DESTROY:
            g.hWnd = nullptr;
            g.hTip = nullptr;
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    bool EnsureWindow(HWND owner)
    {
        if (g.hWnd && GetWindow(g.hWnd, GW_OWNER) == owner) return true;
        if (g.hWnd) DestroyWindow(g.hWnd);

        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc   = TbProc;
            wc.hInstance     = hInst;
            wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = TB_CLASS;
            if (!RegisterClassExW(&wc)) return false;
            registered = true;
        }

        g.hWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, TB_CLASS, nullptr, WS_POPUP,
            0, 0, 1, 1, owner, nullptr, hInst, nullptr);
        if (!g.hWnd) return false;
        SetLayeredWindowAttributes(g.hWnd, 0, 0, LWA_ALPHA);

        // DWM 둥근 모서리 + 그림자 (CS_DROPSHADOW 그림자는 페이드와 따로 놀아 DWM 그림자 사용)
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
        DwmSetWindowAttribute(g.hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        MARGINS margins = { 1, 1, 1, 1 };
        DwmExtendFrameIntoClientArea(g.hWnd, &margins);

        g.hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            g.hWnd, nullptr, hInst, nullptr);
        return true;
    }
}

void SelToolbar::Show(HWND owner, POINT anchor, const COLORREF* palette, int paletteCount, ActionFn onAction)
{
    if (!owner || !EnsureWindow(owner)) return;

    g.owner    = owner;
    g.onAction = onAction;
    g.palette.assign(palette, palette + std::max(0, paletteCount));
    if (g.colorIdx >= (int)g.palette.size()) g.colorIdx = 0;
    g.hot = g.pressed = -1;

    const int w = BuildItems();
    const int h = TB_H;
    UpdateTooltips();

    // 마우스 위치 위쪽 가운데에 표시, 화면 위로 넘치면 아래쪽에 표시
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    int x = anchor.x - w / 2;
    int y = anchor.y - h - 14;
    if (y < wa.top) y = anchor.y + 22;
    x = std::max((int)wa.left, std::min(x, (int)wa.right - w));
    y = std::max((int)wa.top,  std::min(y, (int)wa.bottom - h));

    // 투명·살짝 아래에서 시작해 타이머로 페이드인하며 제자리로 올라온다
    g.x = x;
    g.y = y;
    g.animStart = GetTickCount64();
    SetLayeredWindowAttributes(g.hWnd, 0, 0, LWA_ALPHA);
    SetWindowPos(g.hWnd, HWND_TOP, x, y + SLIDE_PX, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g.hWnd, nullptr, FALSE);
    SetTimer(g.hWnd, ANIM_TIMER, ANIM_TICK, nullptr);
}

void SelToolbar::Hide()
{
    if (!g.hWnd || !IsWindowVisible(g.hWnd)) return;
    KillTimer(g.hWnd, ANIM_TIMER);
    if (g.hTip) SendMessageW(g.hTip, TTM_POP, 0, 0);
    if (GetCapture() == g.hWnd) ReleaseCapture();
    g.hot = g.pressed = -1;
    ShowWindow(g.hWnd, SW_HIDE);
}

bool SelToolbar::IsVisible()
{
    return g.hWnd && IsWindowVisible(g.hWnd);
}
