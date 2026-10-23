// ---------------------------------------------------------------
// CleanPreferences.cpp - Fluent 스타일 환경 설정 창 구현
// ---------------------------------------------------------------
#include "framework.h"
#include <objbase.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <string>
#include "CleanPreferences.h"
#include "Lang.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

#undef min
#undef max

// ---------------------------------------------------------------
// 내부 상수
// ---------------------------------------------------------------
namespace
{
    // 창 크기
    constexpr int  DLG_W      = 440;
    constexpr int  DLG_H      = 650;

    // 색상
    constexpr COLORREF C_BG        = RGB(247, 248, 250); // 전체 배경
    constexpr COLORREF C_CARD      = RGB(255, 255, 255); // 카드 배경
    constexpr COLORREF C_BORDER    = RGB(218, 220, 226); // 카드 테두리
    constexpr COLORREF C_SEC_FG    = RGB( 60, 120, 210); // 섹션 헤더 글자
    constexpr COLORREF C_LABEL     = RGB( 45,  48,  55); // 일반 레이블
    constexpr COLORREF C_HINT      = RGB(140, 143, 150); // 힌트 텍스트
    constexpr COLORREF C_BTN_PRI   = RGB(  0, 120, 212); // 확인 버튼
    constexpr COLORREF C_BTN_PRI_H = RGB(  0, 100, 180); // 확인 버튼 hover
    constexpr COLORREF C_BTN_SEC   = RGB(245, 246, 248); // 취소 버튼
    constexpr COLORREF C_BTN_SEC_H = RGB(230, 232, 237); // 취소 버튼 hover
    constexpr COLORREF C_BTN_BDR   = RGB(200, 202, 208); // 취소 버튼 테두리

    // 레이아웃
    constexpr int MARGIN      = 26;
    constexpr int CARD_PAD    = 22;  // 카드 내부 상하 여백
    constexpr int CARD_GAP    = 32;  // 카드 사이 간격 (섹션 헤더 공간 포함)
    constexpr int SEC_H       = 22;  // 섹션 헤더 텍스트 높이 (카드 위 여백)
    constexpr int ROW_H       = 46;
    constexpr int BTN_W       = 100;
    constexpr int BTN_H       = 36;
    constexpr int SLIDER_W    = 160;
    constexpr int SWATCH_R    = 10; // 색상 원 반지름
    constexpr int COLOR_BTN_W = 140;
    constexpr int COLOR_BTN_H = 34;
    constexpr int STEP_BTN    = 30;
    constexpr int STEP_VAL    = 40;
    constexpr int STEP_H      = 32;
    constexpr int TOGGLE_W    = 40;
    constexpr int TOGGLE_H    = 22;
    constexpr UINT_PTR TOGGLE_TIMER = 1;
    constexpr double   TOGGLE_MS    = 160.0;

    const wchar_t* PREF_CLASS = L"CleanPreferencesClass";
    bool g_classRegistered    = false;

    // ── 컨트롤 ID ──────────────────────────────────────────────
    enum CtrlId : int {
        ID_SLIDER     = 3001,
        ID_EDIT       = 3002,
        ID_SPIN       = 3003,
        ID_OK         = 3004,
        ID_CANCEL     = 3005,

    };

    // ── 버튼 상태 ──────────────────────────────────────────────
    struct Btn {
        RECT rc{};
        bool hovered = false;
        bool pressed = false;
    };

    // ── 창 상태 ────────────────────────────────────────────────
    struct PrefState {
        AppPreferences  work;           // 편집 중 복사본
        COLORREF        origColor;      // 원본 (취소 복원용)
        COLORREF        customColors[16] = {};

        // GDI 컨트롤 핸들
        int   sliderVal  = 3;            // 1~10
        int   recentVal  = 5;            // 1~10
        RECT  sliderRc{};                // 슬라이더 트랙 영역
        RECT  minusRc{};                 // 스테퍼 - 버튼
        RECT  plusRc{};                  // 스테퍼 + 버튼
        RECT  stepValRc{};               // 스테퍼 값 영역
        RECT  toggleRc{};                // 주석 도구 토글 스위치
        bool  dragSlider = false;
        bool  minusHot   = false;
        bool  plusHot    = false;
        bool  sliderHot  = false;
        bool  toggleHot  = false;
        float toggleAnim = 0.0f;         // 토글 표시 위치 (0 = 꺼짐, 1 = 켜짐)
        float toggleFrom = 0.0f;         // 애니메이션 시작 위치
        ULONGLONG toggleStart = 0;       // 애니메이션 시작 시각


        // 커스텀 버튼
        Btn   colorBtn;
        Btn   okBtn;
        Btn   cancelBtn;

        // 창 닫힘 / 결과
        bool  closed = false;
        bool  result = false;

        // 브러시 캐시
        HBRUSH hBgBrush   = nullptr;
        HBRUSH hCardBrush = nullptr;
        HFONT  hLabelFont = nullptr;
        HFONT  hSecFont   = nullptr;
        HFONT  hSmallFont = nullptr;

        // 슬라이더 값 문자열 (렌더에서 사용)
        wchar_t speedLabel[16] = L"x1.0";
        RECT    speedLabelRc   = {};     // WM_HSCROLL 무효화용
    } g_ps;

    // ── 전방 선언 ──────────────────────────────────────────────
    LRESULT CALLBACK PrefProc(HWND, UINT, WPARAM, LPARAM);
    void    DrawPref(HWND, HDC);
    void    ClosePref(HWND, bool ok);
    void    UpdateSpeedLabel();
    void    LayoutControls(HWND);

