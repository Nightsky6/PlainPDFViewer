// PageManager.cpp : 페이지 관리 창 구현 (Fluent 스타일)
//

#include "framework.h"
#include "resource.h"
#include "Lang.h"
#include "PageManager.h"
#include "PlainPDFViewer.h"
#include "ToastDlg.h"
#include "CleanPopup.h"
#include <objbase.h>
#include <commdlg.h>
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <vector>
#include <algorithm>
#include <string>
#include <windowsx.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

// ── 외부 심볼 ────────────────────────────────────────────────────────────────
extern std::wstring g_filePath;
extern bool SavePDFToFile(const std::wstring& path);
extern void EmbedHighlightAnnotations();
extern void EmbedNoteAnnotations();
extern void EmbedTextBoxAnnotations();
extern void EmbedSignatureObjects();
extern void ClosePDF();
extern void OpenFileFromPath(HWND hWnd, const std::wstring& path, bool showSpinner);
extern void FitToWidth(HWND hWnd);

// ── PM_FileWrite ─────────────────────────────────────────────────────────────
struct PM_FileWriteContext {
    FPDF_FILEWRITE_ fileWrite;
    FILE*           fp;
};
static int PM_FileWriteBlock(FPDF_FILEWRITE_* self, const void* data, unsigned long size)
{
    auto* ctx = reinterpret_cast<PM_FileWriteContext*>(self);
    return (fwrite(data, 1, size, ctx->fp) == size) ? 1 : 0;
}

// ── 레이아웃 상수 ─────────────────────────────────────────────────────────────
static const int PM_LABEL_H  = 22;    // 페이지 번호 레이블 높이
static const int PM_CELL_PAD = 12;    // 셀 내부 여백
static const int PM_GAP      = 12;    // 셀 사이 간격
static const int PM_MARGIN   = 20;    // 그리드 외곽 여백
static const int PM_BTN_AREA = 68;    // 하단 버튼 영역 높이 (고정)
static const int PM_MIN_COLS = 3;     // 최소 열 수
static const int PM_THUMB_RATIO = 75; // 썸네일 가로:세로 비율 (w/h * 100)

// ── 색상 팔레트 (라이트 테마) ─────────────────────────────────────────────────
namespace Clr {
    static const Gdiplus::Color BG          (255, 245, 246, 248);  // 전체 배경
    static const Gdiplus::Color GRID_BG     (255, 255, 255, 255);  // 스크롤 그리드 배경
    static const Gdiplus::Color BTN_BAR     (255, 255, 255, 255);  // 버튼 바 배경
    static const Gdiplus::Color DIVIDER     (255, 218, 220, 224);  // 구분선
    static const Gdiplus::Color CELL_BG     (255, 255, 255, 255);  // 셀 배경
    static const Gdiplus::Color CELL_BORDER (255, 210, 213, 218);  // 셀 테두리
    static const Gdiplus::Color SEL_RING    (255,  0, 120, 212);   // 선택 테두리 (Fluent Blue)
    static const Gdiplus::Color SEL_FILL    (255, 235, 244, 255);  // 선택 배경
    static const Gdiplus::Color DROP_LINE   (255,  0, 120, 212);   // 드롭 위치 선
    static const Gdiplus::Color LABEL_TEXT  (255,  80,  85,  95);  // 페이지 번호
    static const Gdiplus::Color SEL_LABEL   (255,   0, 100, 185);  // 선택된 페이지 번호
    static const Gdiplus::Color DRAG_BORDER (255,  0, 153,  76);   // 드래그 미리보기 테두리
    // 추가된 파일 페이지 (연한 민트)
    static const Gdiplus::Color EXT_CELL_BG   (255, 230, 248, 242);
    static const Gdiplus::Color EXT_SEL_FILL  (255, 200, 240, 228);
    static const Gdiplus::Color EXT_CELL_BDR  (255, 150, 210, 185);
    static const Gdiplus::Color EXT_SEL_RING  (255,   0, 160, 110);
    static const Gdiplus::Color EXT_LABEL     (255,   0, 110,  75);
    // 파일 추가 버튼 — 보라 그라데이션 (#A770EF→#CA9BF4)
    static const Gdiplus::Color BTN_ADD_TOP   (255, 167, 112, 239);  // #A770EF
    static const Gdiplus::Color BTN_ADD_BOT   (255, 202, 155, 244);  // #CA9BF4
    static const Gdiplus::Color BTN_ADD_HOV   (255, 178, 142, 242);  // #B28EF2
    static const Gdiplus::Color BTN_ADD_PRS   (255, 221,  36, 118);  // #DD2476 pressed
    // 저장 버튼 — 하늘색 (#24ACC8→#32C4F0)
    static const Gdiplus::Color BTN_PRI_TOP (255,  36, 172, 200);  // #24ACC8
    static const Gdiplus::Color BTN_PRI_BOT (255,  50, 196, 240);  // #32C4F0
    static const Gdiplus::Color BTN_PRI_HOV (255,  59, 184, 209);  // #3BB8D1
    static const Gdiplus::Color BTN_PRI_PRS (255, 110, 200, 255);  // #6EC8FF pressed
    // Secondary 버튼 (← → 취소) — 주황 그라데이션 (#FF8C00→#FFB84D)
    static const Gdiplus::Color BTN_SEC_TOP (255, 255, 140,   0);  // #FF8C00
    static const Gdiplus::Color BTN_SEC_MID (255, 255, 165,   0);  // #FFA500
    static const Gdiplus::Color BTN_SEC_BOT (255, 255, 184,  77);  // #FFB84D
    static const Gdiplus::Color BTN_SEC_HOV (255, 255, 165,   0);  // #FFA500 hover
    static const Gdiplus::Color BTN_SEC_PRS (255, 230, 122,   0);  // #E67A00 pressed
    static const Gdiplus::Color BTN_BORDER  (255, 210, 213, 220);
    static const Gdiplus::Color BTN_TXT_PRI (255, 255, 255, 255);
    static const Gdiplus::Color BTN_TXT_SEC (255, 255, 255, 255);  // 주황 배경에 흰 텍스트
    static const Gdiplus::Color BTN_PRESSED_RING (255,  99, 184, 255);  // #63B8FF
    // 이전/다음 버튼 — 라임 그린 (#43A047→#66BB6A)
    static const Gdiplus::Color BTN_NAV_TOP (255,  67, 160,  71);  // #43A047
    static const Gdiplus::Color BTN_NAV_BOT (255, 102, 187, 106);  // #66BB6A
    static const Gdiplus::Color BTN_NAV_HOV (255,  81, 175,  85);  // #51AF55
    static const Gdiplus::Color BTN_NAV_PRS (255,  46, 125,  50);  // #2E7D32
    // 삭제 버튼 — 빨강 그라데이션 (#FF5F5F→#FF9E9E)
    static const Gdiplus::Color BTN_DEL_TOP (255, 255,  95,  95);  // #FF5F5F
    static const Gdiplus::Color BTN_DEL_BOT (255, 255, 158, 158);  // #FF9E9E
    static const Gdiplus::Color BTN_DEL_HOV (255, 255, 127, 127);  // #FF7F7F
    static const Gdiplus::Color BTN_DEL_PRS (255, 217,  54,  54);  // #D93636 pressed
}

