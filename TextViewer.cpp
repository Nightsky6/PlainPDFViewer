// TextViewer.cpp : PDF 텍스트 추출 및 뷰어 창
//

#include "framework.h"
#include "Lang.h"
#include <objbase.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include "TextViewer.h"
#include "resource.h"
#include <fpdf_text.h>
#include <commctrl.h>
#include <windowsx.h>
#include <vector>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

// ── 컨트롤 ID / 레이아웃 상수 ─────────────────────────────────────────────────
#define TEXTVIEW_TOOLBAR_H  52   // 툴바 높이
#define TEXTVIEW_FOOTER_H   52   // 하단 버튼바 높이
#define IDC_TV_EDIT         2001
#define IDC_TV_COPY         2002
#define IDC_TV_PAGE_FROM    2003
#define IDC_TV_PAGE_TO      2004
#define IDC_TV_APPLY        2005
#define IDC_TV_CLOSE        2006
#define WM_TV_LOAD_TEXT     (WM_USER + 300)  // 창 생성 후 텍스트 로딩 지시

// ── 뷰어 창 내부 데이터 ────────────────────────────────────────────────────────
struct TextViewerData {
    HWND hEdit;
    HWND hPageFrom;   // 시작 페이지 입력
    HWND hPageTo;     // 끝 페이지 입력
    HWND hParent;
    HFONT hFont;
    HFONT hUIFont;
    int pageIndex;
    int pageCount;
    bool textLoaded;  // 텍스트 로딩 완료 여부
    bool closed;      // 모달 루프 종료 신호
    // 버튼 상태
    struct TvBtn { RECT rc; bool hovered; bool pressed; };
    TvBtn btnApply;   // 툴바 내 적용 버튼
    TvBtn btnCopy;
    TvBtn btnClose;
    // 툴바 레이아웃 캐시 (TvLayout에서 계산, TvPaint에서 사용)
    int tbFromX = 0, tbToX = 0, tbEditY = 0, tbEditW = 44;
};

// ── GDI+ 헬퍼: 둥근 사각형 경로 ──────────────────────────────────────────────
static Gdiplus::GraphicsPath* TvRoundRect(float x, float y, float w, float h, float r)
{
    using namespace Gdiplus;
    auto* p = new GraphicsPath();
    float d = r * 2.f;
    p->AddArc(x,         y,         d, d, 180, 90);
    p->AddArc(x + w - d, y,         d, d, 270, 90);
    p->AddArc(x + w - d, y + h - d, d, d,   0, 90);
    p->AddArc(x,         y + h - d, d, d,  90, 90);
    p->CloseFigure();
    return p;
}

// ── 버튼 그리기 ───────────────────────────────────────────────────────────────
enum TvBtnStyle { TVB_SECONDARY, TVB_PRIMARY, TVB_DANGER };

static void TvDrawButton(HDC hdc, const RECT& rc, const wchar_t* text,
                          HFONT hFont, bool hover, bool pressed, TvBtnStyle style)
{
    using namespace Gdiplus;
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    float x = (float)rc.left, y = (float)rc.top;
    float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);

    // 버튼 배경
    Color top, bot;
    if (style == TVB_PRIMARY) {
        top = hover ? Color(255, 14,148,244) : Color(255,  0,137,230);
        bot = hover ? Color(255, 14,148,244) : Color(255,  0,108,196);
    } else if (style == TVB_DANGER) {
        top = hover ? Color(255,220, 60, 60) : Color(255,200, 40, 40);
        bot = hover ? Color(255,220, 60, 60) : Color(255,170, 25, 25);
    } else {
        top = hover ? Color(255,234,234,234) : Color(255,255,255,255);
        bot = hover ? Color(255,234,234,234) : Color(255,242,242,242);
    }

    auto* path = TvRoundRect(x + .5f, y + .5f, w - 1.f, h - 1.f, 5.f);
    LinearGradientBrush gb(PointF(x,y), PointF(x, y+h), top, bot);
    g.FillPath(&gb, path);

    if (style == TVB_SECONDARY) {
        Pen bp(Color(255,200,200,200), 1.f);
        g.DrawPath(&bp, path);
    }
    delete path;

    // 텍스트
    Color txtClr = (style == TVB_SECONDARY)
        ? Color(255, 45, 45, 45) : Color(255, 255, 255, 255);
    SolidBrush tb(txtClr);
    FontFamily ff(L"Segoe UI");
    Font fnt(&ff, 9, FontStyleBold, UnitPoint);
    RectF tr(x, y, w, h);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &fnt, tr, &sf, &tb);
}

