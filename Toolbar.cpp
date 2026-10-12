// Toolbar.cpp : 툴바 / 상태바 / PNG 아이콘 그리기
//

#include "framework.h"
#include "Toolbar.h"

bool g_toolbarHidden = false;

// 툴바 내부 그리기는 항상 전체 높이 기준
#undef TOOLBAR_HEIGHT
#define TOOLBAR_HEIGHT TOOLBAR_FULL_HEIGHT
#include "Lang.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <unordered_map>

// ── PNG 아이콘 캐시 ───────────────────────────────────────────────────────────
static std::unordered_map<int, Gdiplus::Bitmap*> s_iconCache;
static std::unordered_map<int, Gdiplus::Bitmap*> s_iconCacheDisabled;

// ── 툴바 오프스크린 베이스 버퍼 ──────────────────────────────────────────────
// 아이콘·배경 등 정적 레이어를 한 번만 그리고 재사용. 호버는 이 위에만 덧그림.
static HDC     s_tbBaseDC   = nullptr;
static HBITMAP s_tbBaseBmp  = nullptr;
static HBITMAP s_tbBaseOld  = nullptr;
static int     s_tbBaseW    = 0;   // 마지막으로 렌더링한 너비

void InvalidateToolbarBaseCache()
{
    s_tbBaseW = 0; // 다음 DrawToolbar 호출 시 강제 재생성
}

// ── 상태바 오프스크린 베이스 버퍼 ────────────────────────────────────────────
static HDC     s_sbBaseDC  = nullptr;
static HBITMAP s_sbBaseBmp = nullptr;
static HBITMAP s_sbBaseOld = nullptr;
static int     s_sbBaseW   = 0;  // 마지막으로 렌더링한 너비 (0 = 무효)
static int     s_sbBasePage = -1; // 마지막으로 렌더링한 페이지
static float   s_sbBaseZoom = 0.f; // 마지막으로 렌더링한 줌
static int     s_sbBaseHover = -1; // 마지막으로 렌더링한 호버 버튼
int            g_statusHoverBtn = -1; // 0=처음 1=이전 2=다음 3=마지막, -1=없음

void InvalidateStatusBarCache()
{
    s_sbBaseW = 0; // 다음 DrawStatusBar 호출 시 강제 재생성
}

static Gdiplus::Bitmap* LoadPngBitmap(int resId)
{
    HINSTANCE hInst = GetModuleHandle(nullptr);
    HRSRC   hRes = FindResourceW(hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hRes) return nullptr;
    HGLOBAL hMem = LoadResource(hInst, hRes);
    DWORD   sz   = SizeofResource(hInst, hRes);
    if (!hMem || !sz) return nullptr;
    void* pData = LockResource(hMem);
    HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, sz);
    if (!hBuf) return nullptr;
    void* pBuf = GlobalLock(hBuf);
    if (!pBuf) { GlobalFree(hBuf); return nullptr; }
    memcpy(pBuf, pData, sz);
    GlobalUnlock(hBuf);
    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hBuf, TRUE, &pStream))) { GlobalFree(hBuf); return nullptr; }
    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(pStream);
    pStream->Release();
    if (bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return nullptr; }
    return bmp;
}

static Gdiplus::Bitmap* GetCachedIcon(int resId, bool enabled)
{
    if (enabled)
    {
        auto it = s_iconCache.find(resId);
        if (it != s_iconCache.end()) return it->second;
        Gdiplus::Bitmap* bmp = LoadPngBitmap(resId);
        s_iconCache[resId] = bmp;
        return bmp;
    }
    else
    {
        auto it = s_iconCacheDisabled.find(resId);
        if (it != s_iconCacheDisabled.end()) return it->second;
        Gdiplus::Bitmap* src = GetCachedIcon(resId, true);
        if (!src) { s_iconCacheDisabled[resId] = nullptr; return nullptr; }
        // disabled 버전: 그레이스케일 + 반투명 비트맵 미리 생성
        UINT w = src->GetWidth(), h = src->GetHeight();
        Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
        Gdiplus::Graphics g(bmp);
        Gdiplus::ColorMatrix cm = {};
        cm.m[0][0] = 0.33f; cm.m[0][1] = 0.33f; cm.m[0][2] = 0.33f;
        cm.m[1][0] = 0.33f; cm.m[1][1] = 0.33f; cm.m[1][2] = 0.33f;
        cm.m[2][0] = 0.33f; cm.m[2][1] = 0.33f; cm.m[2][2] = 0.33f;
        cm.m[3][3] = 0.3f;  cm.m[4][4] = 1.0f;
        Gdiplus::ImageAttributes attr;
        attr.SetColorMatrix(&cm);
        g.DrawImage(src, Gdiplus::Rect(0, 0, (INT)w, (INT)h), 0, 0, (INT)w, (INT)h, Gdiplus::UnitPixel, &attr);
        s_iconCacheDisabled[resId] = bmp;
        return bmp;
    }
}

extern bool g_twoPageView;