// ── 버튼 정의 ─────────────────────────────────────────────────────────────────
enum PmBtnId { PM_BTN_UP = 0, PM_BTN_DOWN, PM_BTN_DEL, PM_BTN_ADD, PM_BTN_SAVE, PM_BTN_CANCEL, PM_BTN_COUNT };

struct PmButton {
    RECT    rc;
    bool    hovered;
    bool    pressed;
    wchar_t label[32];
    int     style;      // 0=secondary, 1=primary, 2=danger, 3=add, 4=nav
    int     iconResId;  // 0 = 텍스트만, 양수면 PNG 리소스 ID
};

// ── PNG 리소스 로더 (Toolbar.cpp와 동일 방식) ──────────────────────────────
static Gdiplus::Bitmap* PM_LoadPngBitmap(int resId)
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

// ── 페이지 항목 (원본 문서 추적) ─────────────────────────────────────────────
struct PageEntry {
    int docIdx;   // 0 = 원본 문서, 1+ = 추가된 파일
    int pageIdx;  // 해당 문서 내 페이지 인덱스
};

// ── 상태 구조체 ───────────────────────────────────────────────────────────────
struct PageMgrState
{
    HWND        hWnd;
    int         pageCount;
    std::vector<PageEntry>     order;       // 페이지 순서 (docIdx, pageIdx)
    std::vector<HBITMAP>       thumbs;
    std::vector<FPDF_DOCUMENT> extraDocs;   // 추가된 PDF 문서 핸들
    std::vector<std::wstring>  extraPaths;  // 추가된 PDF 파일 경로
    std::vector<std::string>   extraPasswords; // 추가 문서 열기 암호 (메모리 전용)
    int         selected;
    int         dragFrom;
    int         pendingDragFrom; // 드래그 대기 중인 셀 인덱스 (-1이면 없음)
    int         pendingDragX;    // 마우스 버튼 눌린 원점 X
    int         pendingDragY;    // 마우스 버튼 눌린 원점 Y
    int         scrollY;
    int         maxScrollY;
    HWND        hDlg;
    bool        closed;
    bool        saved;
    int         dropPos;
    int         dragX, dragY;
    PmButton    btns[PM_BTN_COUNT];
    int         gridH;
    int         gridW;
    int         cols;
    int         cellW;
    int         cellH;
    int         thumbW;
    int         thumbH;
};

// ── GDI+ 헬퍼 ────────────────────────────────────────────────────────────────
static Gdiplus::GraphicsPath* MakeRoundRect(float x, float y, float w, float h, float r)
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

// ── 썸네일 렌더링 ──────────────────────────────────────────────────────────────
static HBITMAP RenderThumb(FPDF_DOCUMENT doc, int pageIdx, int maxW, int maxH)
{
    if (!doc || pageIdx < 0) return nullptr;
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIdx);
    if (!page) return nullptr;

    double pw = FPDF_GetPageWidth(page);
    double ph = FPDF_GetPageHeight(page);
    float scale = (ph > 0) ? std::min((float)maxW / (float)pw,
                                       (float)maxH / (float)ph) : 1.f;
    int tw = std::max(1, (int)(pw * scale));
    int th = std::max(1, (int)(ph * scale));

    HDC hdc = GetDC(nullptr);
    HBITMAP hBmp = CreateCompatibleBitmap(hdc, tw, th);
    HDC hMem = CreateCompatibleDC(hdc);
    HBITMAP hOld = (HBITMAP)SelectObject(hMem, hBmp);

    RECT r = {0, 0, tw, th};
    FillRect(hMem, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));

    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(tw, th, FPDFBitmap_BGRx, nullptr, 0);
    FPDFBitmap_FillRect(bmp, 0, 0, tw, th, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bmp, page, 0, 0, tw, th, 0, FPDF_ANNOT);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = tw;
    bi.bmiHeader.biHeight      = -th;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBits(hMem, hBmp, 0, th, FPDFBitmap_GetBuffer(bmp), &bi, DIB_RGB_COLORS);

    FPDFBitmap_Destroy(bmp);
    FPDF_ClosePage(page);
    SelectObject(hMem, hOld);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hdc);
    return hBmp;
}

// ── 동적 그리드 레이아웃 계산 ─────────────────────────────────────────────────
// 창 너비에 맞게 열 수와 셀 크기를 결정한다.
static void CalcGridLayout(PageMgrState* s)
{
    // 사용 가능한 너비에서 열 수 결정:
    // 최소 셀 너비 = 썸네일 80 + 패딩 24 = 104, 최소 3열 보장
    int avail = s->gridW - PM_MARGIN * 2;
    // cols를 최대로 늘려 셀이 avail 에 딱 맞도록
    int cols = std::max(PM_MIN_COLS, (avail + PM_GAP) / (104 + PM_GAP));
    // 실제 셀 너비 = 열 수로 균등 분배
    int cellW = (avail - PM_GAP * (cols - 1)) / cols;
    int thumbW = cellW - PM_CELL_PAD * 2;
    int thumbH = (int)(thumbW * 100.0 / PM_THUMB_RATIO);  // 가로:세로 = 75:100
    int cellH = thumbH + PM_LABEL_H + PM_CELL_PAD * 2 + 4;

    s->cols   = cols;
    s->cellW  = cellW;
    s->cellH  = cellH;
    s->thumbW = thumbW;
    s->thumbH = thumbH;
}

