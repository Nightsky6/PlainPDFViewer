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
#include <shlobj.h>
#include <string>
#include <cstdio>
#include "SignaturePad.h"
#include "PopupAnim.h"

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

        std::vector<std::vector<StrokePt>> savedStrokes; // 지난번에 적용한 서명 (있으면 "이전 서명 사용" 버튼)
        bool prevHover = false;

        // 펜 (서명 전체에 적용)
        int penColorIdx = 0;   // 0 = 검정, 1 = 파랑
        int penWidthIdx = 1;   // 0 = 가늘게, 1 = 보통, 2 = 굵게
        COLORREF savedColor = RGB(20, 20, 20);   // 지난 서명의 펜
        float    savedWidth = 2.4f;

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

    // ── 펜 색·굵기 ─────────────────────────────────────────────
    const COLORREF kPenColors[2] = { RGB(20, 20, 20), RGB(22, 64, 170) };   // 검정, 파랑(계약서 서명용)
    const float    kPenWidths[3] = { 1.6f, 2.4f, 3.6f };
    COLORREF PenColor() { return kPenColors[g_ss.penColorIdx]; }
    float    PenWidth() { return kPenWidths[g_ss.penWidthIdx]; }
    Gdiplus::Color PenGColor() { const COLORREF c = PenColor(); return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

    // 제목 줄 오른쪽 도구: [● ●]  [· • ●]  [↶]
    RECT ColorRect(int i) { const int x = 312 + i * 28; return { x, 16, x + 26, 42 }; }
    RECT WidthRect(int i) { const int x = 384 + i * 28; return { x, 16, x + 26, 42 }; }
    RECT UndoRect()       { return { PAD_W - 20 - 28, 16, PAD_W - 20, 42 }; }

    // ── 획 다듬기: 너무 가까운 점은 버리고 Catmull-Rom 곡선으로 점 사이를 메움 ──
    //    (마우스 점을 직선으로만 이으면 각져 보이므로)
    std::vector<StrokePt> SmoothStroke(const std::vector<StrokePt>& in)
    {
        std::vector<StrokePt> p;
        for (const auto& q : in)
        {
            if (!p.empty())
            {
                const float dx = q.x - p.back().x, dy = q.y - p.back().y;
                if (dx * dx + dy * dy < 2.25f) continue;   // 1.5px 이내 점은 생략
            }
            p.push_back(q);
        }
        if (!in.empty() && (p.empty() || p.back().x != in.back().x || p.back().y != in.back().y))
            p.push_back(in.back());                        // 끝점은 유지
        if (p.size() < 3) return p;

        std::vector<StrokePt> out;
        const int SUB = 4;
        for (size_t i = 0; i + 1 < p.size(); ++i)
        {
            const StrokePt& p0 = p[i == 0 ? 0 : i - 1];
            const StrokePt& p1 = p[i];
            const StrokePt& p2 = p[i + 1];
            const StrokePt& p3 = p[i + 2 < p.size() ? i + 2 : i + 1];
            for (int s = 0; s < SUB; ++s)
            {
                const float t1 = (float)s / SUB, t2 = t1 * t1, t3 = t2 * t1;
                auto cr = [&](float a, float b, float c, float d) {
                    return 0.5f * ((2 * b) + (-a + c) * t1 + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3);
                };
                out.push_back({ cr(p0.x, p1.x, p2.x, p3.x), cr(p0.y, p1.y, p2.y, p3.y) });
            }
        }
        out.push_back(p.back());
        return out;
    }

    // ── 지난 서명 저장/불러오기 (%APPDATA%\PlainPDFViewer\signature.dat, 캔버스 좌표) ──
    std::wstring SavedSigPath()
    {
        wchar_t appData[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
        std::wstring dir = std::wstring(appData) + L"\\PlainPDFViewer";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\signature.dat";
    }

    std::vector<std::vector<StrokePt>> LoadSavedSig()
    {
        std::vector<std::vector<StrokePt>> strokes;
        FILE* f = nullptr;
        if (_wfopen_s(&f, SavedSigPath().c_str(), L"rb") != 0 || !f) return strokes;
        char line[1 << 16];
        while (fgets(line, sizeof(line), f))
        {
            if (line[0] == '#')
            {
                int r = 20, gg = 20, b = 20; float w = 2.4f;
                if (sscanf_s(line, "#pen %d %d %d %f", &r, &gg, &b, &w) == 4)
                { g_ss.savedColor = RGB(r, gg, b); g_ss.savedWidth = w; }
                continue;
            }
            // 한 줄 = 획 하나: "x,y x,y ..."
            std::vector<StrokePt> s;
            const char* c = line;
            float x, y; int n;
            while (sscanf_s(c, " %f,%f%n", &x, &y, &n) == 2)
            {
                if (x >= 0 && y >= 0 && x <= CANVAS_W && y <= CANVAS_H) s.push_back({ x, y });
                c += n;
            }
            if (!s.empty()) strokes.push_back(std::move(s));
        }
        fclose(f);
        return strokes;
    }

    void SaveSig(const std::vector<std::vector<StrokePt>>& strokes)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, SavedSigPath().c_str(), L"wb") != 0 || !f) return;
        const COLORREF c = PenColor();
        fprintf(f, "#pen %d %d %d %.1f\n", GetRValue(c), GetGValue(c), GetBValue(c), PenWidth());
        for (const auto& s : strokes)
        {
            for (const auto& q : s) fprintf(f, "%.1f,%.1f ", q.x, q.y);
            fputc('\n', f);
        }
        fclose(f);
    }

    // "이전 서명 사용" 버튼: 빈 캔버스 오른쪽 위 (저장된 서명이 있을 때만)
    bool ShowPrevBtn() { return !g_ss.savedStrokes.empty() && g_ss.strokes.empty() && g_ss.cur.empty() && !g_ss.isImage; }
    RECT PrevBtnRect()
    {
        const int w = 150, h = 28;
        return { CANVAS_X + CANVAS_W - 10 - w, CANVAS_Y + 10, CANVAS_X + CANVAS_W - 10, CANVAS_Y + 10 + h };
    }

    // ── 이미지 서명의 흰 배경 → 투명 ──────────────────────────────
    // 이미 투명한 부분이 있는 이미지(PNG 등)는 그대로 둔다 (nullptr 반환).
    // 밝기 235 이상은 완전 투명, 180 이하는 불투명, 그 사이는 부드럽게 (글씨 가장자리 보존)
    Gdiplus::Bitmap* MakeWhiteTransparent(Gdiplus::Bitmap* src)
    {
        const int w = (int)src->GetWidth(), h = (int)src->GetHeight();
        if (w <= 0 || h <= 0) return nullptr;
        auto* dst = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(dst);
            g.Clear(Gdiplus::Color(0, 0, 0, 0));
            g.DrawImage(src, 0, 0, w, h);
        }
        Gdiplus::BitmapData bd;
        Gdiplus::Rect rc(0, 0, w, h);
        if (dst->LockBits(&rc, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        { delete dst; return nullptr; }
        bool hasAlpha = false;
        for (int y = 0; y < h && !hasAlpha; ++y)
        {
            const BYTE* row = (const BYTE*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
            for (int x = 0; x < w; ++x) if (row[x * 4 + 3] < 250) { hasAlpha = true; break; }
        }
        if (!hasAlpha)
        {
            for (int y = 0; y < h; ++y)
            {
                BYTE* row = (BYTE*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
                for (int x = 0; x < w; ++x)
                {
                    BYTE* px = row + x * 4;   // B G R A
                    const int lum = (px[0] * 114 + px[1] * 587 + px[2] * 299) / 1000;
                    int a = lum >= 235 ? 0 : lum <= 180 ? 255 : (235 - lum) * 255 / 55;
                    px[3] = (BYTE)a;
                }
            }
        }
        dst->UnlockBits(&bd);
        if (hasAlpha) { delete dst; return nullptr; }
        return dst;
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

        Gdiplus::Pen pen(PenGColor(), PenWidth());
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);

        for (auto& s : g_ss.strokes)
        {
            if (s.size() == 1)
            {
                Gdiplus::SolidBrush dot(PenGColor());
                const float r = PenWidth() / 2.f;
                g_ss.cg->FillEllipse(&dot, s[0].x - r, s[0].y - r, r * 2.f, r * 2.f);
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

        // 펜 도구: 색(검정·파랑), 굵기(3단계), 되돌리기 — 이미지 서명일 때는 숨김
        if (!g_ss.isImage)
        {
            auto selBg = [&](const RECT& r) {
                RectF rf((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top));
                auto* sp = RRP(rf, 6.f);
                SolidBrush sb(Color(255, 229, 238, 251));
                g.FillPath(&sb, sp);
                Pen spn(Color(255, 0, 120, 212), 1.f);
                g.DrawPath(&spn, sp);
                delete sp;
            };
            for (int i = 0; i < 2; ++i)
            {
                const RECT r = ColorRect(i);
                if (i == g_ss.penColorIdx) selBg(r);
                const COLORREF c = kPenColors[i];
                SolidBrush cb(Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
                g.FillEllipse(&cb, (float)r.left + 6.f, (float)r.top + 6.f, 14.f, 14.f);
            }
            for (int i = 0; i < 3; ++i)
            {
                const RECT r = WidthRect(i);
                if (i == g_ss.penWidthIdx) selBg(r);
                const float d = 3.f + i * 3.5f;
                SolidBrush wb(Color(255, 60, 60, 66));
                g.FillEllipse(&wb, (r.left + r.right) / 2.f - d / 2.f, (r.top + r.bottom) / 2.f - d / 2.f, d, d);
            }
            {
                const RECT r = UndoRect();
                FontFamily mdl(L"Segoe MDL2 Assets");
                FontFamily flu(L"Segoe Fluent Icons");
                Font uf(mdl.IsAvailable() ? &mdl : &flu, 13.f, FontStyleRegular, UnitPixel);
                SolidBrush ub(g_ss.strokes.empty() ? Color(255, 190, 190, 196) : Color(255, 60, 60, 66));
                StringFormat usf;
                usf.SetAlignment(StringAlignmentCenter);
                usf.SetLineAlignment(StringAlignmentCenter);
                g.DrawString(L"\xE7A7", 1, &uf, RectF((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top)), &usf, &ub);   // Undo
            }
        }

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
        if (ShowPrevBtn())
        {
            const RECT pb = PrevBtnRect();
            RectF prf((float)pb.left, (float)pb.top, (float)(pb.right - pb.left), (float)(pb.bottom - pb.top));
            auto* pp = RRP(prf, 14.f);
            SolidBrush pbBg(g_ss.prevHover ? Color(255, 222, 234, 252) : Color(255, 236, 242, 252));
            g.FillPath(&pbBg, pp);
            Pen pbPen(Color(255, 0, 120, 212), 1.f);
            g.DrawPath(&pbPen, pp);
            delete pp;
            Font pbFont(pFF, 9, FontStyleRegular, UnitPoint);
            SolidBrush pbTxt(Color(255, 0, 84, 166));
            StringFormat psf;
            psf.SetAlignment(StringAlignmentCenter);
            psf.SetLineAlignment(StringAlignmentCenter);
            psf.SetFormatFlags(StringFormatFlagsNoWrap);
            psf.SetTrimming(StringTrimmingEllipsisCharacter);
            g.DrawString(LS(STR_SIG_USE_PREV), -1, &pbFont, prf, &psf, &pbTxt);
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
        PopupAnim::Hide(hWnd);
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
                    Gdiplus::Pen pen(PenGColor(), PenWidth());
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
            {
                const RECT pb = PrevBtnRect();
                const bool ov = ShowPrevBtn() && PtInRect(&pb, pt);
                if (ov != g_ss.prevHover) { g_ss.prevHover = ov; InvalidateRect(hWnd, &pb, FALSE); }
            }

            // 커서
            RECT cvRc = { CANVAS_X, CANVAS_Y, CANVAS_X+CANVAS_W, CANVAS_Y+CANVAS_H };
            SetCursor(g_ss.prevHover ? LoadCursor(nullptr, IDC_HAND) : (PtInRect(&cvRc, pt) ? curPen : curArrow));

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
            // 펜 도구
            if (!g_ss.isImage)
            {
                for (int i = 0; i < 2; ++i)
                {
                    const RECT r = ColorRect(i);
                    if (PtInRect(&r, pt)) { g_ss.penColorIdx = i; RedrawAllStrokes(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }
                }
                for (int i = 0; i < 3; ++i)
                {
                    const RECT r = WidthRect(i);
                    if (PtInRect(&r, pt)) { g_ss.penWidthIdx = i; RedrawAllStrokes(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }
                }
                const RECT ur = UndoRect();
                if (PtInRect(&ur, pt))
                {
                    if (!g_ss.strokes.empty()) { g_ss.strokes.pop_back(); RedrawAllStrokes(); InvalidateRect(hWnd, nullptr, FALSE); }
                    return 0;
                }
            }
            {
                const RECT pb = PrevBtnRect();
                if (ShowPrevBtn() && PtInRect(&pb, pt))
                {
                    g_ss.strokes = g_ss.savedStrokes;   // 지난 서명을 캔버스에 불러옴 (그때의 펜 색·굵기도)
                    for (int i = 0; i < 2; ++i) if (kPenColors[i] == g_ss.savedColor) g_ss.penColorIdx = i;
                    for (int i = 0; i < 3; ++i) if (kPenWidths[i] == g_ss.savedWidth) g_ss.penWidthIdx = i;
                    g_ss.prevHover = false;
                    RedrawAllStrokes();
                    InvalidateRect(hWnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (PtInRect(&cvRc, pt)) {
                g_ss.drawing = true;
                SetCapture(hWnd);
                g_ss.cur.clear();
                int cx = pt.x - CANVAS_X, cy = pt.y - CANVAS_Y;
                g_ss.cur.push_back({ (float)cx, (float)cy });
                // 점 하나도 그려두기
                if (g_ss.cg) {
                    Gdiplus::SolidBrush dot(PenGColor());
                    const float r = PenWidth() / 2.f;
                    g_ss.cg->FillEllipse(&dot, (float)cx - r, (float)cy - r, r * 2.f, r * 2.f);
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
                {
                    g_ss.strokes.push_back(SmoothStroke(g_ss.cur));
                    RedrawAllStrokes();   // 다듬은 곡선으로 다시 그림
                    RECT cvRc2 = { CANVAS_X, CANVAS_Y, CANVAS_X + CANVAS_W, CANVAS_Y + CANVAS_H };
                    InvalidateRect(hWnd, &cvRc2, FALSE);
                }
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
                        // 종이에 쓴 서명을 찍은 사진처럼 흰 배경이 있으면 투명하게 (문서 글자를 가리지 않도록)
                        if (Gdiplus::Bitmap* clear = MakeWhiteTransparent(g_ss.imgBitmap))
                        { delete g_ss.imgBitmap; g_ss.imgBitmap = clear; }
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
            if (wParam == VK_RETURN && (!g_ss.strokes.empty() || (g_ss.isImage && g_ss.imgBitmap))) { CloseSig(hWnd, true); return 0; }
            if (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) && !g_ss.isImage && !g_ss.strokes.empty())
            {
                g_ss.strokes.pop_back();   // 마지막 획 되돌리기
                RedrawAllStrokes();
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
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
    g_ss.savedStrokes = LoadSavedSig();
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
    EnableWindow(parent, FALSE);
    // 아래에서 살짝 떠오르며 나타남 (PopupAnim.h)
    PopupAnim::Show(hWnd, SW_SHOWNOACTIVATE);
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

        Gdiplus::Pen pen(PenGColor(), PenWidth());
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        cg.TranslateTransform(-minX, -minY);

        for (auto& s : g_ss.strokes) {
            if (s.size() == 1) {
                Gdiplus::SolidBrush dot(PenGColor());
                const float r = PenWidth() / 2.f;
                cg.FillEllipse(&dot, s[0].x - r, s[0].y - r, r * 2.f, r * 2.f);
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
        res.cropX = minX;
        res.cropY = minY;
        res.penColor = PenColor();
        res.penWidth = PenWidth();
        SaveSig(g_ss.strokes);   // 다음에 "이전 서명 사용"으로 불러올 수 있게
        }
    }

    delete g_ss.canvas; g_ss.canvas = nullptr;
    delete g_ss.imgBitmap; g_ss.imgBitmap = nullptr;
    return res;
}