// ── 툴바 버튼 테이블 정의 ─────────────────────────────────────────────────────
const ToolbarButton g_toolbarButtons[] = {
    { nullptr, nullptr, IDM_FILE_OPEN,        false, IDB_OPEN_PNG,      0,  0  },
    { nullptr, nullptr, IDM_SAVE,             true,  IDB_SAVE_PNG,      0,  0  },
    { nullptr, nullptr, IDM_PRINT,            true,  IDB_PRINTER_PNG,   0,  0  },
    { L"|",    nullptr, 0,                    false, 0,                 0,  0  },
    { nullptr, nullptr, IDM_ZOOM_OUT,         true,  IDB_MINUS_PNG,     0,  0  },
    { nullptr, nullptr, IDM_ZOOM_IN,          true,  IDB_PLUS_PNG,      0,  0  },
    { nullptr, L"100% (Ctrl+0)", IDM_ZOOM_RESET, true, IDB_ACTUAL_PNG,  0,  0  },
    { L"|",    nullptr, 0,                    false, 0,                 0,  0  },
    { nullptr, nullptr, IDM_FIT_WIDTH,        true,  IDB_WIDTH_PNG,     0,  0  },
    { nullptr, nullptr, IDM_FIT_PAGE,         true,  IDB_HEIGHT_PNG,    0,  0  },
    { L"|",    nullptr, 0,                    false, 0,                 0,  0  },
    { nullptr, nullptr, IDM_HAND_TOOL,        true,  IDB_HAND_PNG,      18, 0  },
    { L"|",    nullptr, 0,                    false, 0,                 0,  0  },
    { nullptr, nullptr, IDM_HIGHLIGHT_MODE,   true,  IDB_HIGHLIGHT_PNG, 0,  0  },
    { L"#",    nullptr, IDM_HIGHLIGHT_COLOR,  true,  0,                 0,  26 },
    { L"|",    nullptr, 0,                    false, 0,                 0,  0  },
    { nullptr, nullptr, IDM_INK_MODE,         true,  IDB_INK_PNG,       0,  0  },
    { L"#",    nullptr, IDM_INK_COLOR,        true,  0,                 0,  26 },
    { L"|",    nullptr, 0,                    false, 0,                 0,  0  },
    { nullptr, nullptr, IDM_TOGGLE_SIDEBAR,   true,  IDB_BOOKMARK_PNG,  0,  0  },
    { nullptr, nullptr, IDM_TOGGLE_THUMBNAIL, true,  IDB_THUMBNAIL_PNG, 0,  0  },
    { L"|",    nullptr, 0,                    false, 0,                 0,  0  },
    { nullptr, nullptr, IDM_AUTO_SCROLL,      true,  IDB_SCROLL_PNG,    0,  0  },
    { L"|",    nullptr, 0,                    false, 0,                 0,  0  },
    { nullptr, nullptr, IDM_EXTRACT_TEXT,     true,  IDB_TEXT_PNG,      0,  0  },
    { L"|",    nullptr, 0,                    false, 0,                 0,  0  },
    { nullptr, nullptr, IDM_SLIDESHOW,        true,  IDB_SLIDESHOW_PNG, 0,  0  },
    { L"|",    nullptr, 0,                    false, 0,                 0,  0  },
    { nullptr, nullptr, IDM_GOTO_PAGE,        true,  IDB_JUMP_PNG,      0,  0  },
};
const int g_toolbarButtonCount = _countof(g_toolbarButtons);

// 런타임 언어에 맞는 툴바 버튼 툴팁 반환
const wchar_t* GetToolbarTooltip(int index)
{
    switch (index)
    {
    case 0:  return LS(STR_TIP_BTN_OPEN);
    case 1:  return LS(STR_TIP_BTN_SAVE);
    case 2:  return LS(STR_TIP_PRINT);
    case 4:  return LS(STR_TIP_BTN_ZOOMOUT);
    case 5:  return LS(STR_TIP_BTN_ZOOMIN);
    case 8:  return LS(STR_TIP_BTN_FITWIDTH);
    case 9:  return LS(STR_TIP_BTN_FITPAGE);
    case 11: return LS(STR_TIP_BTN_HAND);
    case 13: return LS(STR_TIP_BTN_HIGHLIGHT);
    case 14: return LS(STR_TIP_BTN_HLCOLOR);
    case 16: return LS(STR_TIP_BTN_INK);
    case 17: return LS(STR_TIP_BTN_INKCOLOR);
    case 19: return LS(STR_TIP_BTN_BOOKMARK);
    case 20: return LS(STR_TIP_BTN_THUMBNAIL);
    case 22: return LS(STR_TIP_BTN_AUTOSCROLL);
    case 24: return LS(STR_TIP_BTN_EXTRACT);
    case 26: return LS(STR_TIP_BTN_SLIDESHOW);
    case 28: return LS(STR_TIP_BTN_GOTO);
    default: return (index >= 0 && index < g_toolbarButtonCount) ? g_toolbarButtons[index].tooltip : nullptr;
    }
}

// ── 버튼 영역 계산 ────────────────────────────────────────────────────────────
RECT GetToolbarButtonRect(int index)
{
    if (index < 0 || index >= g_toolbarButtonCount)
        return { 0, 0, 0, 0 };
    int btnX = TOOLBAR_MARGIN;
    for (int i = 0; i < index; ++i)
    {
        if (g_toolbarButtons[i].label && g_toolbarButtons[i].label[0] == L'|')
            btnX += 12;
        else {
            int w = g_toolbarButtons[i].width > 0 ? g_toolbarButtons[i].width : TOOLBAR_BTN_W;
            btnX += w + 0;
        }
    }
    int btnW = (index >= 0 && index < g_toolbarButtonCount && g_toolbarButtons[index].width > 0)
        ? g_toolbarButtons[index].width : TOOLBAR_BTN_W;
    return {
        btnX,
        (TOOLBAR_HEIGHT - TOOLBAR_BTN_H) / 2,
        btnX + btnW,
        (TOOLBAR_HEIGHT - TOOLBAR_BTN_H) / 2 + TOOLBAR_BTN_H
    };
}