// ── 버튼 배치 계산 ────────────────────────────────────────────────────────────
static void LayoutButtons(PageMgrState* s, int clientW, int clientH)
{
    const int BW = 100, BH = 34, GAP = 10;
    int totalW = BW * PM_BTN_COUNT + GAP * (PM_BTN_COUNT - 1);
    int bx = (clientW - totalW) / 2;
    int by = clientH - PM_BTN_AREA + (PM_BTN_AREA - BH) / 2;

    const wchar_t* labels[PM_BTN_COUNT] = { LS(STR_PM_BTN_PREV), LS(STR_PM_BTN_NEXT), LS(STR_PM_BTN_DELETE), LS(STR_PM_BTN_ADD), LS(STR_PM_BTN_SAVE), LS(STR_PM_BTN_CANCEL) };
    int styles[PM_BTN_COUNT]   = { 4, 4, 2, 3, 1, 0 };
    int iconIds[PM_BTN_COUNT]  = { IDB_NAV_PREV_PNG, IDB_NAV_NEXT_PNG, 0, 0, 0, 0 };

    for (int i = 0; i < PM_BTN_COUNT; ++i) {
        s->btns[i].rc        = { bx, by, bx + BW, by + BH };
        s->btns[i].hovered   = false;
        s->btns[i].pressed   = false;
        s->btns[i].style     = styles[i];
        s->btns[i].iconResId = iconIds[i];
        wcscpy_s(s->btns[i].label, labels[i]);
        bx += BW + GAP;
    }
}

// ── 히트 테스트 ───────────────────────────────────────────────────────────────
static int PmHitTest(PageMgrState* s, int mx, int my)
{
    for (int i = 0; i < (int)s->order.size(); ++i) {
        int col = i % s->cols;
        int row = i / s->cols;
        int x = PM_MARGIN + col * (s->cellW + PM_GAP);
        int y = PM_MARGIN + row * (s->cellH + PM_GAP) - s->scrollY;
        if (mx >= x && mx < x + s->cellW && my >= y && my < y + s->cellH)
            return i;
    }
    return -1;
}

// ── 드롭 위치 계산 ────────────────────────────────────────────────────────────
static int PmDropPos(PageMgrState* s, int mx, int my)
{
    int n = (int)s->order.size();
    int bestPos = n;
    int bestDist = INT_MAX;
    for (int i = 0; i <= n; ++i) {
        int lineX, lineY;
        if (i < n) {
            int col = i % s->cols, row = i / s->cols;
            lineX = PM_MARGIN + col * (s->cellW + PM_GAP) - PM_GAP / 2;
            lineY = PM_MARGIN + row * (s->cellH + PM_GAP) - s->scrollY + s->cellH / 2;
        } else {
            int col = (n - 1) % s->cols, row = (n - 1) / s->cols;
            lineX = PM_MARGIN + col * (s->cellW + PM_GAP) + s->cellW + PM_GAP / 2;
            lineY = PM_MARGIN + row * (s->cellH + PM_GAP) - s->scrollY + s->cellH / 2;
        }
        int dist = (mx - lineX) * (mx - lineX) + (my - lineY) * (my - lineY);
        if (dist < bestDist) { bestDist = dist; bestPos = i; }
    }
    return bestPos;
}

// ── 페이지 이동 ───────────────────────────────────────────────────────────────
static void PmMovePage(PageMgrState* s, int from, int to)
{
    if (from == to || from < 0 || to < 0 ||
        from >= (int)s->order.size() || to >= (int)s->order.size()) return;
    PageEntry val = s->order[from];
    HBITMAP tb = s->thumbs[from];
    s->order.erase(s->order.begin() + from);
    s->thumbs.erase(s->thumbs.begin() + from);
    s->order.insert(s->order.begin() + to, val);
    s->thumbs.insert(s->thumbs.begin() + to, tb);
}

// ── 스크롤 최대값 갱신 ────────────────────────────────────────────────────────
static void UpdateMaxScroll(PageMgrState* s)
{
    int n = (int)s->order.size();
    int rows = (n + s->cols - 1) / s->cols;
    int totalH = PM_MARGIN * 2 + rows * s->cellH + std::max(0, rows - 1) * PM_GAP;
    s->maxScrollY = std::max(0, totalH - s->gridH);
}