// ── 페이지 입력 서브클래스 (Enter 키 → 새로고침) ─────────────────────────────
static LRESULT CALLBACK TvPageEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uId, DWORD_PTR dwRef)
{
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        SendMessageW(GetParent(hWnd), WM_COMMAND,
                     MAKEWPARAM(IDC_TV_APPLY, BN_CLICKED), 0);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hWnd, TvPageEditProc, uId);
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// ── HitTest: 버튼 클릭 확인 ──────────────────────────────────────────────────
static int TvHitBtn(TextViewerData* d, POINT pt)
{
    if (PtInRect(&d->btnApply.rc, pt)) return IDC_TV_APPLY;
    if (PtInRect(&d->btnCopy.rc,  pt)) return IDC_TV_COPY;
    if (PtInRect(&d->btnClose.rc, pt)) return IDC_TV_CLOSE;
    return 0;
}

// ── 레이아웃 계산 ─────────────────────────────────────────────────────────────
static void TvLayout(HWND hWnd, TextViewerData* d)
{
    RECT cr; GetClientRect(hWnd, &cr);
    int W = cr.right;
    int H = cr.bottom;

    // 에디트 영역
    if (d->hEdit)
        MoveWindow(d->hEdit, 12, TEXTVIEW_TOOLBAR_H + 8,
                   W - 24, H - TEXTVIEW_TOOLBAR_H - TEXTVIEW_FOOTER_H - 8, TRUE);

    // 툴바: 오른쪽 영역에 [페이지 라벨] [from] ~ [to] [적용]
    // 적용 버튼은 WM_PAINT에서 그림, 에디트만 MoveWindow
    const int EW = 44, EH = 26, APPLY_W = 52;
    const int SPACING = 6;
    // 오른쪽에서부터 역방향으로 배치
    int applyX = W - 12 - APPLY_W;
    int toX    = applyX - SPACING - EW;
    int fromX  = toX - SPACING - 12 - EW;  // 12 = "~" 너비
    int editY  = (TEXTVIEW_TOOLBAR_H - EH) / 2;
    if (d->hPageFrom) MoveWindow(d->hPageFrom, fromX, editY, EW, EH, TRUE);
    if (d->hPageTo)   MoveWindow(d->hPageTo,   toX,   editY, EW, EH, TRUE);
    d->tbFromX = fromX; d->tbToX = toX; d->tbEditY = editY; d->tbEditW = EW;
    // 적용 버튼 RECT
    int applyY = (TEXTVIEW_TOOLBAR_H - 28) / 2;
    d->btnApply.rc = { applyX, applyY, applyX + APPLY_W, applyY + 28 };

    // 하단 버튼 2개 배치 (복사 / 닫기)
    const int BW = 90, BH = 34, GAP = 12;
    int totalW = BW * 2 + GAP;
    int bx = (W - totalW) / 2;
    int by = H - TEXTVIEW_FOOTER_H + (TEXTVIEW_FOOTER_H - BH) / 2;
    d->btnCopy.rc  = { bx,        by, bx + BW,        by + BH };
    d->btnClose.rc = { bx+BW+GAP, by, bx+BW*2+GAP,   by + BH };
}