    // 트랙바 서브클래스: WM_PAINT 후 포커스 점선 제거
    int SliderValFromX(int x)
    {
        const int pad = 8;
        int w = (g_ps.sliderRc.right - g_ps.sliderRc.left) - pad * 2;
        float t = (float)(x - g_ps.sliderRc.left - pad) / (float)w;
        t = std::max(0.0f, std::min(1.0f, t));
        return 1 + (int)roundf(t * 9.0f);
    }

    // ── GDI+ 헬퍼 ─────────────────────────────────────────────
    static Gdiplus::GraphicsPath* RoundRectPath(const Gdiplus::RectF& r, float rad)
    {
        using namespace Gdiplus;
        auto* p = new GraphicsPath();
        float d = rad * 2.0f;
        p->AddArc(r.X,             r.Y,              d, d, 180, 90);
        p->AddArc(r.X+r.Width-d,  r.Y,              d, d, 270, 90);
        p->AddArc(r.X+r.Width-d,  r.Y+r.Height-d,  d, d,   0, 90);
        p->AddArc(r.X,             r.Y+r.Height-d,  d, d,  90, 90);
        p->CloseFigure();
        return p;
    }

    static Gdiplus::Color GdipColor(COLORREF c, BYTE a = 255) {
        return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
    }

    // ── 슬라이더 값 → 속도 문자열 ─────────────────────────────
    void UpdateSpeedLabel()
    {
        int val = g_ps.sliderVal;
        g_ps.work.scrollSpeed = val * 0.3f;
        swprintf_s(g_ps.speedLabel, L"x%.1f", g_ps.work.scrollSpeed);
    }

    // ── 컨트롤 배치 ───────────────────────────────────────────
    void LayoutControls(HWND hWnd)
    {
        // 카드 영역 계산
        // 카드 1 (표시): y=56, 높이 ROW_H+CARD_PAD*2
        // 카드 2 (스크롤): y=카드1_bottom+12
        // 카드 3 (파일):  y=카드2_bottom+12

        int cardX  = MARGIN;
        int cardW  = DLG_W - MARGIN*2;
        int cardH  = CARD_PAD + ROW_H + CARD_PAD;  // 카드 공통 높이
        (void)cardH; // DrawPref와 공유

        // ── 카드 1: 표시 ──
        int c2y = 76;
        int cbx = cardX + cardW - CARD_PAD - COLOR_BTN_W;
        int cby = c2y + CARD_PAD + (ROW_H - COLOR_BTN_H) / 2;
        g_ps.colorBtn.rc = { cbx, cby, cbx + COLOR_BTN_W, cby + COLOR_BTN_H };

        // ── 카드 3: 자동 스크롤 ──
        int c3y = c2y + cardH + CARD_GAP;
        constexpr int VAL_LABEL_W = 52;
        int sliderX = cardX + cardW - CARD_PAD - VAL_LABEL_W - SLIDER_W;
        int sliderY = c3y + CARD_PAD + (ROW_H - 22) / 2;
        g_ps.sliderRc = { sliderX, sliderY, sliderX + SLIDER_W, sliderY + 22 };
        // 숫자 레이블 영역 저장 (WM_HSCROLL 무효화용)
        {
            int valX = cardX + cardW - CARD_PAD - 44;
            int valY = c3y + CARD_PAD + (ROW_H - 16) / 2;
            g_ps.speedLabelRc = { valX - 2, valY - 2, valX + 46, valY + 22 };
        }

        // ── 카드 4: 파일 ──
        int c4y = c3y + cardH + CARD_GAP;
        int stepX = cardX + cardW - CARD_PAD - (STEP_BTN + STEP_VAL + STEP_BTN);
        int stepY = c4y + CARD_PAD + (ROW_H - STEP_H) / 2;
        g_ps.minusRc   = { stepX, stepY, stepX + STEP_BTN, stepY + STEP_H };
        g_ps.stepValRc = { stepX + STEP_BTN, stepY, stepX + STEP_BTN + STEP_VAL, stepY + STEP_H };
        g_ps.plusRc    = { stepX + STEP_BTN + STEP_VAL, stepY, stepX + STEP_BTN * 2 + STEP_VAL, stepY + STEP_H };

        // ── 카드 5: 주석 ──
        int c5y = c4y + cardH + CARD_GAP;
        int tglX = cardX + cardW - CARD_PAD - TOGGLE_W;
        int tglY = c5y + CARD_PAD + (ROW_H - TOGGLE_H) / 2;
        g_ps.toggleRc = { tglX, tglY, tglX + TOGGLE_W, tglY + TOGGLE_H };

        // ── 확인 / 취소 버튼 ──
        int btnY  = DLG_H - MARGIN - BTN_H;
        int okX   = DLG_W - MARGIN - BTN_W;
        int canX  = okX - BTN_W - 10;
        g_ps.okBtn.rc     = { okX,  btnY, okX  + BTN_W, btnY + BTN_H };
        g_ps.cancelBtn.rc = { canX, btnY, canX + BTN_W, btnY + BTN_H };
    }

