// TextEdit.cpp : 텍스트 편집 모드 (점선 상자 표시, 클릭해서 고치기, 저장 시 반영)
//
// 다른 편집 기능(가림, 텍스트 박스 등)과 같은 방식으로 동작한다.
//  - 고친 내용은 문서별 대기 목록에만 들고 있고, 화면에는 오버레이로 미리 보여 준다.
//    (렌더 워커는 별도 문서 사본으로 그리므로 g_pdfDoc을 바로 바꾸면 화면에 반영되지 않는다)
//  - 저장할 때 ApplyPendingEdits()가 원래 텍스트 오브젝트를 새 오브젝트로 바꿔 넣는다.
//
// 글꼴: PDF에 들어 있는 글꼴은 대개 문서에 쓰인 글자만 담은 일부분이라 새 글자를 그릴 수 없다.
// 그래서 고친 오브젝트는 시스템 글꼴로 다시 만든다.
//  - 영문/숫자/기호(ASCII)만 있으면 표준 글꼴 Helvetica (파일에 넣지 않음, 미리보기는 Arial)
//  - 그 밖의 글자가 있으면 맑은 고딕을 쓰인 글자만 남겨 서브셋으로 넣는다 (미리보기도 맑은 고딕)

#include "framework.h"
#include "PlainPDFViewer.h"
#include "TextEdit.h"
#include "Lang.h"
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fontsub.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "fontsub.lib")

extern std::mutex g_pdfiumMtx;
extern bool       g_darkMode;

namespace
{
    // 밑줄/취소선: PDF에서는 글자와 별개인 가는 가로 선(경로) 오브젝트로 그려진다.
    // 글자만 바꾸면 선이 원래 길이로 남으므로, 텍스트 오브젝트에 묶어 두고 함께 바꾼다.
    struct Deco
    {
        int      objIndex = -1;
        float    l = 0, b = 0, r = 0, t = 0;  // PDF 페이지 좌표 경계
        COLORREF color = 0;
    };

    // 페이지의 텍스트 오브젝트 하나
    struct TextObj
    {
        int          objIndex = -1;          // FPDFPage_GetObject 인덱스
        float        l = 0, b = 0, r = 0, t = 0; // PDF 페이지 좌표 경계
        FS_MATRIX    m{};                    // 텍스트 행렬 (e, f = 첫 글자 기준선 원점)
        float        fontSize = 0;
        COLORREF     color = 0;
        bool         bold = false;
        std::wstring text;
        std::vector<Deco> decos;             // 이 글자에 딸린 밑줄/취소선
    };

    struct PageObjs
    {
        bool                 loaded = false;
        std::vector<TextObj> objs;
    };

    struct PendingEdit
    {
        int          page = -1;
        int          objIndex = -1;
        std::wstring original;   // 저장할 때 같은 오브젝트인지 확인하는 데 쓴다
        std::wstring text;
        bool         bold = false;
        std::vector<Deco> decos;
    };

    struct DocState
    {
        std::vector<PageObjs>    pages;
        std::vector<PendingEdit> edits;
    };

    struct PageGeo
    {
        int    left = 0, top = 0;
        double pdfW = 0, pdfH = 0;
        int    rot = 0;
    };

    std::map<FPDF_DOCUMENT, DocState> s_docs;
    bool  s_active    = false;
    int   s_hoverPage = -1;
    int   s_hoverObj  = -1;   // PageObjs::objs 안의 인덱스

    // 입력창 상태
    HWND          s_hMain      = nullptr;
    HWND          s_hEdit      = nullptr;
    HFONT         s_editFont   = nullptr;
    FPDF_DOCUMENT s_editDoc    = nullptr;
    int           s_editPage   = -1;
    int           s_editObj    = -1;
    bool          s_finishing  = false;

    // 저장 후에도 PDFium이 글꼴 데이터를 참조할 수 있으므로 다음 저장까지 보관
    std::vector<std::vector<unsigned char>> s_fontKeepAlive;

    bool IsAsciiText(const std::wstring& s)
    {
        for (wchar_t c : s)
            if (c < 0x20 || c > 0x7E) return false;
        return true;
    }

    bool IsBlank(const std::wstring& s)
    {
        for (wchar_t c : s)
            if (!iswspace(c)) return false;
        return true;
    }

    std::wstring ReadObjText(FPDF_PAGEOBJECT obj, FPDF_TEXTPAGE tp)
    {
        // 반환값은 끝의 NUL을 포함한 바이트 수
        unsigned long bytes = FPDFTextObj_GetText(obj, tp, nullptr, 0);
        if (bytes <= sizeof(FPDF_WCHAR)) return {};
        std::vector<FPDF_WCHAR> buf(bytes / sizeof(FPDF_WCHAR) + 1, 0);
        FPDFTextObj_GetText(obj, tp, buf.data(), bytes);
        return std::wstring(reinterpret_cast<const wchar_t*>(buf.data()));
    }