// ── GDI+ 버튼 그리기 ─────────────────────────────────────────────────────────
static void DrawPmButton(Gdiplus::Graphics& g, const PmButton& btn)
{
    using namespace Gdiplus;
    float x = (float)btn.rc.left,  y = (float)btn.rc.top;
    float w = (float)(btn.rc.right - btn.rc.left);
    float h = (float)(btn.rc.bottom - btn.rc.top);

    auto* path = MakeRoundRect(x + 0.5f, y + 0.5f, w - 1.f, h - 1.f, 5.f);

    if (btn.style == 4) {                        // Nav (이전/다음) — 투명 배경, 아이콘만
        if (btn.pressed) {
            // 눌림: 어두운 반투명 원형
            SolidBrush pb(Color(60, 0, 0, 0));
            g.FillPath(&pb, path);
        } else if (btn.hovered) {
            // 호버: 밝은 반투명 원형
            SolidBrush hb(Color(40, 0, 0, 0));
            g.FillPath(&hb, path);
        }
        // 배경 없음 → path 삭제 후 아이콘만
        delete path;
        if (btn.iconResId != 0) {
            Gdiplus::Bitmap* icon = PM_LoadPngBitmap(btn.iconResId);
            if (icon) {
                const float iconH = h * 0.65f;
                const float iconW = iconH * ((float)icon->GetWidth() / (float)icon->GetHeight());
                float ix = x + (w - iconW) * 0.5f;
                float iy = y + (h - iconH) * 0.5f;
                // 눌림 시 살짝 아래로
                if (btn.pressed) iy += 1.f;
                g.DrawImage(icon, Gdiplus::RectF(ix, iy, iconW, iconH));
                delete icon;
            }
        }
        return;
    } else if (btn.style == 1) {                 // Primary (저장) — 하늘색
        Color top = btn.pressed ? Clr::BTN_PRI_PRS
                  : btn.hovered ? Clr::BTN_PRI_HOV : Clr::BTN_PRI_TOP;
        Color bot = btn.pressed ? Clr::BTN_PRI_PRS
                  : btn.hovered ? Clr::BTN_PRI_HOV : Clr::BTN_PRI_BOT;
        LinearGradientBrush gb(PointF(x,y), PointF(x+w, y), top, bot);
        g.FillPath(&gb, path);
        if (btn.pressed) { Pen rp(Clr::BTN_PRESSED_RING, 2.f); g.DrawPath(&rp, path); }
    } else if (btn.style == 2) {                 // Danger (삭제) — 빨강
        Color top = btn.pressed ? Clr::BTN_DEL_PRS
                  : btn.hovered ? Clr::BTN_DEL_HOV : Clr::BTN_DEL_TOP;
        Color bot = btn.pressed ? Clr::BTN_DEL_PRS
                  : btn.hovered ? Clr::BTN_DEL_HOV : Clr::BTN_DEL_BOT;
        LinearGradientBrush gb(PointF(x,y), PointF(x+w, y), top, bot);
        g.FillPath(&gb, path);
        if (btn.pressed) { Pen rp(Clr::BTN_PRESSED_RING, 2.f); g.DrawPath(&rp, path); }
    } else if (btn.style == 3) {                 // Add (파일 추가) — 보라
        Color top = btn.pressed ? Clr::BTN_ADD_PRS
                  : btn.hovered ? Clr::BTN_ADD_HOV : Clr::BTN_ADD_TOP;
        Color bot = btn.pressed ? Clr::BTN_ADD_PRS
                  : btn.hovered ? Clr::BTN_ADD_HOV : Clr::BTN_ADD_BOT;
        LinearGradientBrush gb(PointF(x,y), PointF(x+w, y), top, bot);
        g.FillPath(&gb, path);
        if (btn.pressed) { Pen rp(Clr::BTN_PRESSED_RING, 2.f); g.DrawPath(&rp, path); }
    } else {                                     // Secondary (취소) — 주황
        Color top = btn.pressed ? Clr::BTN_SEC_PRS
                  : btn.hovered ? Clr::BTN_SEC_HOV : Clr::BTN_SEC_TOP;
        Color bot = btn.pressed ? Clr::BTN_SEC_PRS
                  : btn.hovered ? Clr::BTN_SEC_HOV : Clr::BTN_SEC_BOT;
        LinearGradientBrush gb(PointF(x,y), PointF(x+w, y), top, bot);
        g.FillPath(&gb, path);
        if (btn.pressed) { Pen rp(Clr::BTN_PRESSED_RING, 2.f); g.DrawPath(&rp, path); }
    }
    delete path;

    Color txtClr = (btn.style == 0) ? Clr::BTN_TXT_SEC : Clr::BTN_TXT_PRI;
    SolidBrush tBrush(txtClr);
    FontFamily ff(L"Segoe UI");
    Font fnt(&ff, 9, FontStyleBold, UnitPoint);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);

    if (btn.iconResId != 0) {
        // 아이콘만 버튼 중앙에 렌더링
        Gdiplus::Bitmap* icon = PM_LoadPngBitmap(btn.iconResId);
        if (icon) {
            const float iconH = h * 0.55f;
            const float iconW = iconH * ((float)icon->GetWidth() / (float)icon->GetHeight());
            float ix = x + (w - iconW) * 0.5f;
            float iy = y + (h - iconH) * 0.5f;
            g.DrawImage(icon, Gdiplus::RectF(ix, iy, iconW, iconH));
            delete icon;
        }
    } else {
        RectF tr(x, y, w, h);
        g.DrawString(btn.label, -1, &fnt, tr, &sf, &tBrush);
    }
}