RECT GetScreenLockToolbarBtnRect(const RECT& clientRect)
{
    const int findBtnW = TOOLBAR_BTN_W + 52;
    int x = clientRect.right - TOOLBAR_MARGIN - findBtnW - TB_GAP - TOOLBAR_BTN_W - TB_GAP - TOOLBAR_BTN_W - TB_GAP - TOOLBAR_BTN_W - TB_GAP - TOOLBAR_BTN_W;
    int y = (TOOLBAR_HEIGHT - TOOLBAR_BTN_H) / 2;
    return { x, y, x + TOOLBAR_BTN_W, y + TOOLBAR_BTN_H };
}

RECT GetDarkModeToolbarBtnRect(const RECT& clientRect)
{
    const int findBtnW = TOOLBAR_BTN_W + 52;
    int x = clientRect.right - TOOLBAR_MARGIN - findBtnW - TB_GAP - TOOLBAR_BTN_W;
    int y = (TOOLBAR_HEIGHT - TOOLBAR_BTN_H) / 2;
    return { x, y, x + TOOLBAR_BTN_W, y + TOOLBAR_BTN_H };
}

RECT GetSpotlightToolbarBtnRect(const RECT& clientRect)
{
    const int findBtnW = TOOLBAR_BTN_W + 52;
    int x = clientRect.right - TOOLBAR_MARGIN - findBtnW - TB_GAP - TOOLBAR_BTN_W - TB_GAP - TOOLBAR_BTN_W;
    int y = (TOOLBAR_HEIGHT - TOOLBAR_BTN_H) / 2;
    return { x, y, x + TOOLBAR_BTN_W, y + TOOLBAR_BTN_H };
}

RECT GetLaserPointerToolbarBtnRect(const RECT& clientRect)
{
    const int findBtnW = TOOLBAR_BTN_W + 52;
    int x = clientRect.right - TOOLBAR_MARGIN - findBtnW - TB_GAP - TOOLBAR_BTN_W - TB_GAP - TOOLBAR_BTN_W - TB_GAP - TOOLBAR_BTN_W;
    int y = (TOOLBAR_HEIGHT - TOOLBAR_BTN_H) / 2;
    return { x, y, x + TOOLBAR_BTN_W, y + TOOLBAR_BTN_H };
}

RECT GetFindToolbarBtnRect(const RECT& clientRect)
{
    const int btnW = TOOLBAR_BTN_W + 52;
    int x = clientRect.right - TOOLBAR_MARGIN - btnW;
    int y = (TOOLBAR_HEIGHT - TOOLBAR_BTN_H) / 2;
    return { x, y, x + btnW, y + TOOLBAR_BTN_H };
}

RECT GetQuickJumpToolbarBtnRect(const RECT& clientRect)
{
    const int findBtnW = TOOLBAR_BTN_W + 52;
    const int qjBtnW   = TOOLBAR_BTN_W; // 아이콘만
    // QuickJump는 ScreenLock 바로 왼쪽
    int screenLockX = clientRect.right - TOOLBAR_MARGIN - findBtnW - TB_GAP - TOOLBAR_BTN_W - TB_GAP - TOOLBAR_BTN_W - TB_GAP - TOOLBAR_BTN_W - TB_GAP - TOOLBAR_BTN_W;
    int x = screenLockX - TB_GAP - qjBtnW;
    int y = (TOOLBAR_HEIGHT - TOOLBAR_BTN_H) / 2;
    return { x, y, x + qjBtnW, y + TOOLBAR_BTN_H };
}

RECT GetStatusPrevBtnRect(const RECT& clientRect)
{
    int cy = clientRect.bottom - STATUSBAR_HEIGHT / 2;
    int cx = clientRect.right  / 2;
    int btnW = 30, btnH = 26;
    return { cx - 72 - btnW, cy - btnH / 2, cx - 72, cy + btnH / 2 };
}

RECT GetStatusNextBtnRect(const RECT& clientRect)
{
    int cy = clientRect.bottom - STATUSBAR_HEIGHT / 2;
    int cx = clientRect.right  / 2;
    int btnW = 30, btnH = 26;
    return { cx + 72, cy - btnH / 2, cx + 72 + btnW, cy + btnH / 2 };
}

RECT GetStatusFirstBtnRect(const RECT& clientRect)
{
    RECT prev = GetStatusPrevBtnRect(clientRect);
    int btnW = 30;
    return { prev.left - 4 - btnW, prev.top, prev.left - 4, prev.bottom };
}

RECT GetStatusLastBtnRect(const RECT& clientRect)
{
    RECT next = GetStatusNextBtnRect(clientRect);
    int btnW = 30;
    return { next.right + 4, next.top, next.right + 4 + btnW, next.bottom };
}

// ── GetHighlightColorBtnRect (하위 호환용 — 버튼 배열에서 직접 계산) ─────────
RECT GetHighlightColorBtnRect()
{
    for (int i = 0; i < g_toolbarButtonCount; ++i)
        if (g_toolbarButtons[i].commandId == IDM_HIGHLIGHT_COLOR)
            return GetToolbarButtonRect(i);
    return {};
}

// ── GetInkColorBtnRect ────────────────────────────────────────────────────────
RECT GetInkColorBtnRect()
{
    for (int i = 0; i < g_toolbarButtonCount; ++i)
        if (g_toolbarButtons[i].commandId == IDM_INK_COLOR)
            return GetToolbarButtonRect(i);
    return {};
}