// ── 전체 WM_PAINT 그리기 ──────────────────────────────────────────────────────
static void TvPaint(HWND hWnd, TextViewerData* d)
{
    using namespace Gdiplus;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT cr; GetClientRect(hWnd, &cr);
    int W = cr.right, H = cr.bottom;

    // 오프스크린 버퍼
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    Graphics g(mem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // 전체 배경 (흰색)
    SolidBrush bgBrush(Color(255,255,255,255));
    g.FillRectangle(&bgBrush, 0, 0, W, H);

    // 외곽 테두리
    Pen borderPen(Color(255,218,220,224), 1.f);
    g.DrawRectangle(&borderPen, 0.5f, 0.5f, (float)W-1.f, (float)H-1.f);

    // ── 툴바 ──
    SolidBrush tbBrush(Color(255,248,249,250));
    g.FillRectangle(&tbBrush, 0, 0, W, TEXTVIEW_TOOLBAR_H);
    Pen tbPen(Color(255,218,220,224), 1.f);
    g.DrawLine(&tbPen, 0.f, (float)TEXTVIEW_TOOLBAR_H,
               (float)W,  (float)TEXTVIEW_TOOLBAR_H);

    // 툴바 제목
    FontFamily ff(L"Segoe UI");
    Font titleFont(&ff, 10, FontStyleBold, UnitPoint);
    SolidBrush titleBrush(Color(255, 32, 32, 32));
    wchar_t titleBuf[64];
    swprintf_s(titleBuf, L"%s", LS(STR_TV_TITLE));
    RectF titleRc(12.f, 0.f, (float)(W/2 - 60), (float)TEXTVIEW_TOOLBAR_H);
    StringFormat sf; sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(titleBuf, -1, &titleFont, titleRc, &sf, &titleBrush);

    // 페이지 라벨 + "~" (에디트 위치 기반)
    Font lblFont(&ff, 9, FontStyleRegular, UnitPoint);
    SolidBrush lblBrush(Color(255, 80, 85, 95));
    // "페이지" 라벨 - hPageFrom 왼쪽
    if (d->hPageFrom) {
        int fx = d->tbFromX, tx = d->tbToX, ey = d->tbEditY, ew = d->tbEditW;
        RectF lblRc((float)(fx - 56), 0.f, 54.f, (float)TEXTVIEW_TOOLBAR_H);
        sf.SetAlignment(StringAlignmentFar);
        g.DrawString(LS(STR_TV_PAGE), -1, &lblFont, lblRc, &sf, &lblBrush);
        // "~" 사이
        RectF tildeRc((float)(fx + ew), 0.f, (float)(tx - fx - ew), (float)TEXTVIEW_TOOLBAR_H);
        sf.SetAlignment(StringAlignmentCenter);
        g.DrawString(L"~", -1, &lblFont, tildeRc, &sf, &lblBrush);
    }
    // 적용 버튼
    TvDrawButton(mem, d->btnApply.rc, LS(STR_TV_BTN_APPLY), d->hUIFont,
                 d->btnApply.hovered, d->btnApply.pressed, TVB_PRIMARY);

    // ── 에디트 영역 테두리 ──
    int ex = 12, ey = TEXTVIEW_TOOLBAR_H + 8;
    int ew = W - 24, eh = H - TEXTVIEW_TOOLBAR_H - TEXTVIEW_FOOTER_H - 8;
    Pen editBorder(Color(255,210,213,218), 1.f);
    auto* editPath = TvRoundRect((float)ex-1, (float)ey-1, (float)ew+2, (float)eh+2, 4.f);
    g.DrawPath(&editBorder, editPath);
    delete editPath;

    // ── 하단 버튼바 ──
    SolidBrush footerBrush(Color(255,255,255,255));
    g.FillRectangle(&footerBrush, 0, H - TEXTVIEW_FOOTER_H, W, TEXTVIEW_FOOTER_H);
    Pen footerPen(Color(255,218,220,224), 1.f);
    g.DrawLine(&footerPen, 0.f, (float)(H-TEXTVIEW_FOOTER_H),
               (float)W,  (float)(H-TEXTVIEW_FOOTER_H));

    // 버튼 그리기
    TvDrawButton(mem, d->btnCopy.rc,  LS(STR_TV_BTN_COPY), d->hUIFont,
                 d->btnCopy.hovered,  d->btnCopy.pressed,  TVB_SECONDARY);
    TvDrawButton(mem, d->btnClose.rc,  LS(STR_TV_BTN_CLOSE), d->hUIFont,
                 d->btnClose.hovered, d->btnClose.pressed, TVB_PRIMARY);

    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hWnd, &ps);
}