// ── 메인 페인트 ───────────────────────────────────────────────────────────────
static void PmPaint(HWND hDlg, PageMgrState* s)
{
    using namespace Gdiplus;

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hDlg, &ps);
    RECT cr; GetClientRect(hDlg, &cr);
    int W = cr.right, H = cr.bottom;

    // 오프스크린 버퍼
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP memOld = (HBITMAP)SelectObject(memDC, memBmp);

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // ── 전체 배경 ──
    SolidBrush bgBrush(Clr::BG);
    g.FillRectangle(&bgBrush, 0, 0, W, H);

    // ── 그리드 영역 배경 ──
    SolidBrush gridBrush(Clr::GRID_BG);
    g.FillRectangle(&gridBrush, 0, 0, W, s->gridH);

    // ── 스크롤 클리핑: 그리드 영역 안에서만 그리기 ──
    g.SetClip(RectF(0, 0, (float)W, (float)s->gridH));

    int n = (int)s->order.size();
    FontFamily ff(L"Segoe UI");
    Font labelFont(&ff, 8.5f, FontStyleRegular, UnitPoint);
    Font labelFontSel(&ff, 8.5f, FontStyleBold, UnitPoint);

    for (int i = 0; i < n; ++i) {
        int col = i % s->cols;
        int row = i / s->cols;
        float cx = (float)(PM_MARGIN + col * (s->cellW + PM_GAP));
        float cy = (float)(PM_MARGIN + row * (s->cellH + PM_GAP) - s->scrollY);
        float cw = (float)s->cellW, ch = (float)s->cellH;

        if (cy + ch < 0 || cy > s->gridH) continue;

        bool selected = (i == s->selected);
        bool isExt = (s->order[i].docIdx > 0);  // 추가된 파일 페이지

        // 셀 배경 + 그림자 효과 (선택 시 파란 배경)
        auto* cellPath = MakeRoundRect(cx + 1, cy + 1, cw - 2, ch - 2, 6.f);
        if (selected) {
            SolidBrush selBg(isExt ? Clr::EXT_SEL_FILL : Clr::SEL_FILL);
            g.FillPath(&selBg, cellPath);
            Pen selPen(isExt ? Clr::EXT_SEL_RING : Clr::SEL_RING, 2.f);
            g.DrawPath(&selPen, cellPath);
        } else {
            SolidBrush cellBg(isExt ? Clr::EXT_CELL_BG : Clr::CELL_BG);
            g.FillPath(&cellBg, cellPath);
            Pen cellPen(isExt ? Clr::EXT_CELL_BDR : Clr::CELL_BORDER, 1.f);
            g.DrawPath(&cellPen, cellPath);
        }
        delete cellPath;

        // 썸네일 그리기 (셀 내 중앙 정렬)
        if (s->thumbs[i]) {
            BITMAP bm{}; GetObject(s->thumbs[i], sizeof(bm), &bm);
            int ox = (int)cx + PM_CELL_PAD + (s->thumbW - bm.bmWidth)  / 2;
            int oy = (int)cy + PM_CELL_PAD + (s->thumbH - bm.bmHeight) / 2;

            HDC tDC = CreateCompatibleDC(memDC);
            HBITMAP tOld = (HBITMAP)SelectObject(tDC, s->thumbs[i]);
            // 썸네일 주위 얇은 그림자 느낌의 테두리
            RECT tBorder = { ox - 1, oy - 1, ox + bm.bmWidth + 1, oy + bm.bmHeight + 1 };
            FrameRect(memDC, &tBorder, CreateSolidBrush(RGB(200, 202, 206)));
            BitBlt(memDC, ox, oy, bm.bmWidth, bm.bmHeight, tDC, 0, 0, SRCCOPY);
            SelectObject(tDC, tOld);
            DeleteDC(tDC);
        }

        // 페이지 번호 레이블
        WCHAR label[64];
        if (isExt && s->order[i].docIdx - 1 < (int)s->extraPaths.size()) {
            // 파일명만 추출해 표시
            const std::wstring& ep = s->extraPaths[static_cast<size_t>(s->order[i].docIdx) - 1];
            size_t slash = ep.find_last_of(L"\\/");
            std::wstring fname = (slash != std::wstring::npos) ? ep.substr(slash + 1) : ep;
            // 확장자 제거
            size_t dot = fname.rfind(L'.');
            if (dot != std::wstring::npos) fname = fname.substr(0, dot);
            // 너무 길면 자름
            if (fname.size() > 10) fname = fname.substr(0, 9) + L"\u2026";
            swprintf_s(label, L"%s / %d", fname.c_str(), s->order[i].pageIdx + 1);
        } else {
            swprintf_s(label, L"%d", s->order[i].pageIdx + 1);
        }
        SolidBrush lblBrush(selected ? (isExt ? Clr::EXT_LABEL : Clr::SEL_LABEL)
                                     : (isExt ? Clr::EXT_LABEL : Clr::LABEL_TEXT));
        RectF lblR(cx, cy + PM_CELL_PAD + s->thumbH + 2, cw, (float)PM_LABEL_H + PM_CELL_PAD);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(label, -1, selected ? &labelFontSel : &labelFont, lblR, &sf, &lblBrush);
    }

    // ── 드롭 위치 표시선 ──
    if (s->dragFrom >= 0 && s->dropPos >= 0) {
        int di = (s->dropPos < n) ? s->dropPos : n - 1;
        int col = di % s->cols, row = di / s->cols;
        float lx, ly;
        if (s->dropPos < n) {
            lx = (float)(PM_MARGIN + col * (s->cellW + PM_GAP) - PM_GAP / 2);
        } else {
            lx = (float)(PM_MARGIN + col * (s->cellW + PM_GAP) + s->cellW + PM_GAP / 2);
        }
        ly = (float)(PM_MARGIN + row * (s->cellH + PM_GAP) - s->scrollY);

        Pen dropPen(Clr::DROP_LINE, 3.f);
        g.DrawLine(&dropPen, lx, ly - 6, lx, ly + (float)s->cellH + 6);

        // 위·아래 삼각형 화살표
        SolidBrush arrowBrush(Clr::DROP_LINE);
        PointF top[3] = { {lx-5, ly-7}, {lx+5, ly-7}, {lx, ly-2} };
        PointF bot[3] = { {lx-5, ly+(float)s->cellH+7}, {lx+5, ly+(float)s->cellH+7}, {lx, ly+(float)s->cellH+2} };
        g.FillPolygon(&arrowBrush, top, 3);
        g.FillPolygon(&arrowBrush, bot, 3);
    }

    // ── 드래그 미리보기 썸네일 ──
    if (s->dragFrom >= 0 && s->dragX >= 0 && s->dragFrom < (int)s->thumbs.size() && s->thumbs[s->dragFrom]) {
        BITMAP bm{}; GetObject(s->thumbs[s->dragFrom], sizeof(bm), &bm);
        int dx = s->dragX - bm.bmWidth / 2;
        int dy = s->dragY - bm.bmHeight / 2;

        HDC tDC = CreateCompatibleDC(memDC);
        HBITMAP tOld = (HBITMAP)SelectObject(tDC, s->thumbs[s->dragFrom]);
        BLENDFUNCTION bf = {};
        bf.BlendOp             = AC_SRC_OVER;
        bf.SourceConstantAlpha = 200;
        AlphaBlend(memDC, dx, dy, bm.bmWidth, bm.bmHeight, tDC, 0, 0, bm.bmWidth, bm.bmHeight, bf);
        SelectObject(tDC, tOld);
        DeleteDC(tDC);

        // 미리보기 테두리
        auto* dpPath = MakeRoundRect((float)dx - 1, (float)dy - 1, (float)bm.bmWidth + 2, (float)bm.bmHeight + 2, 4.f);
        Pen dpPen(Clr::DRAG_BORDER, 2.f);
        g.DrawPath(&dpPen, dpPath);
        delete dpPath;
    }

    // ── 클리핑 해제 후 버튼 바 그리기 ──
    g.ResetClip();

    // 버튼 바 배경
    SolidBrush btnBarBrush(Clr::BTN_BAR);
    g.FillRectangle(&btnBarBrush, 0, s->gridH, W, PM_BTN_AREA);

    // 구분선
    Pen divPen(Clr::DIVIDER, 1.f);
    g.DrawLine(&divPen, 0.f, (float)s->gridH, (float)W, (float)s->gridH);

    // 버튼들
    for (int i = 0; i < PM_BTN_COUNT; ++i)
        DrawPmButton(g, s->btns[i]);

    // ── 스크롤바 표시 (필요한 경우) ──
    if (s->maxScrollY > 0) {
        float trackH = (float)(s->gridH - 8);
        float thumbH = std::max(30.f, trackH * s->gridH / (s->gridH + s->maxScrollY));
        float thumbY = 4.f + (trackH - thumbH) * s->scrollY / s->maxScrollY;
        auto* scrollPath = MakeRoundRect((float)(W - 8), thumbY, 5.f, thumbH, 2.5f);
        SolidBrush scrollBrush(Color(120, 100, 100, 100));
        g.FillPath(&scrollBrush, scrollPath);
        delete scrollPath;
    }

    BitBlt(hdc, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, memOld);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(hDlg, &ps);
}