// ── HitTestToolbar ────────────────────────────────────────────────────────────
int HitTestToolbar(int x, int y)
{
    if (y < 0 || y >= TOOLBAR_HEIGHT) return -1;
    for (int i = 0; i < g_toolbarButtonCount; ++i)
    {
        if (g_toolbarButtons[i].label && g_toolbarButtons[i].label[0] == L'|') continue;
        RECT r = GetToolbarButtonRect(i);
        POINT pt = { x, y };
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

// ── DrawPngIcon ───────────────────────────────────────────────────────────────
void DrawPngIcon(HDC hdc, int resId, const RECT& rc, int iconSize, bool enabled, bool darken)
{
    Gdiplus::Bitmap* bmp = GetCachedIcon(resId, enabled);
    if (!bmp) return;
    Gdiplus::Graphics gfx(hdc);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    int ix = rc.left + (rc.right  - rc.left - iconSize) / 2;
    int iy = rc.top  + (rc.bottom - rc.top  - iconSize) / 2;
    if (darken)
    {
        Gdiplus::ColorMatrix cm = {
            0.75f, 0, 0, 0, 0,
            0, 0.75f, 0, 0, 0,
            0, 0, 0.75f, 0, 0,
            0, 0, 0, 1, 0,
            0, 0, 0, 0, 1 };
        Gdiplus::ImageAttributes ia;
        ia.SetColorMatrix(&cm);
        gfx.DrawImage(bmp, Gdiplus::Rect(ix, iy, iconSize, iconSize),
            0, 0, (INT)bmp->GetWidth(), (INT)bmp->GetHeight(),
            Gdiplus::UnitPixel, &ia);
        return;
    }
    gfx.DrawImage(bmp, ix, iy, iconSize, iconSize);
}

// ── DrawGradientBackground ────────────────────────────────────────────────────
void DrawGradientBackground(HDC hdc, const RECT& rect)
{
    TRIVERTEX vert[2] = {};
    vert[0].x = rect.left;  vert[0].y = rect.top;
    vert[0].Red = 0x3A00; vert[0].Green = 0x3A00; vert[0].Blue = 0x4000; vert[0].Alpha = 0;
    vert[1].x = rect.right; vert[1].y = rect.bottom;
    vert[1].Red = 0x2D00; vert[1].Green = 0x2D00; vert[1].Blue = 0x3200; vert[1].Alpha = 0;
    GRADIENT_RECT gRect = { 0, 1 };
    GradientFill(hdc, vert, 2, &gRect, 1, GRADIENT_FILL_RECT_V);
}

// ── DrawSoftShadow ────────────────────────────────────────────────────────────
void DrawSoftShadow(HDC hdc, int x, int y, int w, int h)
{
    struct ShadowLayer { int offset; COLORREF color; };
    static const ShadowLayer layers[] = {
        { 6, RGB(0,   0,   0)  },
        { 4, RGB(10, 10,  12)  },
        { 2, RGB(20, 20,  24)  },
    };
    for (auto& layer : layers)
    {
        RECT sr = { x + layer.offset, y + layer.offset,
                    x + w + layer.offset, y + h + layer.offset };
        HBRUSH br = CreateSolidBrush(layer.color);
        FillRect(hdc, &sr, br);
        DeleteObject(br);
    }
}

// ── DrawToolbar (내부 헬퍼: 지정 HDC에 툴바 전체 내용을 그림) ──────────────
static void DrawToolbarContent(HDC hdc, const RECT& clientRect, int hoverBtn)
{
    RECT tbRect = { 0, 0, clientRect.right, TOOLBAR_HEIGHT };

    const bool dark = false;
    COLORREF bgColor      = dark ? RGB(32,  32,  32)  : RGB(238, 239, 245);  // Fluent 쿨 그레이
    COLORREF borderColor  = dark ? RGB(55,  55,  55)  : RGB(218, 219, 228);
    COLORREF sepColor     = dark ? RGB(70,  70,  78)  : RGB(200, 201, 212);
    COLORREF iconTextClr  = dark ? RGB(200, 200, 210) : RGB(30,  30,  30);
    COLORREF iconDisClr   = dark ? RGB(90,  90, 100)  : RGB(170, 170, 170);
    COLORREF hoverBg      = dark ? RGB(110, 112, 130) : RGB(220, 221, 232);
    COLORREF hoverBorder  = dark ? RGB(160, 163, 185) : RGB(195, 196, 210);
    COLORREF activeBg     = dark ? RGB(60,  100, 170) : RGB(205, 228, 255);
    COLORREF activeBorder = dark ? RGB(100, 150, 230) : RGB(140, 190, 245);
    COLORREF activeText   = dark ? RGB(180, 210, 255) : RGB(0,   90,  210);
    COLORREF hoverText    = dark ? RGB(180, 210, 255) : RGB(0,   70,  170);

    HBRUSH bgBrush = CreateSolidBrush(bgColor);
    FillRect(hdc, &tbRect, bgBrush);
    DeleteObject(bgBrush);

    HPEN linePen = CreatePen(PS_SOLID, 1, borderColor);
    HPEN oldPen  = static_cast<HPEN>(SelectObject(hdc, linePen));
    MoveToEx(hdc, 0, TOOLBAR_HEIGHT - 1, nullptr);
    LineTo(hdc, clientRect.right, TOOLBAR_HEIGHT - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(linePen);

    static HFONT hFont = nullptr;
    if (!hFont)
        hFont = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFont));
    SetBkMode(hdc, TRANSPARENT);

    for (int i = 0; i < g_toolbarButtonCount; ++i)
    {
        const auto& btn = g_toolbarButtons[i];
        RECT r = GetToolbarButtonRect(i);

        if (btn.label && btn.label[0] == L'|')
        {
            int cx = r.left + 4;
            HPEN sepPen = CreatePen(PS_SOLID, 1, sepColor);
            HPEN prev   = static_cast<HPEN>(SelectObject(hdc, sepPen));
            MoveToEx(hdc, cx, r.top + 2, nullptr);
            LineTo(hdc, cx, r.bottom - 2);
            SelectObject(hdc, prev);
            DeleteObject(sepPen);
            continue;
        }

        bool enabled = !btn.needsDoc || g_pdfDoc != nullptr;
        bool hovered = (i == hoverBtn) && enabled;
        bool active  = ((btn.commandId == IDM_HAND_TOOL)         && g_handMode                          && enabled)
                    || ((btn.commandId == IDM_TOGGLE_SIDEBAR)    && g_sidebarVisible && g_sidebarTab == 0 && enabled)
                    || ((btn.commandId == IDM_TOGGLE_THUMBNAIL)  && g_sidebarVisible && g_sidebarTab == 1 && enabled)
                    || ((btn.commandId == IDM_AUTO_SCROLL)       && g_autoScrolling                       && enabled)
                    || ((btn.commandId == IDM_SLIDESHOW)         && g_slideshowActive                     && enabled)
                    || ((btn.commandId == IDM_HIGHLIGHT_MODE)    && g_highlightMode                       && enabled)
                    || ((btn.commandId == IDM_INK_MODE)         && g_inkMode                               && enabled);

        if (hovered || active)
        {
            HBRUSH hoverBr = CreateSolidBrush(active ? activeBg : hoverBg);
            HPEN   hoverPn = CreatePen(PS_SOLID, 1, active ? activeBorder : hoverBorder);
            HPEN   prevPn  = static_cast<HPEN>(SelectObject(hdc, hoverPn));
            HBRUSH prevBr  = static_cast<HBRUSH>(SelectObject(hdc, hoverBr));
            RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
            SelectObject(hdc, prevPn); SelectObject(hdc, prevBr);
            DeleteObject(hoverBr);    DeleteObject(hoverPn);
        }

        SetTextColor(hdc, enabled
            ? ((active) ? activeText : (hovered ? hoverText : iconTextClr))
            : iconDisClr);

        if (btn.commandId == IDM_HIGHLIGHT_COLOR || btn.commandId == IDM_INK_COLOR)
        {
            int sz = 16;
            int cx = (r.left + r.right) / 2;
            int cy = (r.top + r.bottom) / 2;
            RECT cr = { cx - sz/2, cy - sz/2, cx + sz/2, cy + sz/2 };
            COLORREF swatchClr = (btn.commandId == IDM_INK_COLOR)
                ? (enabled ? g_inkColor : RGB(200,200,200))
                : (enabled ? g_highlightColor : RGB(200,200,200));
            HBRUSH colBr = CreateSolidBrush(swatchClr);
            HPEN   borPn = (HPEN)GetStockObject(NULL_PEN);
            HPEN   oPn   = static_cast<HPEN>(SelectObject(hdc, borPn));
            HBRUSH oBr   = static_cast<HBRUSH>(SelectObject(hdc, colBr));
            RoundRect(hdc, cr.left, cr.top, cr.right, cr.bottom, 4, 4);
            SelectObject(hdc, oPn); SelectObject(hdc, oBr);
            DeleteObject(colBr);
        }
        else if (btn.pngResId != 0)
        {
            int iconSize = btn.iconSize > 0 ? btn.iconSize : 18;
            DrawPngIcon(hdc, btn.pngResId, r, iconSize, enabled);
        }
        else if (btn.label)
        {
            DrawTextW(hdc, btn.label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // 오른쪽 고정 버튼들
    auto drawRightSep = [&](int xCenter)
    {
        HPEN sp = CreatePen(PS_SOLID, 1, sepColor);
        HPEN op = static_cast<HPEN>(SelectObject(hdc, sp));
        int  y1 = (TOOLBAR_HEIGHT - TOOLBAR_BTN_H) / 2 + 3;
        int  y2 = (TOOLBAR_HEIGHT + TOOLBAR_BTN_H) / 2 - 3;
        MoveToEx(hdc, xCenter, y1, nullptr);
        LineTo  (hdc, xCenter, y2);
        SelectObject(hdc, op);
        DeleteObject(sp);
    };

    {
        RECT r      = GetScreenLockToolbarBtnRect(clientRect);
        bool hovered = (hoverBtn == 9997);
        bool active  = g_screenLock;
        if (hovered || active)
        {
            HBRUSH hBr = CreateSolidBrush(active ? activeBg : hoverBg);
            HPEN   hPn = CreatePen(PS_SOLID, 1, active ? activeBorder : hoverBorder);
            HPEN   oPn = static_cast<HPEN>(SelectObject(hdc, hPn));
            HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, hBr));
            RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
            SelectObject(hdc, oPn); SelectObject(hdc, oBr);
            DeleteObject(hPn);      DeleteObject(hBr);
        }
        DrawPngIcon(hdc, g_screenLock ? IDB_LOCK_PNG : IDB_UNLOCK_PNG, r, 18, true);
    }
    // ① ScreenLock | LaserPointer 사이 구분선
    {
        RECT sl = GetScreenLockToolbarBtnRect(clientRect);
        RECT lp = GetLaserPointerToolbarBtnRect(clientRect);
        drawRightSep((lp.right + sl.left) / 2);
    }
    {
        RECT r       = GetSpotlightToolbarBtnRect(clientRect);
        bool hovered = (hoverBtn == 9996);
        bool active  = g_spotlightMode;
        if (hovered || active)
        {
            HBRUSH hBr = CreateSolidBrush(active ? activeBg : hoverBg);
            HPEN   hPn = CreatePen(PS_SOLID, 1, active ? activeBorder : hoverBorder);
            HPEN   oPn = static_cast<HPEN>(SelectObject(hdc, hPn));
            HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, hBr));
            RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
            SelectObject(hdc, oPn); SelectObject(hdc, oBr);
            DeleteObject(hPn);      DeleteObject(hBr);
        }
        DrawPngIcon(hdc, IDB_SPOTLIGHT_PNG, r, 18, true);
    }
    // ② Spotlight | DarkMode 사이 구분선
    {
        RECT sp = GetSpotlightToolbarBtnRect(clientRect);
        RECT dm = GetDarkModeToolbarBtnRect(clientRect);
        drawRightSep((dm.right + sp.left) / 2);
    }
    {
        RECT r       = GetLaserPointerToolbarBtnRect(clientRect);
        bool hovered = (hoverBtn == 9995);
        bool active  = g_laserMode;
        if (hovered || active)
        {
            HBRUSH hBr = CreateSolidBrush(active ? activeBg : hoverBg);
            HPEN   hPn = CreatePen(PS_SOLID, 1, active ? activeBorder : hoverBorder);
            HPEN   oPn = static_cast<HPEN>(SelectObject(hdc, hPn));
            HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, hBr));
            RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
            SelectObject(hdc, oPn); SelectObject(hdc, oBr);
            DeleteObject(hPn);      DeleteObject(hBr);
        }
        DrawPngIcon(hdc, IDB_LASER_PNG, r, 18, true);
    }
    {
        RECT r       = GetDarkModeToolbarBtnRect(clientRect);
        bool hovered = (hoverBtn == 9998);
        bool active  = g_darkMode;
        if (hovered || active)
        {
            HBRUSH hBr = CreateSolidBrush(active ? activeBg : hoverBg);
            HPEN   hPn = CreatePen(PS_SOLID, 1, active ? activeBorder : hoverBorder);
            HPEN   oPn = static_cast<HPEN>(SelectObject(hdc, hPn));
            HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, hBr));
            RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
            SelectObject(hdc, oPn); SelectObject(hdc, oBr);
            DeleteObject(hPn);      DeleteObject(hBr);
        }
        DrawPngIcon(hdc, IDB_DARKMODE_PNG, r, 18, true);
    }
    // ③ DarkMode | Find 사이 구분선
    {
        RECT dm = GetDarkModeToolbarBtnRect(clientRect);
        RECT fn = GetFindToolbarBtnRect(clientRect);
        drawRightSep((dm.right + fn.left) / 2);
    }
    {
        RECT r       = GetQuickJumpToolbarBtnRect(clientRect);
        bool hovered = (hoverBtn == 9994);
        bool active  = g_quickJumpVisible;
        if (hovered || active)
        {
            HBRUSH hBr = CreateSolidBrush(active ? activeBg : hoverBg);
            HPEN   hPn = CreatePen(PS_SOLID, 1, active ? activeBorder : hoverBorder);
            HPEN   oPn = static_cast<HPEN>(SelectObject(hdc, hPn));
            HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, hBr));
            RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
            SelectObject(hdc, oPn); SelectObject(hdc, oBr);
            DeleteObject(hPn);      DeleteObject(hBr);
        }
        DrawPngIcon(hdc, IDB_GOTO_PNG, r, 18, true);
    }
    {
        RECT r       = GetFindToolbarBtnRect(clientRect);
        bool hovered = (hoverBtn == 9999);
        bool active  = g_findBarVisible;
        if (hovered || active)
        {
            HBRUSH hBr = CreateSolidBrush(active ? activeBg : hoverBg);
            HPEN   hPn = CreatePen(PS_SOLID, 1, active ? activeBorder : hoverBorder);
            HPEN   oPn = static_cast<HPEN>(SelectObject(hdc, hPn));
            HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, hBr));
            RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
            SelectObject(hdc, oPn); SelectObject(hdc, oBr);
            DeleteObject(hPn);      DeleteObject(hBr);
        }
        RECT iconR = { r.left, r.top, r.left + TOOLBAR_BTN_W, r.bottom };
        DrawPngIcon(hdc, IDB_SEARCH_PNG, iconR, 18, true);
        RECT textR = { r.left + TOOLBAR_BTN_W - 2, r.top, r.right, r.bottom };
        SetTextColor(hdc, (hovered || active) ? (active ? activeText : hoverText) : iconTextClr);
        DrawTextW(hdc, LS(STR_SEARCH_LABEL), -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, oldFont);
    // hFont는 정적 캐시이므로 삭제하지 않음
}