    // ── 렌더링 ────────────────────────────────────────────────
    void DrawPref(HWND hWnd, HDC hdc)
    {
        using namespace Gdiplus;

        RECT cr; GetClientRect(hWnd, &cr);
        int W = cr.right, H = cr.bottom;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        FontFamily ff(L"Malgun Gothic");
        if (!ff.IsAvailable()) { /* fallback to generic sans */ }
        const FontFamily* pFF = ff.IsAvailable() ? &ff : FontFamily::GenericSansSerif();

        // ── 배경 ──
        SolidBrush bgBr(GdipColor(C_BG));
        g.FillRectangle(&bgBr, 0, 0, W, H);

        // ── 타이틀 ──
        Font titleFont(pFF, 13, FontStyleBold, UnitPoint);
        SolidBrush titleBr(GdipColor(C_LABEL));
        {
            RectF rt((float)MARGIN, 16.0f, (float)(W - MARGIN*2), 28.0f);
            g.DrawString(LS(STR_PREF_TITLE), -1, &titleFont, rt, nullptr, &titleBr);
        }

        // ── 공통 카드 그리기 람다 ──
        int cardX = MARGIN, cardW = W - MARGIN*2;
        auto DrawCard = [&](int cy, int ch) {
            RectF rc((float)cardX + 0.5f, (float)cy + 0.5f,
                     (float)cardW - 1.0f, (float)ch - 1.0f);
            SolidBrush cardBr(GdipColor(C_CARD));
            GraphicsPath* path = RoundRectPath(rc, 8.0f);
            g.FillPath(&cardBr, path);
            Pen borderPen(GdipColor(C_BORDER), 1.0f);
            g.DrawPath(&borderPen, path);
            delete path;
        };

        // ── 섹션 헤더 텍스트 ──
        Font secFont(pFF, 8, FontStyleBold, UnitPoint);
        Font labelFont(pFF, 9, FontStyleRegular, UnitPoint);
        Font hintFont(pFF, 8, FontStyleRegular, UnitPoint);
        SolidBrush secBr(GdipColor(C_SEC_FG));
        SolidBrush labelBr(GdipColor(C_LABEL));
        SolidBrush hintBr(GdipColor(C_HINT));

        // ── 카드 1: 표시 ──
        int cardH = CARD_PAD + ROW_H + CARD_PAD;
        int c2y = 76, c2h = cardH;

        int secX   = cardX + CARD_PAD;
        DrawCard(c2y, c2h);

        int labelY = c2y + CARD_PAD + (ROW_H - 18) / 2;

        {
            RectF rs((float)secX, (float)(c2y - SEC_H), 80.0f, (float)SEC_H);
            StringFormat sfSec; sfSec.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(LS(STR_PREF_SEC_DISPLAY), -1, &secFont, rs, &sfSec, &secBr);
            RectF rl((float)secX, (float)labelY, 160.0f, 20.0f);
            g.DrawString(LS(STR_PREF_SEL_COLOR), -1, &labelFont, rl, nullptr, &labelBr);
        }

        // 색상 버튼 렌더링
        const Btn& cb = g_ps.colorBtn;
        {
            RectF bcRc((float)cb.rc.left+0.5f, (float)cb.rc.top+0.5f,
                       (float)(cb.rc.right-cb.rc.left)-1.0f,
                       (float)(cb.rc.bottom-cb.rc.top)-1.0f);
            SolidBrush cbBg(cb.hovered ? GdipColor(C_BTN_SEC_H) : GdipColor(C_CARD));
            GraphicsPath* p = RoundRectPath(bcRc, 5.0f);
            g.FillPath(&cbBg, p);
            Pen bdrPen(GdipColor(C_BTN_BDR), 1.0f);
            g.DrawPath(&bdrPen, p);
            delete p;

            Font cbFont(pFF, 8.5f, FontStyleRegular, UnitPoint);
            SolidBrush cbTxtBr(GdipColor(C_LABEL));
            float btnCY = (float)(cb.rc.top + cb.rc.bottom) * 0.5f;
            float btnW  = (float)(cb.rc.right - cb.rc.left);
            constexpr float CONT_W = 20.0f + 6.0f + 70.0f;
            float startX = (float)cb.rc.left + (btnW - CONT_W) * 0.5f;
            float circX  = startX + (float)SWATCH_R;
            SolidBrush swBr2(GdipColor(g_ps.work.selectionColor));
            Pen swPen2(GdipColor(C_BTN_BDR), 1.0f);
            g.FillEllipse(&swBr2, circX - (float)SWATCH_R, btnCY - (float)SWATCH_R, (float)SWATCH_R*2, (float)SWATCH_R*2);
            g.DrawEllipse(&swPen2, circX - (float)SWATCH_R, btnCY - (float)SWATCH_R, (float)SWATCH_R*2, (float)SWATCH_R*2);
            RectF txtRc(startX + 20.0f + 6.0f, (float)cb.rc.top, 72.0f, (float)(cb.rc.bottom - cb.rc.top));
            StringFormat sf;
            sf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(LS(STR_PREF_COLOR_PICK), -1, &cbFont, txtRc, &sf, &cbTxtBr);
        }

        // ── 카드 3: 자동 스크롤 ──
        int c3y = c2y + c2h + CARD_GAP, c3h = cardH;
        DrawCard(c3y, c3h);

        {
            RectF r1((float)secX, (float)(c3y - SEC_H), 140.0f, (float)SEC_H);
            StringFormat sfSec2; sfSec2.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(LS(STR_PREF_SEC_SCROLL), -1, &secFont, r1, nullptr, &secBr);
            RectF r2((float)secX, (float)(c3y + CARD_PAD + (ROW_H-18)/2), 120.0f, 20.0f);
            g.DrawString(LS(STR_PREF_SCROLL_SPEED), -1, &labelFont, r2, nullptr, &labelBr);
        }

        {
            // 모던 슬라이더
            float sx0 = (float)g_ps.sliderRc.left + 8.0f;
            float sx1 = (float)g_ps.sliderRc.right - 8.0f;
            float sy  = (float)(g_ps.sliderRc.top + g_ps.sliderRc.bottom) * 0.5f;
            float tx  = sx0 + (sx1 - sx0) * (float)(g_ps.sliderVal - 1) / 9.0f;
            Pen trackBg(GdipColor(RGB(222, 225, 231)), 4.0f);
            trackBg.SetStartCap(LineCapRound); trackBg.SetEndCap(LineCapRound);
            g.DrawLine(&trackBg, sx0, sy, sx1, sy);
            Pen trackFg(GdipColor(C_BTN_PRI), 4.0f);
            trackFg.SetStartCap(LineCapRound); trackFg.SetEndCap(LineCapRound);
            g.DrawLine(&trackFg, sx0, sy, tx, sy);
            float tr = (g_ps.dragSlider || g_ps.sliderHot) ? 9.0f : 8.0f;
            SolidBrush shadow(Gdiplus::Color(40, 0, 0, 0));
            g.FillEllipse(&shadow, tx - tr, sy - tr + 1.5f, tr * 2, tr * 2);
            SolidBrush thumbBr(GdipColor(RGB(255, 255, 255)));
            g.FillEllipse(&thumbBr, tx - tr, sy - tr, tr * 2, tr * 2);
            Pen thumbPen(GdipColor(C_BTN_PRI), 2.0f);
            g.DrawEllipse(&thumbPen, tx - tr + 1, sy - tr + 1, tr * 2 - 2, tr * 2 - 2);

            int valY = c3y + CARD_PAD + (ROW_H - 16) / 2;
            Font valFont(pFF, 9, FontStyleBold, UnitPoint);
            SolidBrush valBr(GdipColor(C_SEC_FG));
            int valX = cardX + cardW - CARD_PAD - 44;
            RectF valRc((float)valX, (float)valY, 44.0f, 18.0f);
            StringFormat vSf; vSf.SetAlignment(StringAlignmentFar);
            g.DrawString(g_ps.speedLabel, -1, &valFont, valRc, &vSf, &valBr);
        }

        // ── 카드 4: 파일 ──
        int c4y = c3y + c3h + CARD_GAP, c4h = cardH;
        DrawCard(c4y, c4h);

        {
            RectF r1((float)secX, (float)(c4y - SEC_H), 80.0f, (float)SEC_H);
            StringFormat sfSec3; sfSec3.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(LS(STR_PREF_SEC_FILE), -1, &secFont, r1, &sfSec3, &secBr);
            RectF r2((float)secX, (float)(c4y + CARD_PAD + (ROW_H-18)/2), 160.0f, 20.0f);
            g.DrawString(LS(STR_PREF_RECENT), -1, &labelFont, r2, nullptr, &labelBr);
            int hintX2 = g_ps.minusRc.left;
            RectF r3((float)(hintX2 - 62), (float)(c4y + CARD_PAD + (ROW_H-14)/2 + 1), 56.0f, 14.0f);
            g.DrawString(L"(1 ~ 10)", -1, &hintFont, r3, nullptr, &hintBr);

            // 모던 스테퍼 [ - | 값 | + ]
            RectF whole((float)g_ps.minusRc.left + 0.5f, (float)g_ps.minusRc.top + 0.5f,
                        (float)(g_ps.plusRc.right - g_ps.minusRc.left) - 1.0f,
                        (float)(g_ps.minusRc.bottom - g_ps.minusRc.top) - 1.0f);
            GraphicsPath* sp = RoundRectPath(whole, 6.0f);
            SolidBrush stBg(GdipColor(C_BTN_SEC));
            g.FillPath(&stBg, sp);
            Pen stPen(GdipColor(C_BTN_BDR), 1.0f);
            g.DrawPath(&stPen, sp);
            delete sp;
            auto HotFill = [&](const RECT& r, bool hot) {
                if (!hot) return;
                SolidBrush hb(GdipColor(C_BTN_SEC_H));
                Region old; g.GetClip(&old);
                GraphicsPath* cp = RoundRectPath(whole, 6.0f);
                g.SetClip(cp);
                g.FillRectangle(&hb, (INT)r.left, (INT)r.top, (INT)(r.right - r.left), (INT)(r.bottom - r.top));
                g.SetClip(&old);
                delete cp;
            };
            HotFill(g_ps.minusRc, g_ps.minusHot);
            HotFill(g_ps.plusRc,  g_ps.plusHot);
            g.DrawLine(&stPen, (float)g_ps.minusRc.right, (float)g_ps.minusRc.top + 4, (float)g_ps.minusRc.right, (float)g_ps.minusRc.bottom - 4);
            g.DrawLine(&stPen, (float)g_ps.plusRc.left,   (float)g_ps.plusRc.top + 4,  (float)g_ps.plusRc.left,   (float)g_ps.plusRc.bottom - 4);

            Font stFont(pFF, 11, FontStyleRegular, UnitPoint);
            Font stValFont(pFF, 9.5f, FontStyleBold, UnitPoint);
            StringFormat cf; cf.SetAlignment(StringAlignmentCenter); cf.SetLineAlignment(StringAlignmentCenter);
            bool canMinus = g_ps.recentVal > 1, canPlus = g_ps.recentVal < 10;
            SolidBrush onBr(GdipColor(C_LABEL)), offBr(GdipColor(RGB(190, 192, 198)));
            g.DrawString(L"\u2212", -1, &stFont, RectF((float)g_ps.minusRc.left, (float)g_ps.minusRc.top, (float)STEP_BTN, (float)STEP_H), &cf, canMinus ? &onBr : &offBr);
            g.DrawString(L"+", -1, &stFont, RectF((float)g_ps.plusRc.left, (float)g_ps.plusRc.top, (float)STEP_BTN, (float)STEP_H), &cf, canPlus ? &onBr : &offBr);
            wchar_t rv[8]; swprintf_s(rv, L"%d", g_ps.recentVal);
            g.DrawString(rv, -1, &stValFont, RectF((float)g_ps.stepValRc.left, (float)g_ps.stepValRc.top, (float)STEP_VAL, (float)STEP_H), &cf, &labelBr);
        }

        // ── 카드 5: 주석 ──
        int c5y = c4y + c4h + CARD_GAP, c5h = cardH;
        DrawCard(c5y, c5h);

        {
            RectF r1((float)secX, (float)(c5y - SEC_H), 120.0f, (float)SEC_H);
            StringFormat sfSec4; sfSec4.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(LS(STR_PREF_SEC_ANNOT), -1, &secFont, r1, &sfSec4, &secBr);

            // 레이블이 긴 언어는 두 줄로 줄바꿈되도록 행 높이 전체를 사용
            float lblW = (float)(g_ps.toggleRc.left - 12 - secX);
            RectF r2((float)secX, (float)(c5y + CARD_PAD), lblW, (float)ROW_H);
            StringFormat sfLbl; sfLbl.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(LS(STR_PREF_SEL_TOOLBAR), -1, &labelFont, r2, &sfLbl, &labelBr);

            // 토글 스위치
            const float p = g_ps.toggleAnim;   // 0 = 꺼짐, 1 = 켜짐 (애니메이션 중간값 포함)
            const RECT& tr = g_ps.toggleRc;
            RectF tRc((float)tr.left + 0.5f, (float)tr.top + 0.5f,
                      (float)(tr.right - tr.left) - 1.0f, (float)(tr.bottom - tr.top) - 1.0f);
            GraphicsPath* tp = RoundRectPath(tRc, tRc.Height * 0.5f);
            COLORREF trackOn  = g_ps.toggleHot ? C_BTN_PRI_H : C_BTN_PRI;
            COLORREF trackOff = g_ps.toggleHot ? RGB(178, 181, 188) : RGB(196, 199, 205);
            auto lerpC = [p](int a, int b) { return (BYTE)(a + (b - a) * p + 0.5f); };
            SolidBrush trackBr(Gdiplus::Color(255,
                lerpC(GetRValue(trackOff), GetRValue(trackOn)),
                lerpC(GetGValue(trackOff), GetGValue(trackOn)),
                lerpC(GetBValue(trackOff), GetBValue(trackOn))));
            g.FillPath(&trackBr, tp);
            delete tp;
            const float knobD = tRc.Height - 6.0f;
            const float knobOffX = tRc.X + 3.0f;
            const float knobOnX  = tRc.X + tRc.Width - 3.0f - knobD;
            const float knobX = knobOffX + (knobOnX - knobOffX) * p;
            SolidBrush knobShadow(Gdiplus::Color(40, 0, 0, 0));
            g.FillEllipse(&knobShadow, knobX, tRc.Y + 3.0f + 1.0f, knobD, knobD);
            SolidBrush knobBr(GdipColor(RGB(255, 255, 255)));
            g.FillEllipse(&knobBr, knobX, tRc.Y + 3.0f, knobD, knobD);
        }

        // ── 구분선 ──
        Pen sepPen(GdipColor(C_BORDER), 1.0f);
        g.DrawLine(&sepPen,
            (float)MARGIN, (float)(DLG_H - MARGIN - BTN_H - 14),
            (float)(W - MARGIN), (float)(DLG_H - MARGIN - BTN_H - 14));

        // ── 버튼 ──
        auto DrawBtn = [&](const Btn& btn, bool isPrimary, const wchar_t* label) {
            RectF bRc((float)btn.rc.left+0.5f, (float)btn.rc.top+0.5f,
                      (float)(btn.rc.right-btn.rc.left)-1.0f,
                      (float)(btn.rc.bottom-btn.rc.top)-1.0f);
            GraphicsPath* bp = RoundRectPath(bRc, 5.0f);

            if (isPrimary) {
                COLORREF top = btn.hovered ? C_BTN_PRI_H : C_BTN_PRI;
                LinearGradientBrush grad(
                    PointF(bRc.X, bRc.Y),
                    PointF(bRc.X, bRc.Y + bRc.Height),
                    GdipColor(top),
                    GdipColor(btn.hovered ? RGB(0,86,160) : RGB(0,100,190)));
                g.FillPath(&grad, bp);
                Pen hlPen(Gdiplus::Color(50,255,255,255), 1.0f);
                g.DrawLine(&hlPen, bRc.X+5, bRc.Y+1.5f, bRc.X+bRc.Width-5, bRc.Y+1.5f);
            } else {
                SolidBrush secBgBr(GdipColor(btn.hovered ? C_BTN_SEC_H : C_BTN_SEC));
                g.FillPath(&secBgBr, bp);
                Pen bdrP(GdipColor(C_BTN_BDR), 1.0f);
                g.DrawPath(&bdrP, bp);
            }
            delete bp;

            Font btnFont(pFF, 9, FontStyleBold, UnitPoint);
            SolidBrush btnTxt(isPrimary ? GdipColor(RGB(255,255,255)) : GdipColor(C_LABEL));
            StringFormat bsf;
            bsf.SetAlignment(StringAlignmentCenter);
            bsf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(label, -1, &btnFont, bRc, &bsf, &btnTxt);
        };

        DrawBtn(g_ps.cancelBtn, false, LS(STR_PREF_BTN_CANCEL));
        DrawBtn(g_ps.okBtn,     true,  LS(STR_PREF_BTN_OK));
    }