// ── WndProc ───────────────────────────────────────────────────────────────────
static LRESULT CALLBACK PageMgrDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PageMgrState* s = reinterpret_cast<PageMgrState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = reinterpret_cast<PageMgrState*>(cs->lpCreateParams);
        s->hDlg = hDlg;
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)s);

        RECT cr; GetClientRect(hDlg, &cr);
        s->gridH = cr.bottom - PM_BTN_AREA;
        s->gridW = cr.right;
        CalcGridLayout(s);
        LayoutButtons(s, cr.right, cr.bottom);
        UpdateMaxScroll(s);

        // 썸네일 생성
        s->thumbs.resize(s->order.size(), nullptr);
        for (int i = 0; i < (int)s->order.size(); ++i) {
            FPDF_DOCUMENT doc = (s->order[i].docIdx == 0) ? g_pdfDoc
                              : s->extraDocs[static_cast<size_t>(s->order[i].docIdx) - 1];
            s->thumbs[i] = RenderThumb(doc, s->order[i].pageIdx, s->thumbW, s->thumbH);
        }

        return 0;
    }
    case WM_SIZE:
    {
        if (!s) break;
        int W = LOWORD(lParam), H = HIWORD(lParam);
        s->gridH = H - PM_BTN_AREA;
        s->gridW = W;
        CalcGridLayout(s);
        LayoutButtons(s, W, H);
        UpdateMaxScroll(s);
        s->scrollY = std::min(s->scrollY, s->maxScrollY);
        // 썸네일 크기가 바뀔 수 있으므로 재생성
        for (int i = 0; i < (int)s->thumbs.size(); ++i) {
            if (s->thumbs[i]) { DeleteObject(s->thumbs[i]); s->thumbs[i] = nullptr; }
            FPDF_DOCUMENT doc = (s->order[i].docIdx == 0) ? g_pdfDoc
                              : s->extraDocs[static_cast<size_t>(s->order[i].docIdx) - 1];
            s->thumbs[i] = RenderThumb(doc, s->order[i].pageIdx, s->thumbW, s->thumbH);
        }
        InvalidateRect(hDlg, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        if (s) PmPaint(hDlg, s);
        return 0;

    case WM_MOUSEWHEEL:
    {
        if (!s) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        s->scrollY = std::clamp(s->scrollY - delta / 3, 0, s->maxScrollY);
        InvalidateRect(hDlg, nullptr, FALSE);
        return 0;
    }
    case WM_VSCROLL:
    {
        if (!s) break;
        switch (LOWORD(wParam)) {
        case SB_LINEUP:   s->scrollY = std::max(0, s->scrollY - 30); break;
        case SB_LINEDOWN: s->scrollY = std::min(s->maxScrollY, s->scrollY + 30); break;
        case SB_PAGEUP:   s->scrollY = std::max(0, s->scrollY - s->gridH); break;
        case SB_PAGEDOWN: s->scrollY = std::min(s->maxScrollY, s->scrollY + s->gridH); break;
        case SB_THUMBTRACK: s->scrollY = std::clamp((int)HIWORD(wParam), 0, s->maxScrollY); break;
        }
        InvalidateRect(hDlg, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        if (!s) break;
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);

        // 버튼 클릭 확인
        for (int i = 0; i < PM_BTN_COUNT; ++i)
            if (PtInRect(&s->btns[i].rc, {mx, my})) { s->btns[i].pressed = true; return 0; }

        // 그리드 영역에서만 드래그 시작
        if (my < s->gridH) {
            int hit = PmHitTest(s, mx, my);
            if (hit >= 0) {
                s->selected         = hit;
                s->pendingDragFrom  = hit;
                s->pendingDragX     = mx;
                s->pendingDragY     = my;
                SetCapture(hDlg);
            } else {
                // 빈 곳 클릭 시 선택 해제
                s->selected = -1;
            }
        }
        InvalidateRect(hDlg, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!s) break;
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);

        // 버튼 호버
        bool changed = false;
        for (int i = 0; i < PM_BTN_COUNT; ++i) {
            bool over = !!PtInRect(&s->btns[i].rc, {mx, my});
            if (over != s->btns[i].hovered) { s->btns[i].hovered = over; changed = true; }
        }

        if (s->dragFrom >= 0 && my < s->gridH) {
            s->dragX   = mx;
            s->dragY   = my;
            s->dropPos = PmDropPos(s, mx, my);
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            InvalidateRect(hDlg, nullptr, FALSE);
        } else if (s->pendingDragFrom >= 0) {
            // 대기 중 — 충분히 움직였으면 드래그 활성화
            int dx = mx - s->pendingDragX;
            int dy = my - s->pendingDragY;
            int threshold = std::max(GetSystemMetrics(SM_CXDRAG),
                                     GetSystemMetrics(SM_CYDRAG));
            if (dx*dx + dy*dy > threshold*threshold) {
                s->dragFrom        = s->pendingDragFrom;
                s->pendingDragFrom = -1;
                s->dropPos         = -1;
                s->dragX           = mx;
                s->dragY           = my;
            }
            InvalidateRect(hDlg, nullptr, FALSE);
        } else if (changed) {
            InvalidateRect(hDlg, nullptr, FALSE);
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }

        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hDlg, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        if (!s) break;
        for (int i = 0; i < PM_BTN_COUNT; ++i) s->btns[i].hovered = false;
        InvalidateRect(hDlg, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (!s) break;
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);

        // 버튼 클릭 처리
        for (int i = 0; i < PM_BTN_COUNT; ++i) {
            if (s->btns[i].pressed) {
                s->btns[i].pressed = false;
                if (!PtInRect(&s->btns[i].rc, {mx, my})) continue;
                switch (i) {
                case PM_BTN_UP:
                    if (s->selected > 0) { PmMovePage(s, s->selected, s->selected - 1); s->selected--; }
                    break;
                case PM_BTN_DOWN:
                    if (s->selected >= 0 && s->selected < (int)s->order.size() - 1)
                    { PmMovePage(s, s->selected, s->selected + 1); s->selected++; }
                    break;
                case PM_BTN_DEL:
                    if (s->selected >= 0 && (int)s->order.size() > 1) {
                        if (CleanPopup::Show(hDlg, LS(STR_PM_BTN_DELETE), LS(STR_PM_DELETE_CONFIRM), true)) {
                            DeleteObject(s->thumbs[s->selected]);
                            s->thumbs.erase(s->thumbs.begin() + s->selected);
                            s->order.erase(s->order.begin() + s->selected);
                            if (s->selected >= (int)s->order.size()) s->selected = (int)s->order.size() - 1;
                            UpdateMaxScroll(s);
                        }
                    }
                    break;
                case PM_BTN_ADD:
                {
                    // 파일 탐색기로 PDF 선택
                    OPENFILENAMEW ofn = {};
                    WCHAR fileBuf[MAX_PATH] = {};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner   = hDlg;
                    ofn.lpstrFilter = LS(STR_PM_PDF_FILTER);
                    ofn.lpstrFile   = fileBuf;
                    ofn.nMaxFile    = MAX_PATH;
                    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    ofn.lpstrTitle  = LS(STR_PM_OPEN_TITLE);
                    if (GetOpenFileNameW(&ofn)) {
                        int u8Len = WideCharToMultiByte(CP_UTF8, 0, fileBuf, -1, nullptr, 0, nullptr, nullptr);
                        std::string u8Path(u8Len, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, fileBuf, -1, u8Path.data(), u8Len, nullptr, nullptr);
                        std::string addPw;
                                                FPDF_DOCUMENT addDoc = OpenPdfByPathWithPrompt(hDlg, u8Path, &addPw);
                        if (!addDoc) {
                            MessageBoxW(hDlg, LS(STR_PM_FILE_OPEN_ERR), LS(STR_ERROR), MB_OK | MB_ICONERROR);
                        } else {
                            int newDocIdx = (int)s->extraDocs.size() + 1;
                            s->extraDocs.push_back(addDoc);
                            s->extraPaths.push_back(std::wstring(fileBuf));
                            s->extraPasswords.push_back(addPw);
                            int addPageCount = FPDF_GetPageCount(addDoc);
                            for (int p = 0; p < addPageCount; ++p) {
                                PageEntry pe{ newDocIdx, p };
                                s->order.push_back(pe);
                                HBITMAP tb = RenderThumb(addDoc, p, s->thumbW, s->thumbH);
                                s->thumbs.push_back(tb);
                            }
                            UpdateMaxScroll(s);
                            // 추가된 첫 페이지로 스크롤
                            s->selected = (int)s->order.size() - addPageCount;
                            int selRow = s->selected / s->cols;
                            int selY = PM_MARGIN + selRow * (s->cellH + PM_GAP);
                            s->scrollY = std::clamp(selY - s->gridH / 2, 0, s->maxScrollY);
                        }
                    }
                    break;
                }
                case PM_BTN_SAVE:
                    if (CleanPopup::Show(hDlg, LS(STR_PM_BTN_SAVE), LS(STR_PM_SAVE_CONFIRM), true)) {
                        s->saved = true; s->closed = true;
                        DestroyWindow(hDlg);
                        return 0;
                    }
                    break;
                case PM_BTN_CANCEL:
                    s->saved = false; s->closed = true;
                    DestroyWindow(hDlg);
                    return 0;
                }
                InvalidateRect(hDlg, nullptr, FALSE);
            }
        }

        // 드래그 드롭 처리
        if (s->dragFrom >= 0) {
            int pos = s->dropPos;
            if (pos >= 0 && pos != s->dragFrom && pos != s->dragFrom + 1) {
                int insertAt = (pos > s->dragFrom) ? pos - 1 : pos;
                PmMovePage(s, s->dragFrom, insertAt);
                s->selected = insertAt;
            }
            s->dragFrom = -1;
            s->dropPos  = -1;
            ReleaseCapture();
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            InvalidateRect(hDlg, nullptr, FALSE);
        }
        // 대기 상태 해제 (짧은 클릭 — 드래그 없이 선택만)
        if (s->pendingDragFrom >= 0) {
            s->pendingDragFrom = -1;
            ReleaseCapture();
        }
        return 0;
    }
    case WM_CLOSE:
        if (s) { s->saved = false; s->closed = true; }
        DestroyWindow(hDlg);
        return 0;

    case WM_DESTROY:
        if (s) {
            s->closed = true;
            for (auto h : s->thumbs) if (h) DeleteObject(h);
            s->thumbs.clear();
            for (auto doc : s->extraDocs) if (doc) FPDF_CloseDocument(doc);
            s->extraDocs.clear();
        }
        return 0;
    }
    return DefWindowProcW(hDlg, msg, wParam, lParam);
}

