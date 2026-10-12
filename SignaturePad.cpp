// SignaturePad.cpp - 서명 팝업 구현
#include "framework.h"
#include "Lang.h"
#include "Strings.h"
#include <objbase.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <algorithm>
#include "SignaturePad.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

#undef min
#undef max

namespace
{
    // ── 상수 ──────────────────────────────────────────────────
    constexpr int PAD_W      = 520;
    constexpr int PAD_H      = 400;
    constexpr int CANVAS_X   = 20;
    constexpr int CANVAS_Y   = 54;
    constexpr int CANVAS_W   = PAD_W - 40;
    constexpr int CANVAS_H   = PAD_H - 140;
    constexpr int BTN_W      = 100;
    constexpr int BTN_H      = 36;
    constexpr int BTN_GAP    = 10;

    constexpr COLORREF C_BG      = RGB(247, 248, 250);
    constexpr COLORREF C_CANVAS  = RGB(255, 255, 255);
    constexpr COLORREF C_BORDER  = RGB(200, 202, 208);
    constexpr COLORREF C_LABEL   = RGB( 45,  48,  55);
    constexpr COLORREF C_HINT    = RGB(160, 163, 170);
    constexpr COLORREF C_PRI     = RGB(  0, 120, 212);
    constexpr COLORREF C_PRI_H   = RGB(  0, 100, 180);
    constexpr COLORREF C_SEC     = RGB(245, 246, 248);
    constexpr COLORREF C_SEC_H   = RGB(230, 232, 237);
    constexpr COLORREF C_CLR     = RGB(248, 230, 230);
    constexpr COLORREF C_CLR_H   = RGB(235, 200, 200);
    constexpr COLORREF C_CLR_BDR = RGB(210, 180, 180);

    const wchar_t* SIG_CLASS = L"SignaturePadClass";
    bool g_sigClassReg = false;

    // ── 버튼 ──────────────────────────────────────────────────
    struct SBtn { RECT rc; bool hovered = false; bool pressed = false; };

    // ── 필압 포인트 ────────────────────────────────────────────
    struct StrokePt { float x, y; };

    struct SigState
    {
        std::vector<std::vector<StrokePt>> strokes; // 완료된 획들
        std::vector<StrokePt>              cur;      // 현재 그리는 중

        SBtn btnApply, btnCancel, btnClear, btnImage;

        bool drawing  = false;
        bool closed   = false;
        bool ok       = false;
        bool isImage  = false; // 이미지 불러오기 모드

        Gdiplus::Bitmap* canvas    = nullptr; // 오프스크린 캔버스
        Gdiplus::Bitmap* imgBitmap = nullptr; // 불러온 이미지 원본
        Gdiplus::Graphics* cg     = nullptr;

        HBRUSH hBgBrush     = nullptr;
        HBRUSH hCanvasBrush = nullptr;
    } g_ss;

    // ── GDI+ 헬퍼 ─────────────────────────────────────────────
    static Gdiplus::Color GC(COLORREF c, BYTE a = 255)
    {
        return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
    }

    static Gdiplus::GraphicsPath* RRP(const Gdiplus::RectF& r, float rad)
    {
        using namespace Gdiplus;
        auto* p = new GraphicsPath();
        float d = rad * 2.f;
        p->AddArc(r.X,            r.Y,            d, d, 180, 90);
        p->AddArc(r.X+r.Width-d,  r.Y,            d, d, 270, 90);
        p->AddArc(r.X+r.Width-d,  r.Y+r.Height-d, d, d,   0, 90);
        p->AddArc(r.X,            r.Y+r.Height-d,  d, d,  90, 90);
        p->CloseFigure();
        return p;
    }

    // ── 캔버스 초기화 / 재생성 ─────────────────────────────────
    void ResetCanvas()
    {
        delete g_ss.cg;     g_ss.cg     = nullptr;
        delete g_ss.canvas; g_ss.canvas = nullptr;
        g_ss.canvas = new Gdiplus::Bitmap(CANVAS_W, CANVAS_H, PixelFormat32bppARGB);
        g_ss.cg     = Gdiplus::Graphics::FromImage(g_ss.canvas);
        g_ss.cg->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g_ss.cg->Clear(Gdiplus::Color(255, 255, 255, 255));
    }