    // ── 닫기 (페이드아웃) ─────────────────────────────────────
    void ClosePref(HWND hWnd, bool ok)
    {
        g_ps.result = ok;
        g_ps.closed = true;
        HWND parent = GetParent(hWnd);

        // 페이드아웃
        for (int a = 220; a >= 0; a -= 44) {
            SetLayeredWindowAttributes(hWnd, 0, (BYTE)a, LWA_ALPHA);
            UpdateWindow(hWnd);
            Sleep(6);
        }

        // 순서 중요: 부모 활성화 먼저 → 창 숨김 → 소멸
        // EnableWindow 복원 후 곧바로 SetForegroundWindow 하면
        // 포커스가 부드럽게 넘어가 메인 창 깜빡임이 사라짐
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
        ShowWindow(hWnd, SW_HIDE);
        DestroyWindow(hWnd);
    }

    // ── WndProc ───────────────────────────────────────────────
    LRESULT CALLBACK PrefProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        static HCURSOR curArrow = nullptr, curHand = nullptr;

        switch (msg)
        {
        case WM_CREATE:
            curArrow = LoadCursor(nullptr, IDC_ARROW);
            curHand  = LoadCursor(nullptr, IDC_HAND);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_TIMER:
            if (wParam == TOGGLE_TIMER)
            {
                const double t = std::min(1.0, (double)(GetTickCount64() - g_ps.toggleStart) / TOGGLE_MS);
                const double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);   // ease-out cubic
                const float target = g_ps.work.showSelToolbar ? 1.0f : 0.0f;
                g_ps.toggleAnim = g_ps.toggleFrom + (target - g_ps.toggleFrom) * (float)e;
                if (t >= 1.0) KillTimer(hWnd, TOGGLE_TIMER);
                InvalidateRect(hWnd, &g_ps.toggleRc, FALSE);
                return 0;
            }
            break;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            HDC mdc = CreateCompatibleDC(hdc);
            HBITMAP mbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP obm = (HBITMAP)SelectObject(mdc, mbm);
            DrawPref(hWnd, mdc);
            BitBlt(hdc,
                ps.rcPaint.left, ps.rcPaint.top,
                ps.rcPaint.right  - ps.rcPaint.left,
                ps.rcPaint.bottom - ps.rcPaint.top,
                mdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(mdc, obm); DeleteObject(mbm); DeleteDC(mdc);
            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        {
            // 슬라이더/에디트 배경을 카드 흰색에 맞춤
            HDC hCtlDC = (HDC)wParam;
            SetBkColor(hCtlDC, C_CARD);
            SetTextColor(hCtlDC, C_LABEL);
            if (!g_ps.hCardBrush) g_ps.hCardBrush = CreateSolidBrush(C_CARD);
            return (LRESULT)g_ps.hCardBrush;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case ID_OK:
            {
                g_ps.work.maxRecentFiles = g_ps.recentVal;
                UpdateSpeedLabel();
                ClosePref(hWnd, true);
                return 0;
            }
            case ID_CANCEL:
                ClosePref(hWnd, false);
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool repaint = false;

            auto HoverBtn = [&](Btn& b) {
                bool over = PtInRect(&b.rc, pt);
                if (over != b.hovered) { b.hovered = over; repaint = true;
                    SetCursor(over ? curHand : curArrow); }
            };
            HoverBtn(g_ps.colorBtn);
            HoverBtn(g_ps.okBtn);
            HoverBtn(g_ps.cancelBtn);

            {
                bool mh = PtInRect(&g_ps.minusRc, pt) != FALSE;
                bool ph = PtInRect(&g_ps.plusRc,  pt) != FALSE;
                RECT sr = g_ps.sliderRc; InflateRect(&sr, 0, 4);
                bool sh = PtInRect(&sr, pt) != FALSE;
                bool th = PtInRect(&g_ps.toggleRc, pt) != FALSE;
                if (mh != g_ps.minusHot || ph != g_ps.plusHot || sh != g_ps.sliderHot || th != g_ps.toggleHot) {
                    g_ps.minusHot = mh; g_ps.plusHot = ph; g_ps.sliderHot = sh; g_ps.toggleHot = th;
                    SetCursor((mh || ph || sh || th) ? curHand : curArrow);
                    repaint = true;
                }
                if (g_ps.dragSlider) {
                    int nv = SliderValFromX(pt.x);
                    if (nv != g_ps.sliderVal) { g_ps.sliderVal = nv; UpdateSpeedLabel(); repaint = true; }
                }
            }

            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            if (repaint) {
                InvalidateRect(hWnd, &g_ps.sliderRc, FALSE);
                InvalidateRect(hWnd, &g_ps.speedLabelRc, FALSE);
                InvalidateRect(hWnd, &g_ps.minusRc, FALSE);
                InvalidateRect(hWnd, &g_ps.plusRc, FALSE);
                InvalidateRect(hWnd, &g_ps.stepValRc, FALSE);
                InvalidateRect(hWnd, &g_ps.toggleRc,  FALSE);
                InvalidateRect(hWnd, &g_ps.colorBtn.rc,  FALSE);
                InvalidateRect(hWnd, &g_ps.okBtn.rc,     FALSE);
                InvalidateRect(hWnd, &g_ps.cancelBtn.rc, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            g_ps.colorBtn.hovered = g_ps.okBtn.hovered = g_ps.cancelBtn.hovered = false;
            g_ps.minusHot = g_ps.plusHot = g_ps.sliderHot = g_ps.toggleHot = false;
            InvalidateRect(hWnd, &g_ps.sliderRc, FALSE);
            InvalidateRect(hWnd, &g_ps.toggleRc, FALSE);
            InvalidateRect(hWnd, &g_ps.minusRc,  FALSE);
            InvalidateRect(hWnd, &g_ps.plusRc,   FALSE);
            InvalidateRect(hWnd, &g_ps.colorBtn.rc,  FALSE);
            InvalidateRect(hWnd, &g_ps.okBtn.rc,     FALSE);
            InvalidateRect(hWnd, &g_ps.cancelBtn.rc, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PtInRect(&g_ps.colorBtn.rc,  pt)) g_ps.colorBtn.pressed  = true;
            if (PtInRect(&g_ps.okBtn.rc,     pt)) g_ps.okBtn.pressed     = true;
            if (PtInRect(&g_ps.cancelBtn.rc, pt)) g_ps.cancelBtn.pressed = true;
            {
                RECT sr = g_ps.sliderRc; InflateRect(&sr, 0, 6);
                if (PtInRect(&sr, pt)) {
                    g_ps.dragSlider = true;
                    SetCapture(hWnd);
                    g_ps.sliderVal = SliderValFromX(pt.x);
                    UpdateSpeedLabel();
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
                if (PtInRect(&g_ps.minusRc, pt) && g_ps.recentVal > 1)  { --g_ps.recentVal; InvalidateRect(hWnd, nullptr, FALSE); }
                if (PtInRect(&g_ps.plusRc,  pt) && g_ps.recentVal < 10) { ++g_ps.recentVal; InvalidateRect(hWnd, nullptr, FALSE); }
                if (PtInRect(&g_ps.toggleRc, pt)) {
                    // 값은 즉시 바꾸고, 노브·색상은 현재 위치에서 새 위치로 부드럽게 이동
                    g_ps.work.showSelToolbar = !g_ps.work.showSelToolbar;
                    g_ps.toggleFrom  = g_ps.toggleAnim;
                    g_ps.toggleStart = GetTickCount64();
                    SetTimer(hWnd, TOGGLE_TIMER, 10, nullptr);
                }
            }
            return 0;
        }

        case WM_LBUTTONUP:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            if (g_ps.dragSlider) {
                g_ps.dragSlider = false;
                ReleaseCapture();
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            if (g_ps.colorBtn.pressed && PtInRect(&g_ps.colorBtn.rc, pt)) {
                // 공통 다이얼로그를
                static const LANGID langMap[] = {
                    MAKELANGID(LANG_KOREAN,             SUBLANG_DEFAULT),
                    MAKELANGID(LANG_ENGLISH,            SUBLANG_DEFAULT),
                    MAKELANGID(LANG_JAPANESE,           SUBLANG_DEFAULT),
                    MAKELANGID(LANG_CHINESE_SIMPLIFIED, SUBLANG_DEFAULT),
                    MAKELANGID(LANG_SPANISH,            SUBLANG_DEFAULT),
                    MAKELANGID(LANG_FRENCH,             SUBLANG_DEFAULT),
                    MAKELANGID(LANG_GERMAN,             SUBLANG_DEFAULT),
                    MAKELANGID(LANG_PORTUGUESE,         SUBLANG_PORTUGUESE_BRAZILIAN),
                    MAKELANGID(LANG_RUSSIAN,            SUBLANG_DEFAULT),
                };
                int li = (int)g_appLang;
                if (li < 0 || li >= (int)_countof(langMap)) li = 0;
                LANGID prevLang = SetThreadUILanguage(langMap[li]);

                CHOOSECOLORW cc = {};
                cc.lStructSize  = sizeof(cc);
                cc.hwndOwner    = hWnd;
                cc.rgbResult    = g_ps.work.selectionColor;
                cc.lpCustColors = g_ps.customColors;
                cc.Flags        = CC_FULLOPEN | CC_RGBINIT;
                if (ChooseColorW(&cc)) {
                    g_ps.work.selectionColor = cc.rgbResult;
                    InvalidateRect(hWnd, &g_ps.colorBtn.rc, FALSE);
                }

                SetThreadUILanguage(prevLang);
            }
            if (g_ps.okBtn.pressed && PtInRect(&g_ps.okBtn.rc, pt)) {
                g_ps.work.maxRecentFiles = g_ps.recentVal;
                UpdateSpeedLabel();
                ClosePref(hWnd, true);
            }
            if (g_ps.cancelBtn.pressed && PtInRect(&g_ps.cancelBtn.rc, pt)) {
                ClosePref(hWnd, false);
            }

            g_ps.colorBtn.pressed = g_ps.okBtn.pressed = g_ps.cancelBtn.pressed = false;
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { ClosePref(hWnd, false); return 0; }
            if (wParam == VK_LEFT)  { g_ps.sliderVal = std::max(1, g_ps.sliderVal - 1);  UpdateSpeedLabel(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }
            if (wParam == VK_RIGHT) { g_ps.sliderVal = std::min(10, g_ps.sliderVal + 1); UpdateSpeedLabel(); InvalidateRect(hWnd, nullptr, FALSE); return 0; }
            if (wParam == VK_RETURN) {
                g_ps.work.maxRecentFiles = g_ps.recentVal;
                UpdateSpeedLabel();
                ClosePref(hWnd, true);
                return 0;
            }
            break;

        case WM_NCHITTEST:
        {
            LRESULT hit = DefWindowProc(hWnd, msg, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hWnd, &pt);
                // 버튼/컨트롤 위가 아닌 영역은 드래그 이동 허용
                if (!PtInRect(&g_ps.colorBtn.rc,  pt) &&
                    !PtInRect(&g_ps.okBtn.rc,      pt) &&
                    !PtInRect(&g_ps.cancelBtn.rc,  pt) &&
                    !PtInRect(&g_ps.sliderRc, pt) &&
                    !PtInRect(&g_ps.minusRc,  pt) &&
                    !PtInRect(&g_ps.plusRc,   pt) &&
                    !PtInRect(&g_ps.toggleRc, pt))
                    return HTCAPTION;
            }
            return hit;
        }

        case WM_DESTROY:
            KillTimer(hWnd, TOGGLE_TIMER);
            if (g_ps.hBgBrush)  { DeleteObject(g_ps.hBgBrush);   g_ps.hBgBrush   = nullptr; }
            if (g_ps.hCardBrush) { DeleteObject(g_ps.hCardBrush); g_ps.hCardBrush = nullptr; }
            if (g_ps.hLabelFont) { DeleteObject(g_ps.hLabelFont); g_ps.hLabelFont = nullptr; }
            if (g_ps.hSecFont)   { DeleteObject(g_ps.hSecFont);   g_ps.hSecFont   = nullptr; }
            if (g_ps.hSmallFont) { DeleteObject(g_ps.hSmallFont); g_ps.hSmallFont = nullptr; }
            return 0;

        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

} // anonymous namespace

// ---------------------------------------------------------------
// 공개 API
// ---------------------------------------------------------------
bool CleanPreferences::Show(HWND parent, AppPreferences& prefs)
{
    // GDI+ 초기화 확인 (CleanPopup::Init 이후 이미 됐을 수 있음)
    static ULONG_PTR token = 0;
    if (!token) {
        Gdiplus::GdiplusStartupInput si;
        Gdiplus::GdiplusStartup(&token, &si, nullptr);
    }

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(parent, GWLP_HINSTANCE);

    // 윈도우 클래스 등록 (최초 1회)
    if (!g_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = PrefProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = PREF_CLASS;
        if (!RegisterClassExW(&wc)) return false;
        g_classRegistered = true;
    }

    // 상태 초기화
    g_ps = {};
    g_ps.work      = prefs;
    g_ps.origColor = prefs.selectionColor;
    g_ps.toggleAnim = prefs.showSelToolbar ? 1.0f : 0.0f;
    swprintf_s(g_ps.speedLabel, L"x%.1f", prefs.scrollSpeed);

    // 부모 중앙 위치
    RECT pr; GetWindowRect(parent, &pr);
    int x = pr.left + (pr.right  - pr.left - DLG_W) / 2;
    int y = pr.top  + (pr.bottom - pr.top  - DLG_H) / 2;

    HWND hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        PREF_CLASS, nullptr, WS_POPUP,
        x, y, DLG_W, DLG_H,
        parent, nullptr, hInst, nullptr
    );
    if (!hWnd) return false;

    // DWM 둥근 모서리 + 그림자
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    MARGINS margins = { 1,1,1,1 };
    DwmExtendFrameIntoClientArea(hWnd, &margins);

    // 커스텀 컨트롤 초기값
    g_ps.sliderVal = std::max(1, std::min(10, (int)roundf(prefs.scrollSpeed / 0.3f)));
    g_ps.recentVal = std::max(1, std::min(10, prefs.maxRecentFiles));


    // 컨트롤 배치
    LayoutControls(hWnd);

    // 페이드인: 부모 비활성화 → 창 표시 → 포커스
    // AnimateWindow(AW_BLEND) 대신 수동 페이드로 레이어드 창과 충돌 방지
    SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
    EnableWindow(parent, FALSE);
    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    for (int a = 0; a <= 255; a += 51) {
        SetLayeredWindowAttributes(hWnd, 0, (BYTE)a, LWA_ALPHA);
        UpdateWindow(hWnd);
        Sleep(6);
    }
    SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

    // 모달 메시지 루프
    MSG msg2;
    while (!g_ps.closed && GetMessage(&msg2, nullptr, 0, 0))
    {
        if (msg2.hwnd == parent || IsChild(parent, msg2.hwnd))
        {
            if ((msg2.message >= WM_MOUSEFIRST && msg2.message <= WM_MOUSELAST) ||
                (msg2.message >= WM_KEYFIRST   && msg2.message <= WM_KEYLAST)   ||
                msg2.message == WM_NCLBUTTONDOWN)
            {
                if (msg2.message == WM_LBUTTONDOWN || msg2.message == WM_NCLBUTTONDOWN)
                    FlashWindow(hWnd, TRUE);
                continue;
            }
        }
        if (!IsDialogMessageW(hWnd, &msg2)) {
            TranslateMessage(&msg2);
            DispatchMessage(&msg2);
        }
    }

    if (g_ps.result)
        prefs = g_ps.work;

    return g_ps.result;
}