// ── 뷰어 창 프로시저 ──────────────────────────────────────────────────────────
static LRESULT CALLBACK TextViewerProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    TextViewerData* d = (TextViewerData*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_CREATE:
    {
        auto* cs       = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* initText = reinterpret_cast<std::wstring*>(cs->lpCreateParams);

        d = new TextViewerData{};
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)d);

        // DWM 둥근 모서리
        DWMNCRENDERINGPOLICY pol = DWMNCRP_ENABLED;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &pol, sizeof(pol));
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

        d->hFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FF_MODERN, L"Consolas");
        d->hUIFont = CreateFontW(-13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FF_SWISS, L"Segoe UI");

        // 에디트 (WS_EX_TRANSPARENT 없이, 테두리는 WM_PAINT에서 그림)
        d->hEdit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_AUTOHSCROLL | ES_AUTOVSCROLL,
            0, 0, 100, 100, hWnd, (HMENU)IDC_TV_EDIT, hInst, nullptr);
        if (d->hFont) SendMessageW(d->hEdit, WM_SETFONT, (WPARAM)d->hFont, TRUE);
        if (initText) SetWindowTextW(d->hEdit, initText->c_str());

        // 페이지 입력 (시작 / 끝)
        d->hPageFrom = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_CENTER | ES_NUMBER,
            0, 0, 44, 26, hWnd, (HMENU)IDC_TV_PAGE_FROM, hInst, nullptr);
        d->hPageTo = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_CENTER | ES_NUMBER,
            0, 0, 44, 26, hWnd, (HMENU)IDC_TV_PAGE_TO, hInst, nullptr);
        if (d->hUIFont) {
            SendMessageW(d->hPageFrom, WM_SETFONT, (WPARAM)d->hUIFont, TRUE);
            SendMessageW(d->hPageTo,   WM_SETFONT, (WPARAM)d->hUIFont, TRUE);
        }
        SetWindowSubclass(d->hPageFrom, TvPageEditProc, IDC_TV_PAGE_FROM, 0);
        SetWindowSubclass(d->hPageTo,   TvPageEditProc, IDC_TV_PAGE_TO,   0);

        // 마우스 추적
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);

        TvLayout(hWnd, d);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        TvPaint(hWnd, d);
        return 0;
    case WM_SIZE:
        if (!d) break;
        // 로딩 전 최소화되면 즉시 복구
        if (!d->textLoaded && wParam == SIZE_MINIMIZED) {
            ShowWindow(hWnd, SW_RESTORE);
            return 0;
        }
        // 복원 시 부모 다시 비활성화 (최소화 중 부모를 활성화했으므로)
        if (wParam == SIZE_RESTORED && d->hParent && IsWindowEnabled(d->hParent)) {
            EnableWindow(d->hParent, FALSE);
            SetForegroundWindow(hWnd);
        }
        TvLayout(hWnd, d);
        RedrawWindow(hWnd, nullptr, nullptr,
            RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        return 0;
    case WM_MOUSEMOVE:
    {
        if (!d || !d->textLoaded) return 0;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        auto upd = [&](TextViewerData::TvBtn& b) {
            bool h = !!PtInRect(&b.rc, pt);
            if (h != b.hovered) { b.hovered = h; return true; }
            return false;
        };
        bool dirty = false;
        dirty |= upd(d->btnApply);
        dirty |= upd(d->btnCopy);
        dirty |= upd(d->btnClose);
        if (dirty) {
            InvalidateRect(hWnd, &d->btnApply.rc, FALSE);
            InvalidateRect(hWnd, &d->btnCopy.rc,  FALSE);
            InvalidateRect(hWnd, &d->btnClose.rc, FALSE);
            RECT tr = {0, 0, 9999, TEXTVIEW_TOOLBAR_H};
            InvalidateRect(hWnd, &tr, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        if (!d) break;
        d->btnApply.hovered = d->btnCopy.hovered = d->btnClose.hovered = false;
        d->btnApply.pressed = d->btnCopy.pressed = d->btnClose.pressed = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        if (!d || !d->textLoaded) return 0;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = TvHitBtn(d, pt);
        d->btnApply.pressed = (hit == IDC_TV_APPLY);
        d->btnCopy.pressed  = (hit == IDC_TV_COPY);
        d->btnClose.pressed = (hit == IDC_TV_CLOSE);
        if (hit) { SetCapture(hWnd); InvalidateRect(hWnd, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (!d || !d->textLoaded) return 0;
        ReleaseCapture();
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        bool wasApply = d->btnApply.pressed;
        bool wasCopy  = d->btnCopy.pressed;
        bool wasClose = d->btnClose.pressed;
        d->btnApply.pressed = d->btnCopy.pressed = d->btnClose.pressed = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        if (wasApply && PtInRect(&d->btnApply.rc, pt))
            SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_TV_APPLY, BN_CLICKED), 0);
        if (wasCopy  && PtInRect(&d->btnCopy.rc,  pt))
            SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_TV_COPY,  BN_CLICKED), 0);
        if (wasClose && PtInRect(&d->btnClose.rc, pt))
            SendMessageW(hWnd, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_COMMAND:
    {
        if (!d) break;
        int id = LOWORD(wParam);
        if (id == IDC_TV_COPY) {
            int len = GetWindowTextLengthW(d->hEdit);
            if (len > 0) {
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(d->hEdit, buf.data(), len + 1);
                size_t byteSize = (len + 1) * sizeof(wchar_t);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, byteSize);
                if (hMem) {
                    void* pMem = GlobalLock(hMem);
                    if (pMem) {
                        memcpy(pMem, buf.data(), byteSize);
                        GlobalUnlock(hMem);
                        if (OpenClipboard(hWnd)) {
                            EmptyClipboard();
                            SetClipboardData(CF_UNICODETEXT, hMem);
                            CloseClipboard();
                        } else {
                            GlobalFree(hMem);
                        }
                    } else {
                        GlobalFree(hMem);
                    }
                }
            }
        }
        else if (id == IDC_TV_APPLY) {
            wchar_t fromBuf[16] = {}, toBuf[16] = {};
            GetWindowTextW(d->hPageFrom, fromBuf, 16);
            GetWindowTextW(d->hPageTo,   toBuf,   16);
            int startPage = _wtoi(fromBuf) - 1;
            int endPage   = (toBuf[0] ? _wtoi(toBuf) : _wtoi(fromBuf)) - 1;
            auto* pg = new std::pair<int,int>(startPage, endPage);
            PostMessageW(hWnd, WM_TV_LOAD_TEXT, 0, reinterpret_cast<LPARAM>(pg));
        }
        return 0;
    }
    case WM_CTLCOLOREDIT:
    {
        HDC hdcCtrl = (HDC)wParam;
        SetBkColor(hdcCtrl, RGB(255, 255, 255));
        SetTextColor(hdcCtrl, RGB(30, 30, 30));
        static HBRUSH hWhite = CreateSolidBrush(RGB(255, 255, 255));
        return (LRESULT)hWhite;
    }
    case WM_CTLCOLORSTATIC:
    {
        HDC hdcCtrl = (HDC)wParam;
        SetBkColor(hdcCtrl, RGB(248, 249, 250));
        SetTextColor(hdcCtrl, RGB(80, 85, 95));
        static HBRUSH hTbBrush = CreateSolidBrush(RGB(248, 249, 250));
        return (LRESULT)hTbBrush;
    }
    case WM_SETFOCUS:
        if (d && d->hEdit) SetFocus(d->hEdit);
        return 0;
    case WM_CLOSE:
        if (!d || !d->textLoaded) return 0;
        if (d) d->closed = true;
        DestroyWindow(hWnd);
        return 0;
    case WM_NCLBUTTONDOWN:
        // 로딩 전 타이틀바/시스템 버튼 클릭도 차단
        if (!d || !d->textLoaded) return 0;
        break;
    case WM_SYSCOMMAND:
        // 로딩 전 최소화/최대화/닫기 모두 차단
        if (!d || !d->textLoaded) return 0;
        // 최소화 시: 부모를 잠시 활성화해서 메인창이 함께 내려가는 것 방지
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            if (d && d->hParent) EnableWindow(d->hParent, TRUE);
            ShowWindow(hWnd, SW_MINIMIZE);
            return 0;
        }
        break;
    case WM_WINDOWPOSCHANGING:
    {
        // 로딩 전 최소화 시도 차단
        if (!d || !d->textLoaded) {
            auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
            if (wp && (wp->flags & SWP_HIDEWINDOW)) wp->flags &= ~SWP_HIDEWINDOW;
        }
        break;
    }
    case WM_TV_LOAD_TEXT:
    {
        if (!d) break;
        // lParam = new std::pair<wstring,int>* (pageFrom, pageTo)
        auto* pg = reinterpret_cast<std::pair<int,int>*>(lParam);
        if (!pg) break;
        int startPage = pg->first, endPage = pg->second;
        delete pg;

        if (startPage < 0) startPage = 0;
        if (endPage < startPage) endPage = startPage;
        if (endPage   >= d->pageCount) endPage   = d->pageCount - 1;
        if (startPage >= d->pageCount) startPage = d->pageCount - 1;
        d->pageIndex = startPage;

        std::wstring text;
        for (int p = startPage; p <= endPage; p++) {
            if (p > startPage) {
                wchar_t sep[64];
                swprintf_s(sep, LS(STR_TV_SEP_FMT), p + 1);
                text += sep;
            }
            text += ExtractSmartText(g_pdfDoc, p);
        }
        SetWindowTextW(d->hEdit, text.c_str());
        d->textLoaded = true;

        wchar_t pg2[16];
        swprintf_s(pg2, L"%d", startPage + 1); SetWindowTextW(d->hPageFrom, pg2);
        swprintf_s(pg2, L"%d", endPage   + 1); SetWindowTextW(d->hPageTo,   pg2);

        wchar_t title[128];
        if (startPage == endPage)
            swprintf_s(title, LS(STR_TV_WIN_PAGE_FMT), startPage + 1);
        else
            swprintf_s(title, LS(STR_TV_WIN_RANGE_FMT), startPage + 1, endPage + 1);
        SetWindowTextW(hWnd, title);

        return 0;
    }
    case WM_DESTROY:
        if (d) {
            if (d->hFont)   DeleteObject(d->hFont);
            if (d->hUIFont) DeleteObject(d->hUIFont);
            delete d;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── ExtractSmartText
std::wstring ExtractSmartText(FPDF_DOCUMENT doc, int pageIndex)
{
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return L"";
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) { FPDF_ClosePage(page); return L""; }

    const int charCount = FPDFText_CountChars(textPage);
    if (charCount <= 0) {
        FPDFText_ClosePage(textPage); FPDF_ClosePage(page);
        return L"";
    }

    std::vector<unsigned short> buf(charCount + 2, 0);
    FPDFText_GetText(textPage, 0, charCount, buf.data());
    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);

    return std::wstring(reinterpret_cast<wchar_t*>(buf.data()));
}