// ── ShowPageManager ───────────────────────────────────────────────────────────
void ShowPageManager(HWND hWnd)
{
    if (!g_pdfDoc || g_pageCount <= 0) {
        CleanPopup::Show(hWnd, LS(STR_PM_TITLE), LS(STR_PM_NO_PDF));
        return;
    }

    // GDI+ 초기화
    static ULONG_PTR gdipToken = 0;
    if (!gdipToken) {
        Gdiplus::GdiplusStartupInput si;
        Gdiplus::GdiplusStartup(&gdipToken, &si, nullptr);
    }

    PageMgrState s = {};
    s.hWnd      = hWnd;
    s.pageCount = g_pageCount;
    s.selected        = -1;
    s.dragFrom        = -1;
    s.pendingDragFrom = -1;
    s.pendingDragX    = -1;
    s.pendingDragY    = -1;
    s.dropPos         = -1;
    s.dragX           = -1;
    s.dragY           = -1;
    s.order.resize(g_pageCount);
    for (int i = 0; i < g_pageCount; ++i) s.order[i] = { 0, i };

    // 창 클래스 등록
    static bool registered = false;
    if (!registered) {
        HINSTANCE hInst0 = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = PageMgrDlgProc;
        wc.hInstance     = hInst0;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.hIcon         = LoadIconW(hInst0, MAKEINTRESOURCEW(IDI_PLAINPDFVIEWER));
        wc.hIconSm       = LoadIconW(hInst0, MAKEINTRESOURCEW(IDI_PLAINPDFVIEWER));
        wc.lpszClassName = L"PageMgrDlg";
        RegisterClassExW(&wc);
        registered = true;
    }

    // 창 크기: 모니터의 70% × 80%
    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    int monW = mi.rcWork.right  - mi.rcWork.left;
    int monH = mi.rcWork.bottom - mi.rcWork.top;
    int dlgW = (int)(monW * 0.70);
    int dlgH = (int)(monH * 0.80);

    int dlgX = mi.rcWork.left + (monW - dlgW) / 2;
    int dlgY = mi.rcWork.top  + (monH - dlgH) / 2;

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);
    HWND hDlg = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"PageMgrDlg", LS(STR_PM_TITLE),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        dlgX, dlgY, dlgW, dlgH,
        hWnd, nullptr, hInst, &s);
    if (!hDlg) return;

    // Windows 11 둥근 모서리
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hDlg, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    EnableWindow(hWnd, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    // 모달 루프
    MSG msg;
    while (!s.closed) {
        BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret == 0) { PostQuitMessage((int)msg.wParam); break; }
        if (ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hWnd, TRUE);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

    if (!s.saved) return;

    // ── 새 순서로 PDF 재구성 (병합 포함) ────────────────────────────────────
    EmbedHighlightAnnotations();
    EmbedNoteAnnotations();
    EmbedTextBoxAnnotations();
    EmbedSignatureObjects();

    // 원본 문서를 임시 파일로 저장해 docIdx=0 소스로 사용
    std::wstring tempSrc = g_filePath + L".pmsrc.tmp";
    if (!SavePDFToFile(tempSrc)) {
        MessageBoxW(hWnd, LS(STR_PM_SAVE_FAIL), LS(STR_ERROR), MB_OK | MB_ICONERROR);
        return;
    }
    int u8Len0 = WideCharToMultiByte(CP_UTF8, 0, tempSrc.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u8Path0(u8Len0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, tempSrc.c_str(), -1, u8Path0.data(), u8Len0, nullptr, nullptr);
    FPDF_DOCUMENT srcDoc = FPDF_LoadDocument(u8Path0.c_str(), nullptr);
    if (!srcDoc) {
        DeleteFileW(tempSrc.c_str());
        MessageBoxW(hWnd, LS(STR_PM_REORDER_FAIL), LS(STR_ERROR), MB_OK | MB_ICONERROR);
        return;
    }

    // 추가 파일들도 로드 (WM_DESTROY 이전에 이미 닫혔을 수 있으므로 다시 열기)
    std::vector<FPDF_DOCUMENT> saveDocs;  // index 1+ 대응
    for (size_t epi = 0; epi < s.extraPaths.size(); ++epi) {
        const auto& ep = s.extraPaths[epi];
        const std::string pwStr = (epi < s.extraPasswords.size()) ? s.extraPasswords[epi] : std::string();
        int u8Len = WideCharToMultiByte(CP_UTF8, 0, ep.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string u8p(u8Len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, ep.c_str(), -1, u8p.data(), u8Len, nullptr, nullptr);
        saveDocs.push_back(FPDF_LoadDocument(u8p.c_str(), pwStr.empty() ? nullptr : pwStr.c_str()));
    }

    FPDF_DOCUMENT newDoc = FPDF_CreateNewDocument();
    bool importOk = true;
    for (int i = 0; i < (int)s.order.size(); ++i) {
        const PageEntry& pe = s.order[i];
        FPDF_DOCUMENT fromDoc = (pe.docIdx == 0) ? srcDoc : saveDocs[static_cast<size_t>(pe.docIdx) - 1];
        if (!fromDoc) { importOk = false; break; }
        int indices[1] = { pe.pageIdx };
        if (!FPDF_ImportPagesByIndex(newDoc, fromDoc, indices, 1, i)) { importOk = false; break; }
    }

    FPDF_CloseDocument(srcDoc);
    DeleteFileW(tempSrc.c_str());
    for (auto d : saveDocs) if (d) FPDF_CloseDocument(d);

    if (!importOk) {
        FPDF_CloseDocument(newDoc);
        MessageBoxW(hWnd, LS(STR_PM_IMPORT_FAIL), LS(STR_ERROR), MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring tempDest = g_filePath + L".pmdst.tmp";
    {
        FILE* fp = nullptr;
        _wfopen_s(&fp, tempDest.c_str(), L"wb");
        if (!fp) { FPDF_CloseDocument(newDoc); return; }
        PM_FileWriteContext ctx{};
        ctx.fileWrite.version    = 1;
        ctx.fileWrite.WriteBlock = PM_FileWriteBlock;
        ctx.fp = fp;
        FPDF_SaveAsCopy(newDoc, &ctx.fileWrite, 0);
        fclose(fp);
    }
    FPDF_CloseDocument(newDoc);

    std::wstring origPath = g_filePath;
    ClosePDF();
    if (!MoveFileExW(tempDest.c_str(), origPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MessageBoxW(hWnd, LS(STR_PM_REPLACE_FAIL), LS(STR_ERROR), MB_OK | MB_ICONERROR);
        return;
    }

    OpenFileFromPath(hWnd, origPath, false);
    FitToWidth(hWnd);
    ShowToast(hWnd, LS(STR_PM_SAVED_TOAST));
}