    // 굵은 글꼴인지: 굵기 값(600 이상) 또는 글꼴 이름(Bold/Black/Heavy)으로 판단
    bool IsBoldFont(FPDF_FONT font)
    {
        if (!font) return false;
        if (FPDFFont_GetWeight(font) >= 600) return true;
        char name[256] = {};
        if (FPDFFont_GetBaseFontName(font, name, sizeof(name)) == 0) return false;
        std::string n(name);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)tolower(c); });
        return n.find("bold") != std::string::npos || n.find("black") != std::string::npos
            || n.find("heavy") != std::string::npos;
    }

    // 가로로 바로 선 텍스트만 다룬다 (회전/기울임/뒤집힌 텍스트는 제외)
    bool IsUpright(const FS_MATRIX& m)
    {
        return m.a > 0 && m.d > 0
            && std::fabs(m.b) <= 0.01f * m.a
            && std::fabs(m.c) <= 0.01f * m.d;
    }

    // 현재 문서(g_pdfDoc)의 페이지 텍스트 오브젝트 목록 (처음 요청할 때 한 번만 읽는다)
    PageObjs* EnsurePage(int pi)
    {
        if (!g_pdfDoc || pi < 0 || pi >= g_pageCount) return nullptr;
        DocState& ds = s_docs[g_pdfDoc];
        if ((int)ds.pages.size() != g_pageCount)
            ds.pages.assign(g_pageCount, PageObjs{});
        PageObjs& po = ds.pages[pi];
        if (po.loaded) return &po;
        po.loaded = true;

        std::lock_guard<std::mutex> lk(g_pdfiumMtx);
        const bool isCur = (pi == g_currentPage && g_pdfPage);
        FPDF_PAGE page = isCur ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, pi);
        if (!page) return &po;
        const bool ownTp = !(isCur && g_textPage);
        FPDF_TEXTPAGE tp = ownTp ? FPDFText_LoadPage(page) : g_textPage;

        if (tp)
        {
            std::vector<Deco> lines; // 밑줄/취소선 후보 (가는 가로 선)
            const int n = FPDFPage_CountObjects(page);
            for (int i = 0; i < n; ++i)
            {
                FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
                if (!obj) continue;
                if (FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_PATH)
                {
                    Deco d;
                    d.objIndex = i;
                    if (FPDFPageObj_GetBounds(obj, &d.l, &d.b, &d.r, &d.t)
                        && d.t - d.b > 0 && d.r - d.l > 4 * (d.t - d.b))
                    {
                        int fillMode = 0;
                        FPDF_BOOL stroke = FALSE;
                        FPDFPath_GetDrawMode(obj, &fillMode, &stroke);
                        unsigned int R = 0, G = 0, B = 0, A = 255;
                        if (fillMode ? FPDFPageObj_GetFillColor(obj, &R, &G, &B, &A)
                                     : FPDFPageObj_GetStrokeColor(obj, &R, &G, &B, &A))
                            d.color = RGB(R, G, B);
                        lines.push_back(d);
                    }
                    continue;
                }
                if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;
                // OCR 등으로 넣은 보이지 않는 텍스트는 고칠 대상이 아니다
                if (FPDFTextObj_GetTextRenderMode(obj) == FPDF_TEXTRENDERMODE_INVISIBLE) continue;

                TextObj o;
                o.objIndex = i;
                if (!FPDFPageObj_GetMatrix(obj, &o.m) || !IsUpright(o.m)) continue;
                if (!FPDFPageObj_GetBounds(obj, &o.l, &o.b, &o.r, &o.t)) continue;
                if (o.r - o.l < 0.5f || o.t - o.b < 0.5f) continue;
                if (!FPDFTextObj_GetFontSize(obj, &o.fontSize) || o.fontSize <= 0) continue;

                o.text = ReadObjText(obj, tp);
                if (o.text.empty() || IsBlank(o.text)) continue;
                o.bold = IsBoldFont(FPDFTextObj_GetFont(obj));

                unsigned int R = 0, G = 0, B = 0, A = 255;
                if (FPDFPageObj_GetFillColor(obj, &R, &G, &B, &A))
                    o.color = RGB(R, G, B);
                po.objs.push_back(std::move(o));
            }

            // 선마다 그 선이 꾸미는 글자를 하나 고른다.
            // 조건: 두께가 글자 크기의 15% 이하, 글자 폭 안에 들어가고 그 절반 이상을 덮으며,
            //       세로 위치가 기준선 아래 0.4em ~ 위 0.75em (밑줄·취소선 자리).
            // 표 테두리처럼 글자보다 넓거나 멀리 떨어진 선은 여기서 걸러진다.
            for (const Deco& d : lines)
            {
                int best = -1;
                float bestErr = 0;
                for (int k = 0; k < (int)po.objs.size(); ++k)
                {
                    const TextObj& o = po.objs[k];
                    const float em = o.fontSize * o.m.d;
                    const float w = o.r - o.l;
                    const float cy = (d.b + d.t) / 2;
                    if (d.t - d.b > 0.15f * em + 0.5f) continue;
                    if (d.l < o.l - 0.3f * em || d.r > o.r + 0.3f * em) continue;
                    if (d.r - d.l < 0.5f * w) continue;
                    if (cy < o.m.f - 0.4f * em || cy > o.m.f + 0.75f * em) continue;
                    const float err = std::fabs(d.l - o.l) + std::fabs(d.r - o.r);
                    if (best < 0 || err < bestErr) { best = k; bestErr = err; }
                }
                if (best >= 0) po.objs[best].decos.push_back(d);
            }
        }

        if (tp && ownTp) FPDFText_ClosePage(tp);
        if (!isCur) FPDF_ClosePage(page);
        return &po;
    }

    bool GetPageGeo(int pi, const RECT& vp, PageGeo& g)
    {
        if (pi < 0 || pi >= (int)g_pageLayouts.size()) return false;
        GetPageScreenPos(pi, vp, &g.left, &g.top);
        g.pdfW = g_pageLayouts[pi].pdfW;
        g.pdfH = g_pageLayouts[pi].pdfH;
        g.rot  = GetEffectiveRotation(pi);
        return true;
    }

    bool IsPageVisible(int pi, const RECT& vp, const PageGeo& g)
    {
        const auto& L = g_pageLayouts[pi];
        const bool sideways = (g.rot % 2) != 0;
        const int w = sideways ? L.renderH : L.renderW;
        const int h = sideways ? L.renderW : L.renderH;
        return g.top < vp.bottom && g.top + h > vp.top
            && g.left < vp.right && g.left + w > vp.left;
    }

    // 원래 글자 경계 (화면 좌표)
    RECT GlyphRect(const TextObj& o, const PageGeo& g)
    {
        int x1, y1, x2, y2;
        PdfRectToScreen(o.l, o.t, o.r, o.b, g.pdfW, g.pdfH, g.left, g.top,
            g_zoom, g.rot, &x1, &y1, &x2, &y2);
        return { x1, y1, x2, y2 };
    }

    // 점선 상자 / 클릭 영역 (글자 경계에서 2px 바깥)
    RECT BoxRect(const TextObj& o, const PageGeo& g)
    {
        RECT r = GlyphRect(o, g);
        InflateRect(&r, 2, 2);
        return r;
    }

    double EmPixels(const TextObj& o)
    {
        // 가로로 선 텍스트이므로 세로 배율은 m.d
        return std::max(1.0, (double)o.fontSize * o.m.d * g_zoom);
    }

    PendingEdit* FindEdit(DocState& ds, int page, int objIndex)
    {
        for (auto& e : ds.edits)
            if (e.page == page && e.objIndex == objIndex) return &e;
        return nullptr;
    }

    // 오브젝트의 글자를 text로 정한다 (빈 문자열 = 삭제, 원래 글자 = 편집 취소)
    void SetObjText(DocState& ds, int page, const TextObj& o, const std::wstring& text)
    {
        PendingEdit* e = FindEdit(ds, page, o.objIndex);
        const std::wstring& before = e ? e->text : o.text;
        if (text == before) return;
        if (text == o.text)
            ds.edits.erase(ds.edits.begin() + (e - ds.edits.data())); // 원래대로 → 대기 목록에서 뺀다
        else if (e)
            e->text = text;
        else
            ds.edits.push_back({ page, o.objIndex, o.text, text, o.bold, o.decos });
        g_dirty = true;
    }

    COLORREF Invert(COLORREF c)
    {
        return RGB(255 - GetRValue(c), 255 - GetGValue(c), 255 - GetBValue(c));
    }

    // 고친 글자 미리보기: 원래 글자를 바탕색으로 덮고 새 글자를 같은 기준선에 그린다.
    // 미리보기와 새 글자가 차지하는 영역을 돌려준다.
    RECT DrawPreview(HDC hdc, const TextObj& o, const std::wstring& text, const PageGeo& g)
    {
        RECT cover = GlyphRect(o, g);
        InflateRect(&cover, 2, 2); // 안티앨리어싱으로 번진 가장자리까지 덮는다
        HBRUSH bg = CreateSolidBrush(g_darkMode ? RGB(0, 0, 0) : RGB(255, 255, 255));
        FillRect(hdc, &cover, bg);
        // 딸린 밑줄/취소선도 덮는다
        for (const Deco& d : o.decos)
        {
            int x1, y1, x2, y2;
            PdfRectToScreen(d.l, d.t, d.r, d.b, g.pdfW, g.pdfH, g.left, g.top, g_zoom, g.rot, &x1, &y1, &x2, &y2);
            RECT dr = { x1, y1, x2, y2 };
            InflateRect(&dr, 2, 2);
            FillRect(hdc, &dr, bg);
            UnionRect(&cover, &cover, &dr);
        }
        DeleteObject(bg);
        if (text.empty()) return cover;

        // 화면 회전(시계 방향 90° 단위)에 맞춰 글자 방향을 돌린다 (lfEscapement는 반시계 방향)
        const int esc = ((4 - g.rot) % 4) * 900;
        HFONT font = CreateFontW(-(int)std::lround(EmPixels(o)), 0, esc, esc, o.bold ? FW_BOLD : FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS | CLIP_LH_ANGLES, ANTIALIASED_QUALITY, 0,
            IsAsciiText(text) ? L"Arial" : L"Malgun Gothic");
        HFONT oldFont = (HFONT)SelectObject(hdc, font);

        double ox, oy;
        PdfToPageOffset(o.m.e, o.m.f, g.pdfW, g.pdfH, g_zoom, g.rot, &ox, &oy);
        const int x = g.left + (int)std::lround(ox);
        const int y = g.top  + (int)std::lround(oy);

        const UINT oldAlign = SetTextAlign(hdc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
        const int  oldBk    = SetBkMode(hdc, TRANSPARENT);
        const COLORREF oldClr = SetTextColor(hdc, g_darkMode ? Invert(o.color) : o.color);
        TextOutW(hdc, x, y, text.c_str(), (int)text.size());

        // 새 글자가 차지하는 영역: 진행 방향으로 폭, 위쪽 방향으로 ascent, 아래로 descent
        SIZE sz = {};
        GetTextExtentPoint32W(hdc, text.c_str(), (int)text.size(), &sz);
        TEXTMETRICW tm = {};
        GetTextMetricsW(hdc, &tm);
        static const int advX[4] = { 1, 0, -1, 0 }, advY[4] = { 0, 1, 0, -1 };
        static const int upX[4]  = { 0, 1, 0, -1 }, upY[4]  = { -1, 0, 1, 0 };
        const int rr = g.rot & 3;
        int xs[4], ys[4], k = 0;
        for (int w : { 0, (int)sz.cx })
            for (int u : { -(int)tm.tmDescent, (int)tm.tmAscent })
            {
                xs[k] = x + advX[rr] * w + upX[rr] * u;
                ys[k] = y + advY[rr] * w + upY[rr] * u;
                ++k;
            }
        RECT textRect = { *std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4),
                          *std::max_element(xs, xs + 4), *std::max_element(ys, ys + 4) };

        // 밑줄/취소선을 새 글자 길이에 맞춰 다시 그린다 (원래 선이 글자 끝을 넘던 만큼은 유지)
        const float newRight = o.m.e + (float)(sz.cx / g_zoom);
        for (const Deco& d : o.decos)
        {
            const float r = newRight + (d.r - o.r);
            if (r <= d.l) continue;
            int x1, y1, x2, y2;
            PdfRectToScreen(d.l, d.t, r, d.b, g.pdfW, g.pdfH, g.left, g.top, g_zoom, g.rot, &x1, &y1, &x2, &y2);
            RECT dr = { x1, y1, std::max(x2, x1 + 1), std::max(y2, y1 + 1) };
            HBRUSH lineBr = CreateSolidBrush(g_darkMode ? Invert(d.color) : d.color);
            FillRect(hdc, &dr, lineBr);
            DeleteObject(lineBr);
            UnionRect(&textRect, &textRect, &dr);
        }

        SetTextColor(hdc, oldClr);
        SetBkMode(hdc, oldBk);
        SetTextAlign(hdc, oldAlign);
        SelectObject(hdc, oldFont);
        DeleteObject(font);

        RECT u;
        UnionRect(&u, &cover, &textRect);
        return u;
    }

    void DrawBox(HDC hdc, const RECT& r, bool hovered, bool edited)
    {
        COLORREF clr = edited
            ? (g_darkMode ? RGB(255, 170, 80) : RGB(230, 120, 0))
            : (g_darkMode ? RGB(100, 165, 255) : RGB(0, 120, 215));
        HPEN pen = hovered ? CreatePen(PS_SOLID, 2, clr) : CreatePen(PS_DOT, 1, clr);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        const int oldBk = SetBkMode(hdc, TRANSPARENT); // 점선 사이를 비워 둔다
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SetBkMode(hdc, oldBk);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    // (x, y) 아래의 상자 — 겹치면 가장 작은 상자를 고른다
    bool HitTestObj(int x, int y, int* outPage, int* outObj)
    {
        if (!g_pdfDoc || !g_hMainWnd) return false;
        RECT cr; GetClientRect(g_hMainWnd, &cr);
        const RECT vp = GetViewportRect(cr);
        const POINT pt = { x, y };
        if (!PtInRect(&vp, pt)) return false;

        long bestArea = LONG_MAX;
        int bestPage = -1, bestObj = -1;
        for (int pi = 0; pi < g_pageCount; ++pi)
        {
            PageGeo g;
            if (!GetPageGeo(pi, vp, g) || !IsPageVisible(pi, vp, g)) continue;
            PageObjs* po = EnsurePage(pi);
            if (!po) continue;
            for (int k = 0; k < (int)po->objs.size(); ++k)
            {
                RECT r = BoxRect(po->objs[k], g);
                if (!PtInRect(&r, pt)) continue;
                long area = (long)(r.right - r.left) * (r.bottom - r.top);
                if (area < bestArea) { bestArea = area; bestPage = pi; bestObj = k; }
            }
        }
        if (bestPage < 0) return false;
        *outPage = bestPage;
        *outObj  = bestObj;
        return true;
    }

    void RedrawMain()
    {
        if (s_hMain) InvalidateRect(s_hMain, nullptr, FALSE);
    }

    void FinishEdit(bool keep)
    {
        if (!s_hEdit || s_finishing) return;
        s_finishing = true;

        HWND hEdit = s_hEdit;
        s_hEdit = nullptr;

        if (keep)
        {
            auto it = s_docs.find(s_editDoc);
            if (it != s_docs.end() && s_editPage >= 0 && s_editPage < (int)it->second.pages.size())
            {
                DocState& ds = it->second;
                const auto& objs = ds.pages[s_editPage].objs;
                if (s_editObj >= 0 && s_editObj < (int)objs.size())
                {
                    const TextObj& o = objs[s_editObj];
                    const int len = GetWindowTextLengthW(hEdit);
                    std::wstring text(len, L'\0');
                    if (len > 0) GetWindowTextW(hEdit, &text[0], len + 1);

                    SetObjText(ds, s_editPage, o, text);
                }
            }
        }

        DestroyWindow(hEdit);
        if (s_editFont) { DeleteObject(s_editFont); s_editFont = nullptr; }
        s_editDoc  = nullptr;
        s_editPage = -1;
        s_editObj  = -1;
        s_finishing = false;
        if (s_hMain) SetFocus(s_hMain);
        RedrawMain();
    }

    LRESULT CALLBACK EditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        WNDPROC orig = (WNDPROC)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        switch (msg)
        {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) { FinishEdit(true);  return 0; }
            if (wParam == VK_ESCAPE) { FinishEdit(false); return 0; }
            break;
        case WM_CHAR:
            // Enter/Esc의 경고음 방지
            if (wParam == L'\r' || wParam == 27) return 0;
            break;
        case WM_KILLFOCUS:
            FinishEdit(true);
            return 0;
        }
        return CallWindowProcW(orig, hWnd, msg, wParam, lParam);
    }

    // 입력창 위치: 상자 왼쪽에 맞추고 세로 가운데 정렬
    POINT EditPos(const RECT& box, int editH)
    {
        return { box.left, box.top + ((box.bottom - box.top) - editH) / 2 };
    }

    void StartEdit(HWND hWnd, int page, int objIdx)
    {
        PageObjs* po = EnsurePage(page);
        if (!po || objIdx < 0 || objIdx >= (int)po->objs.size()) return;
        const TextObj& o = po->objs[objIdx];

        RECT cr; GetClientRect(hWnd, &cr);
        const RECT vp = GetViewportRect(cr);
        PageGeo g;
        if (!GetPageGeo(page, vp, g)) return;
        const RECT box = BoxRect(o, g);

        DocState& ds = s_docs[g_pdfDoc];
        PendingEdit* e = FindEdit(ds, page, o.objIndex);
        const std::wstring& current = e ? e->text : o.text;

        const int fontPx = (int)std::clamp(EmPixels(o), 14.0, 40.0);
        const int editH  = fontPx + 12;
        const int editW  = std::max<int>((box.right - box.left) + 80, 200);
        const POINT pos  = EditPos(box, editH);

        s_hMain    = hWnd;
        s_editDoc  = g_pdfDoc;
        s_editPage = page;
        s_editObj  = objIdx;
        s_hEdit = CreateWindowExW(0, L"EDIT", current.c_str(),
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            pos.x, pos.y, editW, editH, hWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!s_hEdit) { s_editDoc = nullptr; s_editPage = s_editObj = -1; return; }

        s_editFont = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
        SendMessageW(s_hEdit, WM_SETFONT, (WPARAM)s_editFont, TRUE);
        SendMessageW(s_hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));

        WNDPROC orig = (WNDPROC)SetWindowLongPtrW(s_hEdit, GWLP_WNDPROC, (LONG_PTR)EditProc);
        SetWindowLongPtrW(s_hEdit, GWLP_USERDATA, (LONG_PTR)orig);

        SetFocus(s_hEdit);
        SendMessageW(s_hEdit, EM_SETSEL, 0, -1);
        RedrawMain();
    }

    // 스크롤/확대에 맞춰 입력창을 따라 움직이고, 뷰포트를 벗어나면 숨긴다
    void UpdateEditPosition(const RECT& vp)
    {
        if (!s_hEdit) return;
        bool show = false;
        if (s_editDoc == g_pdfDoc)
        {
            auto it = s_docs.find(s_editDoc);
            PageGeo g;
            if (it != s_docs.end() && s_editPage < (int)it->second.pages.size()
                && GetPageGeo(s_editPage, vp, g))
            {
                const auto& objs = it->second.pages[s_editPage].objs;
                if (s_editObj >= 0 && s_editObj < (int)objs.size())
                {
                    RECT wr; GetWindowRect(s_hEdit, &wr);
                    const int editH = wr.bottom - wr.top;
                    const POINT pos = EditPos(BoxRect(objs[s_editObj], g), editH);
                    POINT cur = { wr.left, wr.top };
                    ScreenToClient(GetParent(s_hEdit), &cur);
                    if (cur.x != pos.x || cur.y != pos.y)
                        SetWindowPos(s_hEdit, nullptr, pos.x, pos.y, 0, 0,
                            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    show = pos.y >= vp.top && pos.y + editH <= vp.bottom;
                }
            }
        }
        const bool visible = IsWindowVisible(s_hEdit) != FALSE;
        if (show != visible)
            ShowWindow(s_hEdit, show ? SW_SHOWNA : SW_HIDE);
    }

    void* __cdecl FsAlloc(size_t n)            { return malloc(n); }
    void* __cdecl FsRealloc(void* p, size_t n) { return realloc(p, n); }
    void  __cdecl FsFree(void* p)              { free(p); }

    bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& out)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size = {};
        bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1LL << 30);
        if (ok)
        {
            out.resize((size_t)size.QuadPart);
            DWORD read = 0;
            ok = ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr) && read == out.size();
        }
        CloseHandle(h);
        return ok;
    }

    // 맑은 고딕(굵게면 맑은 고딕 Bold)을 chars에 든 글자만 남긴 서브셋으로 doc에 불러온다.
    // 서브셋을 못 만들면 글꼴 전체를 넣는다 (파일이 커지지만 글자는 보인다).
    FPDF_FONT LoadKoreanFont(FPDF_DOCUMENT doc, const std::set<wchar_t>& chars, bool bold)
    {
        wchar_t winDir[MAX_PATH] = {};
        UINT wl = GetWindowsDirectoryW(winDir, MAX_PATH);
        if (wl == 0 || wl >= MAX_PATH) wcscpy_s(winDir, L"C:\\Windows");
        const std::wstring fontDir = std::wstring(winDir) + L"\\Fonts\\";
        std::vector<unsigned char> full;
        if (!(bold && ReadFileBytes(fontDir + L"malgunbd.ttf", full))
            && !ReadFileBytes(fontDir + L"malgun.ttf", full))
            return nullptr;

        std::vector<unsigned short> keep(chars.begin(), chars.end());
        keep.push_back(L' ');
        unsigned char* sub = nullptr;
        unsigned long subSize = 0, written = 0;
        unsigned long rc = CreateFontPackage(full.data(), (unsigned long)full.size(),
            &sub, &subSize, &written,
            TTFCFP_FLAGS_SUBSET, 0, TTFCFP_SUBSET, 0,
            TTFCFP_MS_PLATFORMID, TTFCFP_UNICODE_CHAR_SET,
            keep.data(), (unsigned short)keep.size(),
            FsAlloc, FsRealloc, FsFree, nullptr);
        if (rc == 0 && sub && written > 0)
            s_fontKeepAlive.emplace_back(sub, sub + written);
        else
            s_fontKeepAlive.push_back(std::move(full));
        if (sub) FsFree(sub);

        const auto& data = s_fontKeepAlive.back();
        return FPDFText_LoadFont(doc, data.data(), (uint32_t)data.size(),
            FPDF_FONT_TRUETYPE, /*cid=*/1);
    }
}