    // ── 획 전체 다시 그리기 ────────────────────────────────────
    void RedrawAllStrokes()
    {
        if (!g_ss.canvas || !g_ss.cg) return;
        g_ss.cg->Clear(Gdiplus::Color(255, 255, 255, 255));

        Gdiplus::Pen pen(Gdiplus::Color(255, 20, 20, 20), 2.4f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);

        for (auto& s : g_ss.strokes)
        {
            if (s.size() == 1)
            {
                Gdiplus::SolidBrush dot(Gdiplus::Color(255, 20, 20, 20));
                g_ss.cg->FillEllipse(&dot, s[0].x - 1.2f, s[0].y - 1.2f, 2.4f, 2.4f);
                continue;
            }
            for (int i = 1; i < (int)s.size(); i++)
                g_ss.cg->DrawLine(&pen, s[i-1].x, s[i-1].y, s[i].x, s[i].y);
        }
    }

    // ── 버튼 배치 ─────────────────────────────────────────────
    void LayoutSigBtns()
    {
        int by  = PAD_H - 20 - BTN_H;
        int okX = PAD_W - 20 - BTN_W;

        g_ss.btnApply.rc  = { okX, by, okX + BTN_W, by + BTN_H };
        int canX = okX - BTN_GAP - BTN_W;
        g_ss.btnCancel.rc = { canX, by, canX + BTN_W, by + BTN_H };
        int clrX = 20;
        g_ss.btnClear.rc  = { clrX, by, clrX + BTN_W, by + BTN_H };
        // 이미지 불러오기 버튼: 지우기 오른쪽
        int imgX = clrX + BTN_W + BTN_GAP;
        g_ss.btnImage.rc  = { imgX, by, imgX + BTN_W + 20, by + BTN_H };
    }

    // ── 전체 창 그리기 ─────────────────────────────────────────
    void DrawSigPad(HWND hWnd, HDC hdc)
    {
        using namespace Gdiplus;
        RECT cr; GetClientRect(hWnd, &cr);
        int W = cr.right, H = cr.bottom;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // 배경
        SolidBrush bgBr(GC(C_BG));
        g.FillRectangle(&bgBr, 0, 0, W, H);

        // 타이틀
        FontFamily ff(L"Malgun Gothic");
        const FontFamily* pFF = ff.IsAvailable() ? &ff : FontFamily::GenericSansSerif();
        Font titleFont(pFF, 12, FontStyleBold, UnitPoint);
        SolidBrush titleBr(GC(C_LABEL));
        RectF titleRc(20.f, 16.f, (float)(W - 40), 26.f);
        g.DrawString(LS(STR_SIG_TITLE), -1, &titleFont, titleRc, nullptr, &titleBr);

        // 서명 캔버스 테두리
        RectF cvBorder((float)CANVAS_X - 1, (float)CANVAS_Y - 1,
                       (float)CANVAS_W + 2, (float)CANVAS_H + 2);
        SolidBrush cvBg(GC(C_CANVAS));
        auto* bp = RRP(cvBorder, 6.f);
        g.FillPath(&cvBg, bp);
        Pen borderPen(GC(C_BORDER), 1.f);
        g.DrawPath(&borderPen, bp);
        delete bp;

        // 캔버스 내용 복사
        if (g_ss.canvas)
            g.DrawImage(g_ss.canvas, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H);

        // 빈 캔버스 힌트 (이미지 모드이거나 획이 있으면 표시 안 함)
        if (!g_ss.isImage && g_ss.strokes.empty() && g_ss.cur.empty())
        {
            Font hintFont(pFF, 10, FontStyleRegular, UnitPoint);
            SolidBrush hintBr(GC(C_HINT));
            RectF hRc((float)CANVAS_X, (float)(CANVAS_Y + CANVAS_H / 2 - 14),
                      (float)CANVAS_W, 28.f);
            StringFormat hsf;
            hsf.SetAlignment(StringAlignmentCenter);
            hsf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(LS(STR_SIG_HINT), -1, &hintFont, hRc, &hsf, &hintBr);
        }

        // 기준선
        Pen linePen(GC(C_BORDER), 1.f);
        linePen.SetDashStyle(DashStyleDash);
        float lineY = (float)(CANVAS_Y + CANVAS_H - 28);
        g.DrawLine(&linePen, (float)(CANVAS_X + 20), lineY,
                              (float)(CANVAS_X + CANVAS_W - 20), lineY);

        // ── 버튼 그리기 헬퍼 ──
        auto DrawBtn = [&](const SBtn& btn, COLORREF bg, COLORREF bgH, COLORREF bdr,
                           bool hasBdr, const wchar_t* label, bool isPrimary)
        {
            RectF bRc((float)btn.rc.left + .5f, (float)btn.rc.top + .5f,
                      (float)(btn.rc.right  - btn.rc.left) - 1.f,
                      (float)(btn.rc.bottom - btn.rc.top)  - 1.f);
            auto* pp = RRP(bRc, 5.f);

            if (isPrimary) {
                COLORREF top = btn.hovered ? C_PRI_H : C_PRI;
                LinearGradientBrush grad(
                    PointF(bRc.X, bRc.Y), PointF(bRc.X, bRc.Y + bRc.Height),
                    GC(top), GC(btn.hovered ? RGB(0,86,160) : RGB(0,100,190)));
                g.FillPath(&grad, pp);
            } else {
                SolidBrush secBr(GC(btn.hovered ? bgH : bg));
                g.FillPath(&secBr, pp);
                if (hasBdr) { Pen bdrP(GC(bdr), 1.f); g.DrawPath(&bdrP, pp); }
            }
            delete pp;

            Font btnFont(pFF, 9, FontStyleBold, UnitPoint);
            SolidBrush txtBr(isPrimary ? GC(RGB(255,255,255)) : GC(C_LABEL));
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(label, -1, &btnFont, bRc, &sf, &txtBr);
        };

        DrawBtn(g_ss.btnClear,  C_CLR,  C_CLR_H,  C_CLR_BDR, true,  LS(STR_SIG_CLEAR),  false);
        DrawBtn(g_ss.btnImage,  C_SEC,  C_SEC_H,  C_BORDER,  true,  LS(STR_SIG_IMAGE),  false);
        DrawBtn(g_ss.btnCancel, C_SEC,  C_SEC_H,  C_BORDER,   true,  LS(STR_SIG_CANCEL), false);
        DrawBtn(g_ss.btnApply,  C_PRI,  C_PRI_H,  0,          false, LS(STR_SIG_APPLY),  true);
    }