// ── DrawToolbar ───────────────────────────────────────────────────────────────
// 전략:
//   1) 창 너비 변경 시에만 베이스 버퍼(호버 없음)를 재생성 → 모든 DrawImage 호출 1회로 집약
//   2) 호버/액티브가 바뀌면 베이스를 작업 버퍼에 BitBlt 복사 후 변경된 버튼 영역만 덧그림
//   3) 최종 결과를 목적지 hdc에 BitBlt 1회로 전송
void DrawToolbar(HDC hdc, const RECT& clientRect)
{
    if (g_toolbarHidden) return;
    const int tbW = clientRect.right;

    // 베이스 버퍼 생성/재생성 (너비 변경 또는 캐시 무효화 시)
    if (!s_tbBaseDC || tbW != s_tbBaseW)
    {
        if (s_tbBaseDC)
        {
            SelectObject(s_tbBaseDC, s_tbBaseOld);
            DeleteObject(s_tbBaseBmp);
            DeleteDC(s_tbBaseDC);
        }
        s_tbBaseDC  = CreateCompatibleDC(hdc);
        s_tbBaseBmp = CreateCompatibleBitmap(hdc, tbW, TOOLBAR_HEIGHT);
        s_tbBaseOld = static_cast<HBITMAP>(SelectObject(s_tbBaseDC, s_tbBaseBmp));
        s_tbBaseW   = tbW;

        // 베이스 버퍼: 호버 없음(-1) 상태로 전체 렌더링 (아이콘 DrawImage 여기서만 실행)
        DrawToolbarContent(s_tbBaseDC, clientRect, -1);
    }

    // 호버/액티브가 없으면 베이스를 그대로 복사
    const bool anyHover = (g_hoverButton != -1);
    if (!anyHover)
    {
        BitBlt(hdc, 0, 0, tbW, TOOLBAR_HEIGHT, s_tbBaseDC, 0, 0, SRCCOPY);
        return;
    }

    // 호버/액티브 버튼이 있으면:
    // 작업 버퍼(s_tbBaseDC 위)에 베이스를 복사 → 변경 버튼 영역만 덧그림 → 화면 전송
    // 작업 버퍼를 별도로 만들지 않고 hdc가 이미 백버퍼 DC이므로 그 위에 직접 그린다.
    BitBlt(hdc, 0, 0, tbW, TOOLBAR_HEIGHT, s_tbBaseDC, 0, 0, SRCCOPY);

    // 호버/액티브 버튼 영역만 다시 그리기
    // — 해당 버튼 rect로 클립하여 DrawToolbarContent의 불필요한 픽셀 쓰기를 막음
    auto repaintBtn = [&](int btnIdx)
    {
        RECT r = {};
        if      (btnIdx == 9999) r = GetFindToolbarBtnRect(clientRect);
        else if (btnIdx == 9998) r = GetDarkModeToolbarBtnRect(clientRect);
        else if (btnIdx == 9997) r = GetScreenLockToolbarBtnRect(clientRect);
        else if (btnIdx == 9996) r = GetSpotlightToolbarBtnRect(clientRect);
        else if (btnIdx == 9995) r = GetLaserPointerToolbarBtnRect(clientRect);
        else if (btnIdx == 9994) r = GetQuickJumpToolbarBtnRect(clientRect);
        else if (btnIdx >= 0 && btnIdx < g_toolbarButtonCount)
            r = GetToolbarButtonRect(btnIdx);
        else return;

        // 베이스에서 해당 버튼 영역 복원 후 호버 상태로 덧그림
        HRGN hClip = CreateRectRgn(r.left - 2, r.top - 2, r.right + 2, r.bottom + 2);
        SelectClipRgn(hdc, hClip);
        // 먼저 베이스 상태(배경+아이콘)를 복사
        BitBlt(hdc, r.left - 2, r.top - 2, (r.right - r.left) + 4, (r.bottom - r.top) + 4,
               s_tbBaseDC, r.left - 2, r.top - 2, SRCCOPY);
        // 호버 강조(배경 하이라이트 + 아이콘)만 재그리기
        DrawToolbarContent(hdc, clientRect, g_hoverButton);
        SelectClipRgn(hdc, nullptr);
        DeleteObject(hClip);
    };

    repaintBtn(g_hoverButton);
}