namespace TextEdit
{
    bool IsActive() { return s_active; }

    void SetActive(HWND hWnd, bool active)
    {
        if (!active) FinishEdit(true);
        s_active    = active;
        s_hMain     = hWnd;
        s_hoverPage = s_hoverObj = -1;
        RedrawMain();
    }

    void ForgetDocument(FPDF_DOCUMENT doc)
    {
        if (!doc) return;
        if (s_hEdit && s_editDoc == doc) FinishEdit(false);
        s_docs.erase(doc);
        s_hoverPage = s_hoverObj = -1;
    }

    void Draw(HDC hdc, const RECT& clientRect)
    {
        if (!g_pdfDoc || g_pageLayouts.empty()) return;
        auto it = s_docs.find(g_pdfDoc);
        const bool hasEdits = (it != s_docs.end() && !it->second.edits.empty());
        const RECT vp = GetViewportRect(clientRect);
        if (!s_active && !hasEdits) { UpdateEditPosition(vp); return; }

        const int saved = SaveDC(hdc);
        IntersectClipRect(hdc, vp.left, vp.top, vp.right, vp.bottom);

        for (int pi = 0; pi < g_pageCount; ++pi)
        {
            PageGeo g;
            if (!GetPageGeo(pi, vp, g) || !IsPageVisible(pi, vp, g)) continue;

            bool pageHasEdit = false;
            if (hasEdits)
                for (const auto& e : it->second.edits)
                    if (e.page == pi) { pageHasEdit = true; break; }
            if (!s_active && !pageHasEdit) continue;

            PageObjs* po = EnsurePage(pi);
            if (!po) continue;
            DocState& ds = s_docs[g_pdfDoc];

            for (int k = 0; k < (int)po->objs.size(); ++k)
            {
                const TextObj& o = po->objs[k];
                const bool editing = (s_hEdit && s_editDoc == g_pdfDoc && s_editPage == pi && s_editObj == k);
                PendingEdit* e = pageHasEdit ? FindEdit(ds, pi, o.objIndex) : nullptr;

                RECT box = BoxRect(o, g);
                if (e)
                {
                    RECT area = DrawPreview(hdc, o, e->text, g);
                    InflateRect(&area, 2, 2);
                    UnionRect(&box, &box, &area);
                }
                if (s_active && !editing)
                    DrawBox(hdc, box, pi == s_hoverPage && k == s_hoverObj, e != nullptr);
            }
        }

        RestoreDC(hdc, saved);
        UpdateEditPosition(vp);
    }