    // ── 닫기 ──────────────────────────────────────────────────
    void CloseSig(HWND hWnd, bool ok)
    {
        g_ss.ok     = ok;
        g_ss.closed = true;
        HWND parent = GetParent(hWnd);
        for (int a = 220; a >= 0; a -= 55) {
            SetLayeredWindowAttributes(hWnd, 0, (BYTE)a, LWA_ALPHA);
            UpdateWindow(hWnd);
            Sleep(5);
        }
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
        SetFocus(parent);
        ShowWindow(hWnd, SW_HIDE);
        DestroyWindow(hWnd);
    }

    // ── WndProc ───────────────────────────────────────────────
    LRESULT CALLBACK SigProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        static HCURSOR curPen   = nullptr;
        static HCURSOR curArrow = nullptr;

        switch (msg)
        {
        case WM_CREATE:
            curArrow = LoadCursor(nullptr, IDC_ARROW);
            curPen   = LoadCursor(nullptr, IDC_CROSS);
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            HDC mdc  = CreateCompatibleDC(hdc);
            HBITMAP bm  = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP old = (HBITMAP)SelectObject(mdc, bm);
            DrawSigPad(hWnd, mdc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, old);
            DeleteObject(bm);
            DeleteDC(mdc);
            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
            POINT pt = { mx, my };

            // 캔버스 내 그리기
            if (g_ss.drawing)
            {
                int cx = mx - CANVAS_X, cy = my - CANVAS_Y;
                cx = std::max(0, std::min(CANVAS_W - 1, cx));
                cy = std::max(0, std::min(CANVAS_H - 1, cy));
                g_ss.cur.push_back({ (float)cx, (float)cy });

                // 캔버스에 마지막 선분 추가
                if (g_ss.cur.size() >= 2 && g_ss.cg)
                {
                    Gdiplus::Pen pen(Gdiplus::Color(255, 20, 20, 20), 2.4f);
                    pen.SetStartCap(Gdiplus::LineCapRound);
                    pen.SetEndCap(Gdiplus::LineCapRound);
                    auto& a = g_ss.cur[g_ss.cur.size()-2];
                    auto& b = g_ss.cur.back();
                    g_ss.cg->DrawLine(&pen, a.x, a.y, b.x, b.y);
                }
                // 캔버스 영역만 갱신
                RECT cvRc = { CANVAS_X, CANVAS_Y, CANVAS_X + CANVAS_W, CANVAS_Y + CANVAS_H };
                InvalidateRect(hWnd, &cvRc, FALSE);
            }

            // 버튼 hover
            bool rep = false;
            auto HoverBtn = [&](SBtn& b) {
                bool ov = PtInRect(&b.rc, pt) != 0;
                if (ov != b.hovered) { b.hovered = ov; rep = true; }
            };
            HoverBtn(g_ss.btnApply);
            HoverBtn(g_ss.btnCancel);
            HoverBtn(g_ss.btnClear);
            HoverBtn(g_ss.btnImage);

            // 커서
            RECT cvRc = { CANVAS_X, CANVAS_Y, CANVAS_X+CANVAS_W, CANVAS_Y+CANVAS_H };
            SetCursor(PtInRect(&cvRc, pt) ? curPen : curArrow);

            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            if (rep) {
                RECT btnArea = { 0, PAD_H - 70, PAD_W, PAD_H };
                InvalidateRect(hWnd, &btnArea, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            g_ss.btnApply.hovered = g_ss.btnCancel.hovered = g_ss.btnClear.hovered = g_ss.btnImage.hovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT cvRc = { CANVAS_X, CANVAS_Y, CANVAS_X+CANVAS_W, CANVAS_Y+CANVAS_H };
            if (PtInRect(&cvRc, pt)) {
                g_ss.drawing = true;
                SetCapture(hWnd);
                g_ss.cur.clear();
                int cx = pt.x - CANVAS_X, cy = pt.y - CANVAS_Y;
                g_ss.cur.push_back({ (float)cx, (float)cy });
                // 점 하나도 그려두기
                if (g_ss.cg) {
                    Gdiplus::SolidBrush dot(Gdiplus::Color(255, 20, 20, 20));
                    g_ss.cg->FillEllipse(&dot, (float)cx-1.2f, (float)cy-1.2f, 2.4f, 2.4f);
                }
            }
            if (PtInRect(&g_ss.btnApply.rc,  pt)) g_ss.btnApply.pressed  = true;
            if (PtInRect(&g_ss.btnCancel.rc, pt)) g_ss.btnCancel.pressed = true;
            if (PtInRect(&g_ss.btnClear.rc,  pt)) g_ss.btnClear.pressed  = true;
            if (PtInRect(&g_ss.btnImage.rc,  pt)) g_ss.btnImage.pressed  = true;
            return 0;
        }

        case WM_LBUTTONUP:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (g_ss.drawing) {
                ReleaseCapture();
                g_ss.drawing = false;
                if (!g_ss.cur.empty())
                    g_ss.strokes.push_back(g_ss.cur);
                g_ss.cur.clear();
            }
            if (g_ss.btnApply.pressed  && PtInRect(&g_ss.btnApply.rc,  pt)) CloseSig(hWnd, true);
            if (g_ss.btnCancel.pressed && PtInRect(&g_ss.btnCancel.rc, pt)) CloseSig(hWnd, false);
            if (g_ss.btnClear.pressed  && PtInRect(&g_ss.btnClear.rc,  pt)) {
                g_ss.strokes.clear();
                g_ss.cur.clear();
                g_ss.isImage = false;
                delete g_ss.imgBitmap; g_ss.imgBitmap = nullptr;
                ResetCanvas();
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            if (g_ss.btnImage.pressed && PtInRect(&g_ss.btnImage.rc, pt)) {
                // 이미지 파일 선택
                WCHAR szFile[MAX_PATH] = {};
                OPENFILENAMEW ofn = {};
                ofn.lStructSize  = sizeof(ofn);
                ofn.hwndOwner    = hWnd;
                ofn.lpstrFilter  = LS(STR_SIG_IMG_FILTER);
                ofn.lpstrFile    = szFile;
                ofn.nMaxFile     = MAX_PATH;
                ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    delete g_ss.imgBitmap;
                    g_ss.imgBitmap = Gdiplus::Bitmap::FromFile(szFile);
                    if (g_ss.imgBitmap && g_ss.imgBitmap->GetLastStatus() == Gdiplus::Ok) {
                        // 캔버스에 이미지를 fit하여 렌더링
                        g_ss.strokes.clear();
                        g_ss.cur.clear();
                        g_ss.isImage = true;
                        ResetCanvas();
                        float srcW = (float)g_ss.imgBitmap->GetWidth();
                        float srcH = (float)g_ss.imgBitmap->GetHeight();
                        float scale = std::min((float)CANVAS_W / srcW, (float)CANVAS_H / srcH);
                        float dstW = srcW * scale, dstH = srcH * scale;
                        float dstX = ((float)CANVAS_W - dstW) / 2.f;
                        float dstY = ((float)CANVAS_H - dstH) / 2.f;
                        g_ss.cg->DrawImage(g_ss.imgBitmap,
                            Gdiplus::RectF(dstX, dstY, dstW, dstH));
                    } else {
                        delete g_ss.imgBitmap; g_ss.imgBitmap = nullptr;
                    }
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
            g_ss.btnApply.pressed = g_ss.btnCancel.pressed = g_ss.btnClear.pressed = g_ss.btnImage.pressed = false;
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { CloseSig(hWnd, false); return 0; }
            if (wParam == VK_RETURN && !g_ss.strokes.empty()) { CloseSig(hWnd, true); return 0; }
            break;

        case WM_SETCURSOR:
            return TRUE; // WM_MOUSEMOVE에서 직접 처리

        case WM_NCHITTEST:
        {
            LRESULT hit = DefWindowProc(hWnd, msg, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hWnd, &pt);
                RECT cvRc = { CANVAS_X, CANVAS_Y, CANVAS_X+CANVAS_W, CANVAS_Y+CANVAS_H };
                if (!PtInRect(&cvRc, pt) &&
                    !PtInRect(&g_ss.btnApply.rc,  pt) &&
                    !PtInRect(&g_ss.btnCancel.rc, pt) &&
                    !PtInRect(&g_ss.btnClear.rc,  pt) &&
                    !PtInRect(&g_ss.btnImage.rc,  pt))
                    return HTCAPTION;
            }
            return hit;
        }

        case WM_DESTROY:
            delete g_ss.cg;         g_ss.cg         = nullptr;
            // imgBitmap은 ShowSignaturePad()의 결과 수집 후 정리
            if (g_ss.hBgBrush)     { DeleteObject(g_ss.hBgBrush);     g_ss.hBgBrush     = nullptr; }
            if (g_ss.hCanvasBrush) { DeleteObject(g_ss.hCanvasBrush); g_ss.hCanvasBrush = nullptr; }
            return 0;

        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

} // anonymous namespace

// ── 공개 API ──────────────────────────────────────────────────
SignatureResult ShowSignaturePad(HWND parent)
{
    // GDI+ 보장
    static ULONG_PTR token = 0;
    if (!token) {
        Gdiplus::GdiplusStartupInput si;
        Gdiplus::GdiplusStartup(&token, &si, nullptr);
    }

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(parent, GWLP_HINSTANCE);

    if (!g_sigClassReg) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = SigProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = SIG_CLASS;
        RegisterClassExW(&wc);
        g_sigClassReg = true;
    }

    // 상태 초기화
    g_ss = {};
    LayoutSigBtns();
    ResetCanvas();

    // 부모 중앙
    RECT pr; GetWindowRect(parent, &pr);
    int x = pr.left + (pr.right  - pr.left - PAD_W) / 2;
    int y = pr.top  + (pr.bottom - pr.top  - PAD_H) / 2;

    HWND hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        SIG_CLASS, nullptr, WS_POPUP,
        x, y, PAD_W, PAD_H,
        parent, nullptr, hInst, nullptr);
    if (!hWnd) { delete g_ss.canvas; return {}; }

    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    // 페이드인
    SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
    EnableWindow(parent, FALSE);
    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    for (int a = 0; a <= 255; a += 51) {
        SetLayeredWindowAttributes(hWnd, 0, (BYTE)a, LWA_ALPHA);
        UpdateWindow(hWnd);
        Sleep(5);
    }
    SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

    // 모달 루프
    MSG msg2;
    while (!g_ss.closed && GetMessage(&msg2, nullptr, 0, 0)) {
        if (msg2.hwnd == parent || IsChild(parent, msg2.hwnd)) {
            if ((msg2.message >= WM_MOUSEFIRST && msg2.message <= WM_MOUSELAST) ||
                (msg2.message >= WM_KEYFIRST   && msg2.message <= WM_KEYLAST)   ||
                msg2.message == WM_NCLBUTTONDOWN) {
                if (msg2.message == WM_LBUTTONDOWN || msg2.message == WM_NCLBUTTONDOWN)
                    FlashWindow(hWnd, TRUE);
                continue;
            }
        }
        TranslateMessage(&msg2);
        DispatchMessage(&msg2);
    }

    // 결과 수집
    SignatureResult res;
    if (g_ss.ok)
    {
        if (g_ss.isImage && g_ss.imgBitmap)
        {
            // 이미지 모드: imgBitmap을 크롭/복사하여 반환
            int iw = (int)g_ss.imgBitmap->GetWidth();
            int ih = (int)g_ss.imgBitmap->GetHeight();
            auto* bmp = new Gdiplus::Bitmap(iw, ih, PixelFormat32bppARGB);
            Gdiplus::Graphics cg2(bmp);
            cg2.DrawImage(g_ss.imgBitmap, 0, 0, iw, ih);
            res.bmp     = bmp;
            res.bmpW    = iw;
            res.bmpH    = ih;
            res.isImage = true;
            res.canvasW = CANVAS_W;
            res.canvasH = CANVAS_H;
            // strokes는 비워둠 (이미지 서명은 ink 어노테이션 없음)
            // imgBitmap 소유권을 res.bmp로 이전했으므로 원본 정리
            delete g_ss.imgBitmap; g_ss.imgBitmap = nullptr;
            }
        else if (!g_ss.strokes.empty())
        {
        // 서명 영역 계산 (획들의 bounding box + 여백)
        float minX = (float)CANVAS_W, minY = (float)CANVAS_H;
        float maxX = 0, maxY = 0;
        for (auto& s : g_ss.strokes)
            for (auto& p : s) {
                minX = std::min(minX, p.x); minY = std::min(minY, p.y);
                maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y);
            }
        constexpr float MARGIN = 10.f;
        minX = std::max(0.f, minX - MARGIN);
        minY = std::max(0.f, minY - MARGIN);
        maxX = std::min((float)CANVAS_W, maxX + MARGIN);
        maxY = std::min((float)CANVAS_H, maxY + MARGIN);

        int bw = std::max(1, (int)(maxX - minX));
        int bh = std::max(1, (int)(maxY - minY));

        // 투명 배경 비트맵에 서명만 크롭
        auto* bmp = new Gdiplus::Bitmap(bw, bh, PixelFormat32bppARGB);
        Gdiplus::Graphics cg(bmp);
        cg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        cg.Clear(Gdiplus::Color(0, 255, 255, 255)); // 투명

        Gdiplus::Pen pen(Gdiplus::Color(255, 20, 20, 20), 2.4f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        cg.TranslateTransform(-minX, -minY);

        for (auto& s : g_ss.strokes) {
            if (s.size() == 1) {
                Gdiplus::SolidBrush dot(Gdiplus::Color(255, 20, 20, 20));
                cg.FillEllipse(&dot, s[0].x - 1.2f, s[0].y - 1.2f, 2.4f, 2.4f);
                continue;
            }
            for (int i = 1; i < (int)s.size(); i++)
                cg.DrawLine(&pen, s[i-1].x, s[i-1].y, s[i].x, s[i].y);
        }

        res.bmp  = bmp;
        res.bmpW = bw;
        res.bmpH = bh;
        // 획 데이터 복사 (캔버스 픽셀 좌표)
        for (auto& s : g_ss.strokes) {
            std::vector<SigStrokePt> stroke;
            for (auto& p : s) stroke.push_back({ p.x, p.y });
            res.strokes.push_back(std::move(stroke));
        }
        res.canvasW = CANVAS_W;
        res.canvasH = CANVAS_H;
        }
    }

    delete g_ss.canvas; g_ss.canvas = nullptr;
    delete g_ss.imgBitmap; g_ss.imgBitmap = nullptr;
    return res;
}