// ── ShowTextViewer ─────────────────────────────────────────────────────────────
void ShowTextViewer(HWND hParent, int pageNum)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = TextViewerProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"PlainPDFTextViewer";
        wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(IDI_PLAINPDFVIEWER));
        wc.hIconSm       = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_PLAINPDFVIEWER),
                               IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
        RegisterClassExW(&wc);
        registered = true;
    }

    wchar_t title[128];
    swprintf_s(title, LS(STR_TV_WIN_PAGE_FMT), pageNum + 1);

    // 모니터 작업 영역의 55% × 70%
    RECT wa = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int waW = wa.right - wa.left, waH = wa.bottom - wa.top;
    int winW = (int)(waW * 0.55);
    int winH = (int)(waH * 0.70);
    int winX = wa.left + (waW - winW) / 2;
    int winY = wa.top  + (waH - winH) / 2;

    // 창을 숨김 상태로 먼저 생성
    HWND hViewer = CreateWindowExW(WS_EX_APPWINDOW, L"PlainPDFTextViewer", title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        winX, winY, winW, winH,
        nullptr, nullptr, hInst, nullptr);

    if (!hViewer) return;

    TextViewerData* d = (TextViewerData*)GetWindowLongPtrW(hViewer, GWLP_USERDATA);
    if (d) {
        d->pageIndex = pageNum;
        d->pageCount = g_pageCount;
        d->hParent   = hParent;
        wchar_t pg[16];
        swprintf_s(pg, L"%d", pageNum + 1);
        SetWindowTextW(d->hPageFrom, pg);
        SetWindowTextW(d->hPageTo,   pg);
    }

    // 창이 보이기 전에 텍스트를 모두 로딩
    std::wstring text = ExtractSmartText(g_pdfDoc, pageNum);
    SetWindowTextW(GetDlgItem(hViewer, IDC_TV_EDIT), text.c_str());
    if (d) d->textLoaded = true;

    // PageManager와 동일한 모달 구조: 부모 비활성화 후 자체 메시지 루프
    EnableWindow(hParent, FALSE);
    ShowWindow(hViewer, SW_SHOW);
    UpdateWindow(hViewer);

    MSG msg;
    while (d && !d->closed) {
        BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret == 0) { PostQuitMessage((int)msg.wParam); break; }
        if (ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
}