    bool HitTest(int x, int y)
    {
        int p, o;
        return s_active && HitTestObj(x, y, &p, &o);
    }

    void OnMouseMove(HWND hWnd, int x, int y)
    {
        if (!s_active) return;
        int p = -1, o = -1;
        if (!HitTestObj(x, y, &p, &o)) { p = -1; o = -1; }
        if (p != s_hoverPage || o != s_hoverObj)
        {
            s_hoverPage = p;
            s_hoverObj  = o;
            s_hMain = hWnd;
            RedrawMain();
        }
    }

    bool OnLButtonDown(HWND hWnd, int x, int y)
    {
        if (!s_active || !g_pdfDoc) return false;
        RECT cr; GetClientRect(hWnd, &cr);
        const RECT vp = GetViewportRect(cr);
        const POINT pt = { x, y };
        if (!PtInRect(&vp, pt)) return false;

        FinishEdit(true);
        int p, o;
        if (HitTestObj(x, y, &p, &o))
            StartEdit(hWnd, p, o);
        return true;
    }

    bool OnContextMenu(HWND hWnd, int x, int y, POINT ptScreen)
    {
        if (!s_active || !g_pdfDoc) return false;
        FinishEdit(true);
        int page, k;
        if (!HitTestObj(x, y, &page, &k)) return false; // 빈 곳은 기본 메뉴

        DocState& ds = s_docs[g_pdfDoc];
        const TextObj o = ds.pages[page].objs[k]; // 메뉴가 떠 있는 동안 벡터가 바뀌어도 안전하도록 복사
        const PendingEdit* e = FindEdit(ds, page, o.objIndex);
        const bool deleted = e && e->text.empty();

        enum { CMD_EDIT = 1, CMD_DELETE, CMD_RESTORE };
        HMENU menu = CreatePopupMenu();
        if (!menu) return true;
        AppendMenuW(menu, MF_STRING | (deleted ? MF_GRAYED : 0), CMD_EDIT, LS(STR_CTX_TEXT_EDIT));
        AppendMenuW(menu, MF_STRING | (deleted ? MF_GRAYED : 0), CMD_DELETE, LS(STR_CTX_TEXT_DELETE));
        if (e)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, CMD_RESTORE, LS(STR_CTX_TEXT_RESTORE));
        }
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            ptScreen.x, ptScreen.y, 0, hWnd, nullptr);
        DestroyMenu(menu);

        s_hMain = hWnd;
        if (cmd == CMD_EDIT)
            StartEdit(hWnd, page, k);
        else if (cmd == CMD_DELETE)
            SetObjText(ds, page, o, L"");
        else if (cmd == CMD_RESTORE)
            SetObjText(ds, page, o, o.text);
        RedrawMain();
        return true;
    }

    void CommitEdit() { FinishEdit(true); }

    bool IsEditFocused() { return s_hEdit && GetFocus() == s_hEdit; }

    bool HasPendingEdits(FPDF_DOCUMENT doc)
    {
        auto it = s_docs.find(doc);
        return it != s_docs.end() && !it->second.edits.empty();
    }

    int ApplyPendingEdits(FPDF_DOCUMENT doc)
    {
        FinishEdit(true);
        auto it = s_docs.find(doc);
        if (!doc || it == s_docs.end() || it->second.edits.empty()) return 0;

        std::vector<PendingEdit> edits = std::move(it->second.edits);
        s_docs.erase(it);
        s_hoverPage = s_hoverObj = -1;

        // 필요한 글꼴만 준비한다 — [0] 보통, [1] 굵게
        std::set<wchar_t> koChars[2];
        bool needStd[2] = { false, false };
        for (const auto& e : edits)
        {
            if (e.text.empty()) continue;
            const int bi = e.bold ? 1 : 0;
            if (IsAsciiText(e.text)) needStd[bi] = true;
            else koChars[bi].insert(e.text.begin(), e.text.end());
        }

        std::lock_guard<std::mutex> lk(g_pdfiumMtx);
        s_fontKeepAlive.clear(); // 이전 저장의 문서는 이미 닫혔다
        FPDF_FONT fontStd[2] = {}, fontKo[2] = {};
        for (int bi = 0; bi < 2; ++bi)
        {
            if (needStd[bi])
                fontStd[bi] = FPDFText_LoadStandardFont(doc, bi ? "Helvetica-Bold" : "Helvetica");
            if (!koChars[bi].empty())
                fontKo[bi] = LoadKoreanFont(doc, koChars[bi], bi == 1);
        }

        std::map<int, std::vector<const PendingEdit*>> byPage;
        for (const auto& e : edits) byPage[e.page].push_back(&e);

        int applied = 0;
        for (auto& [pi, list] : byPage)
        {
            FPDF_PAGE pg = FPDF_LoadPage(doc, pi);
            if (!pg) continue;

            // 뒤쪽 오브젝트부터 바꿔야 앞쪽 인덱스가 그대로 유지된다
            std::sort(list.begin(), list.end(),
                [](const PendingEdit* a, const PendingEdit* b) { return a->objIndex > b->objIndex; });

            // 먼저 모든 대상이 읽었을 때와 같은 오브젝트인지 확인한다 (텍스트 페이지는 수정 전에 닫는다)
            struct Job
            {
                FPDF_PAGEOBJECT obj;
                const PendingEdit* e;
                std::vector<std::pair<FPDF_PAGEOBJECT, const Deco*>> decos;
            };
            std::vector<Job> jobs;
            if (FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg))
            {
                for (const PendingEdit* e : list)
                {
                    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(pg, e->objIndex);
                    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT
                        || ReadObjText(obj, tp) != e->original)
                        continue;
                    Job j{ obj, e, {} };
                    // 밑줄/취소선도 읽었을 때와 같은 선인지 경계로 확인한다
                    for (const Deco& d : e->decos)
                    {
                        FPDF_PAGEOBJECT lo = FPDFPage_GetObject(pg, d.objIndex);
                        float l, b, r, t;
                        if (lo && FPDFPageObj_GetType(lo) == FPDF_PAGEOBJ_PATH
                            && FPDFPageObj_GetBounds(lo, &l, &b, &r, &t)
                            && std::fabs(l - d.l) < 0.01f && std::fabs(r - d.r) < 0.01f
                            && std::fabs(b - d.b) < 0.01f && std::fabs(t - d.t) < 0.01f)
                            j.decos.push_back({ lo, &d });
                    }
                    jobs.push_back(std::move(j));
                }
                FPDFText_ClosePage(tp);
            }

            // 지금 위치(그리는 순서)에 새 오브젝트를 넣는다. 앞서 지운 오브젝트 때문에
            // 인덱스가 바뀌었을 수 있으므로 기존 오브젝트의 현재 인덱스를 다시 찾는다.
            auto insertAt = [&](FPDF_PAGEOBJECT newObj, FPDF_PAGEOBJECT oldObj)
            {
                const int cnt = FPDFPage_CountObjects(pg);
                for (int i = 0; i < cnt; ++i)
                    if (FPDFPage_GetObject(pg, i) == oldObj
                        && FPDFPage_InsertObjectAtIndex(pg, newObj, (size_t)i))
                        return;
                FPDFPage_InsertObject(pg, newObj);
            };

            for (const Job& j : jobs)
            {
                const std::wstring& text = j.e->text;
                float newRight = 0;
                if (!text.empty())
                {
                    const int bi = j.e->bold ? 1 : 0;
                    FPDF_FONT font = IsAsciiText(text)
                        ? (fontStd[bi] ? fontStd[bi] : fontKo[bi]) : fontKo[bi];
                    if (!font) continue; // 글꼴이 없으면 원래 글자를 그대로 둔다

                    FS_MATRIX m{};
                    float fontSize = 0;
                    unsigned int R = 0, G = 0, B = 0, A = 255;
                    FPDFPageObj_GetMatrix(j.obj, &m);
                    FPDFTextObj_GetFontSize(j.obj, &fontSize);
                    FPDFPageObj_GetFillColor(j.obj, &R, &G, &B, &A);

                    FPDF_PAGEOBJECT n = FPDFPageObj_CreateTextObj(doc, font, fontSize);
                    if (!n) continue;
                    FPDFText_SetText(n, reinterpret_cast<FPDF_WIDESTRING>(text.c_str()));
                    FPDFPageObj_SetFillColor(n, R, G, B, A);
                    FPDFPageObj_SetMatrix(n, &m);
                    float nl, nb, nt;
                    if (!FPDFPageObj_GetBounds(n, &nl, &nb, &newRight, &nt)) newRight = m.e;
                    insertAt(n, j.obj);
                }

                // 밑줄/취소선: 같은 높이·색으로 새 글자 길이에 맞춘 선을 넣고 원래 선은 지운다
                // (글자를 지운 경우에는 선도 지운다)
                float ol, ob, oldRight, ot;
                if (!FPDFPageObj_GetBounds(j.obj, &ol, &ob, &oldRight, &ot)) oldRight = 0;
                for (const auto& [lineObj, d] : j.decos)
                {
                    const float r = newRight + (d->r - oldRight);
                    if (!text.empty() && r > d->l)
                    {
                        FPDF_PAGEOBJECT rect = FPDFPageObj_CreateNewRect(d->l, d->b, r - d->l, d->t - d->b);
                        if (rect)
                        {
                            FPDFPageObj_SetFillColor(rect, GetRValue(d->color), GetGValue(d->color), GetBValue(d->color), 255);
                            FPDFPath_SetDrawMode(rect, FPDF_FILLMODE_WINDING, FALSE);
                            insertAt(rect, lineObj);
                        }
                    }
                    FPDFPage_RemoveObject(pg, lineObj);
                    FPDFPageObj_Destroy(lineObj);
                }

                FPDFPage_RemoveObject(pg, j.obj);
                FPDFPageObj_Destroy(j.obj);
                ++applied;
            }

            if (!jobs.empty()) FPDFPage_GenerateContent(pg);
            FPDF_ClosePage(pg);
        }

        for (int bi = 0; bi < 2; ++bi)
        {
            if (fontStd[bi]) FPDFFont_Close(fontStd[bi]);
            if (fontKo[bi])  FPDFFont_Close(fontKo[bi]);
        }
        return applied;
    }
}