// ── DrawStatusBar (내부 헬퍼: 지정 HDC에 상태바 전체 내용을 그림) ──────────
static void DrawStatusBarContent(HDC hdc, const RECT& clientRect)
{
    RECT sbRect = {
        0,
        clientRect.bottom - STATUSBAR_HEIGHT,
        clientRect.right,
        clientRect.bottom
    };

    // 상태바 내에서는 항상 (0,0) 기준으로 그림
    RECT localRect = { 0, 0, clientRect.right, STATUSBAR_HEIGHT };

    HBRUSH sbBrush = CreateSolidBrush(GetSysColor(COLOR_MENUBAR));
    FillRect(hdc, &localRect, sbBrush);
    DeleteObject(sbBrush);

    HPEN linePen = CreatePen(PS_SOLID, 1, RGB(210, 210, 210));
    HPEN oldPen  = static_cast<HPEN>(SelectObject(hdc, linePen));
    MoveToEx(hdc, 0, 0, nullptr);
    LineTo(hdc, clientRect.right, 0);
    SelectObject(hdc, oldPen);
    DeleteObject(linePen);

    static HFONT hFont = nullptr;
    if (!hFont)
        hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFont));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(60, 60, 60));

    WCHAR statusText[256];
    wcscpy_s(statusText, LS(STR_STATUS_READY));

    // 버퍼 내부 좌표로 변환하는 오프셋
    const int offY = -(clientRect.bottom - STATUSBAR_HEIGHT);

    // clientRect와 동일한 논리 좌표계를 유지하되,
    // 실제 DC는 상태바 높이만큼의 비트맵이므로 Y를 오프셋
    auto toLocal = [&](RECT r) -> RECT {
        r.top    += offY;
        r.bottom += offY;
        return r;
    };

    if (g_pdfDoc)
    {
        const size_t pos = g_filePath.find_last_of(L"\\/");
        const std::wstring fn = (pos != std::wstring::npos)
            ? g_filePath.substr(pos + 1) : g_filePath;
        swprintf_s(statusText, L"  %s", fn.c_str());

        WCHAR zoomText[32];
        swprintf_s(zoomText, L"%d%%  ", static_cast<int>(g_zoom * 100.0f));
        RECT zoomRect = toLocal({ clientRect.right - 80, sbRect.top, clientRect.right, sbRect.bottom });
        DrawTextW(hdc, zoomText, -1, &zoomRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        WCHAR pageInfo[64];
        if (g_twoPageView)
        {
            int leftPage  = (g_currentPage % 2 == 0) ? g_currentPage : g_currentPage - 1;
            int rightPage = leftPage + 1;
            if (rightPage < g_pageCount)
                swprintf_s(pageInfo, L"%d - %d / %d", leftPage + 1, rightPage + 1, g_pageCount);
            else
                swprintf_s(pageInfo, L"%d / %d", leftPage + 1, g_pageCount);
        }
        else
        {
            swprintf_s(pageInfo, L"%d / %d", g_currentPage + 1, g_pageCount);
        }
        RECT pageRect = toLocal({ 0, sbRect.top, clientRect.right, sbRect.bottom });
        DrawTextW(hdc, pageInfo, -1, &pageRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        bool canPrev = g_currentPage > 0;
        bool canNext = g_twoPageView
            ? g_currentPage + (g_currentPage % 2 == 0 ? 2 : 1) < g_pageCount
            : g_currentPage < g_pageCount - 1;

        auto drawNavBtn = [&](const RECT& rc, int resId, bool enabled, int hoverId) {
            RECT lrc = toLocal(rc);
            DrawPngIcon(hdc, resId, lrc, 18, enabled, enabled && g_statusHoverBtn == hoverId);
        };
        drawNavBtn(GetStatusPrevBtnRect(clientRect),  IDB_PREVPAGE_PNG,  canPrev, 1);
        drawNavBtn(GetStatusNextBtnRect(clientRect),  IDB_NEXTPAGE_PNG,  canNext, 2);
        drawNavBtn(GetStatusFirstBtnRect(clientRect), IDB_FIRSTPAGE_PNG, canPrev, 0);
        drawNavBtn(GetStatusLastBtnRect(clientRect),  IDB_LASTPAGE_PNG,  canNext, 3);
    }

    RECT textRect = toLocal({ 0, sbRect.top, clientRect.right - 90, sbRect.bottom });
    DrawTextW(hdc, statusText, -1, &textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
}

// ── DrawStatusBar ─────────────────────────────────────────────────────────────
void DrawStatusBar(HDC hdc, const RECT& clientRect)
{
    const int sbW = clientRect.right;

    // 페이지·줌·너비 중 하나라도 바뀌면 베이스 버퍼 재생성
    const bool needRebuild = (!s_sbBaseDC)
        || (sbW != s_sbBaseW)
        || (g_currentPage != s_sbBasePage)
        || (g_zoom != s_sbBaseZoom)
        || (g_statusHoverBtn != s_sbBaseHover);

    if (needRebuild)
    {
        if (s_sbBaseDC)
        {
            SelectObject(s_sbBaseDC, s_sbBaseOld);
            DeleteObject(s_sbBaseBmp);
            DeleteDC(s_sbBaseDC);
        }
        s_sbBaseDC  = CreateCompatibleDC(hdc);
        s_sbBaseBmp = CreateCompatibleBitmap(hdc, sbW, STATUSBAR_HEIGHT);
        s_sbBaseOld = static_cast<HBITMAP>(SelectObject(s_sbBaseDC, s_sbBaseBmp));
        s_sbBaseW   = sbW;
        s_sbBasePage = g_currentPage;
        s_sbBaseZoom = g_zoom;
        s_sbBaseHover = g_statusHoverBtn;

        DrawStatusBarContent(s_sbBaseDC, clientRect);
    }

    // 베이스 버퍼를 화면으로 BitBlt 1회
    BitBlt(hdc, 0, clientRect.bottom - STATUSBAR_HEIGHT,
           sbW, STATUSBAR_HEIGHT, s_sbBaseDC, 0, 0, SRCCOPY);
}

// ── DrawAlphaLine
void DrawAlphaLine(HDC hdc, int x1, int y1, int x2, int y2,
                   COLORREF color, int thickness, BYTE alpha)
{
    if (x2 < x1) std::swap(x1, x2);
    int w = x2 - x1 + thickness;
    int h = thickness + 4;
    if (w <= 0 || h <= 0) return;

    HDC     memDC = CreateCompatibleDC(hdc);
    HBITMAP hBmp  = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oBmp  = static_cast<HBITMAP>(SelectObject(memDC, hBmp));

    RECT fr = { 0, 0, w, h };
    FillRect(memDC, &fr, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    HPEN hPen = CreatePen(PS_SOLID, thickness, color);
    HPEN oPen = static_cast<HPEN>(SelectObject(memDC, hPen));
    // 버퍼 중앙에 선을 그림
    MoveToEx(memDC, 0,        h / 2, nullptr);
    LineTo  (memDC, x2 - x1, h / 2);
    SelectObject(memDC, oPen);
    DeleteObject(hPen);

    BLENDFUNCTION bf = {};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = alpha;
    // y1이 선의 중앙이 되도록 h/2만큼 위로 올려서 붙임
    AlphaBlend(hdc, x1, y1 - h / 2, w, h, memDC, 0, 0, w, h, bf);

    SelectObject(memDC, oBmp);
    DeleteObject(hBmp);
    DeleteDC(memDC);
}
