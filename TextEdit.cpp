// TextEdit.cpp : 텍스트 편집 모드 (점선 상자 표시, 클릭해서 고치기, 저장 시 반영)
//
// 다른 편집 기능(가림, 텍스트 박스 등)과 같은 방식으로 동작한다.
//  - 고친 내용은 문서별 대기 목록에만 들고 있고, 화면에는 오버레이로 미리 보여 준다.
//    (렌더 워커는 별도 문서 사본으로 그리므로 g_pdfDoc을 바로 바꾸면 화면에 반영되지 않는다)
//  - 저장할 때 ApplyPendingEdits()가 원래 텍스트 오브젝트를 새 오브젝트로 바꿔 넣는다.
//
// 글꼴: PDF에 들어 있는 글꼴은 대개 문서에 쓰인 글자만 담은 일부분이라 새 글자를 그릴 수 없다.
// 그래서 고친 오브젝트는 시스템 글꼴로 다시 만든다.
//  - "자동": 영문/숫자/기호(ASCII)만 있으면 표준 글꼴 Helvetica (파일에 넣지 않음, 미리보기는 Arial),
//            그 밖의 글자가 있으면 맑은 고딕
//  - 사용자가 고른 글꼴: 그 글꼴. 단, 입력한 글자 중 그 글꼴에 없는 글자가 있으면 맑은 고딕
//  - 넣는 글꼴은 쓰인 글자만 남긴 서브셋으로 만든다 (CreateFontPackage)

#include "framework.h"
#include "PlainPDFViewer.h"
#include "TextEdit.h"
#include "Lang.h"
#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fontsub.h>
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
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

    // 사용자가 고른 모양
    struct EditStyle
    {
        std::wstring face;      // 빈 문자열 = 자동
        float        size = 0;  // 포인트, 0 = 원래 크기
        bool         bold = false;
    };

    bool SameStyle(const EditStyle& a, const EditStyle& b)
    {
        return a.face == b.face && std::fabs(a.size - b.size) < 0.01f && a.bold == b.bold;
    }

    // 실제로 쓸 글꼴 (미리보기와 저장이 같은 결과를 쓰도록 편집 확정 때 정해 둔다)
    struct Resolved
    {
        bool         standard = false; // true = 표준 글꼴 Helvetica (미리보기는 Arial)
        std::wstring face;             // GDI 글꼴 이름
        bool         bold = false;
    };

    struct PendingEdit
    {
        int          page = -1;
        int          objIndex = -1;
        std::wstring original;   // 저장할 때 같은 오브젝트인지 확인하는 데 쓴다
        std::wstring text;
        EditStyle    style;
        Resolved     res;
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

    const wchar_t* const kFallbackFace = L"Malgun Gothic";

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
    float         s_editOrigSize = 0;   // 원래 글자 크기 (포인트)
    bool          s_finishing  = false;

    // 입력창 위 서식 막대
    HWND  s_hBar       = nullptr;
    HWND  s_hFontCombo = nullptr;
    HWND  s_hSizeCombo = nullptr;
    HWND  s_hBoldBtn   = nullptr;
    HWND  s_hWarn      = nullptr;
    HFONT s_uiFont     = nullptr;
    HFONT s_uiBoldFont = nullptr;

    const UINT WM_TE_CHECKFOCUS = WM_APP + 0x351;
    const int  kBarPad = 4, kRowH = 26, kWarnH = 18;

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

    // ── 시스템 글꼴 도우미 (GDI) ────────────────────────────────────────────

    const DWORD TAG_TTCF = 0x66637474; // 'ttcf'
    const DWORD TAG_OS2  = 0x322F534F; // 'OS/2'
    const DWORD TAG_GLYF = 0x66796C67; // 'glyf'

    unsigned BE16(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
    unsigned BE32(const unsigned char* p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

    // 글꼴 하나를 메모리 DC에 골라 둔 상태 (GetFontData 등을 쓰기 위함)
    struct FontDC
    {
        HDC dc = nullptr; HFONT font = nullptr; HGDIOBJ old = nullptr;
        FontDC(const std::wstring& face, bool bold, int height = -64)
        {
            dc = CreateCompatibleDC(nullptr);
            LOGFONTW lf = {};
            lf.lfHeight = height;
            lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
            wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
            font = CreateFontIndirectW(&lf);
            if (dc && font) old = SelectObject(dc, font);
        }
        ~FontDC()
        {
            if (dc && old) SelectObject(dc, old);
            if (font) DeleteObject(font);
            if (dc) DeleteDC(dc);
        }
        bool ok() const { return dc && font; }
    };

    // OS/2 표의 값 (없으면 -1)
    int ReadOS2U16(HDC dc, DWORD offset)
    {
        unsigned char b[2];
        if (GetFontData(dc, TAG_OS2, offset, b, 2) != 2) return -1;
        return (int)BE16(b);
    }

    // PDF에 넣어도 되는 글꼴인지 (fsType: 2 = 제한된 라이선스, 0x200 = 비트맵만 허용)
    bool EmbeddingAllowed(HDC dc)
    {
        const int fsType = ReadOS2U16(dc, 8);
        if (fsType < 0) return true;
        if (fsType & 0x0200) return false;
        if ((fsType & 0x000E) == 0x0002) return false;
        return true;
    }

    // 진짜 굵은 꼴이 설치돼 있는지 (없으면 GDI가 흉내 낸 굵기라 PDF에 넣을 수 없다)
    bool FaceHasRealBold(const std::wstring& face)
    {
        static std::map<std::wstring, bool> cache;
        auto it = cache.find(face);
        if (it != cache.end()) return it->second;
        FontDC f(face, true);
        const bool yes = f.ok() && ReadOS2U16(f.dc, 4) >= 600; // usWeightClass
        cache[face] = yes;
        return yes;
    }

    // text의 글자가 모두 그 글꼴에 있는지
    bool FaceHasGlyphs(const std::wstring& face, bool bold, const std::wstring& text)
    {
        if (text.empty()) return true;
        FontDC f(face, bold);
        if (!f.ok()) return false;
        std::vector<WORD> idx(text.size());
        if (GetGlyphIndicesW(f.dc, text.c_str(), (int)text.size(), idx.data(),
                GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR)
            return false;
        for (size_t i = 0; i < text.size(); ++i)
            if (idx[i] == 0xFFFF && !iswspace(text[i])) return false;
        return true;
    }

    int CALLBACK EnumFontProc(const LOGFONTW* lf, const TEXTMETRICW*, DWORD type, LPARAM lp)
    {
        if ((type & TRUETYPE_FONTTYPE) && lf->lfFaceName[0] && lf->lfFaceName[0] != L'@')
            reinterpret_cast<std::set<std::wstring>*>(lp)->insert(lf->lfFaceName);
        return 1;
    }

    // 고를 수 있는 설치된 글꼴 (처음 한 번만 만든다).
    // TrueType 윤곽선(glyf)이 있고 PDF에 넣는 것이 허용된 글꼴만 남긴다.
    const std::vector<std::wstring>& InstalledFonts()
    {
        static std::vector<std::wstring> list;
        static bool built = false;
        if (built) return list;
        built = true;

        std::set<std::wstring> names;
        HDC dc = CreateCompatibleDC(nullptr);
        LOGFONTW lf = {};
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &lf, EnumFontProc, (LPARAM)&names, 0);
        DeleteDC(dc);

        for (const auto& n : names)
        {
            FontDC f(n, false);
            if (!f.ok()) continue;
            if (GetFontData(f.dc, TAG_GLYF, 0, nullptr, 0) == GDI_ERROR) continue;
            if (!EmbeddingAllowed(f.dc)) continue;
            list.push_back(n);
        }
        std::sort(list.begin(), list.end(), [](const std::wstring& a, const std::wstring& b) {
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE,
                a.c_str(), -1, b.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
        });
        return list;
    }

    Resolved Resolve(const std::wstring& text, const EditStyle& st)
    {
        Resolved r;
        if (st.face.empty())
        {
            if (IsAsciiText(text)) { r.standard = true; r.face = L"Arial"; }
            else r.face = kFallbackFace;
            r.bold = st.bold;
            return r;
        }
        if (!FaceHasGlyphs(st.face, st.bold, text))
        {
            r.face = kFallbackFace;
            r.bold = st.bold;
            return r;
        }
        r.face = st.face;
        r.bold = st.bold && FaceHasRealBold(st.face);
        return r;
    }

    // ── 페이지 오브젝트 ──────────────────────────────────────────────────────

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

    // 원래 글자 크기 (포인트). 가로로 선 텍스트이므로 세로 배율은 m.d
    float OrigSizePt(const TextObj& o) { return o.fontSize * o.m.d; }

    float EffSizePt(const TextObj& o, const EditStyle& st)
    {
        return st.size > 0 ? st.size : OrigSizePt(o);
    }

    EditStyle DefaultStyle(const TextObj& o)
    {
        EditStyle st;
        st.bold = o.bold;
        return st;
    }

    PendingEdit* FindEdit(DocState& ds, int page, int objIndex)
    {
        for (auto& e : ds.edits)
            if (e.page == page && e.objIndex == objIndex) return &e;
        return nullptr;
    }

    // 오브젝트의 글자와 모양을 정한다 (빈 문자열 = 삭제, 원래 글자+기본 모양 = 편집 취소)
    void SetObjEdit(DocState& ds, int page, const TextObj& o, const std::wstring& text, const EditStyle& style)
    {
        PendingEdit* e = FindEdit(ds, page, o.objIndex);
        if (text == o.text && SameStyle(style, DefaultStyle(o)))
        {
            if (e)
            {
                ds.edits.erase(ds.edits.begin() + (e - ds.edits.data())); // 원래대로 → 대기 목록에서 뺀다
                g_dirty = true;
            }
            return;
        }
        if (e && e->text == text && SameStyle(e->style, style)) return;

        const Resolved res = Resolve(text, style);
        if (e) { e->text = text; e->style = style; e->res = res; }
        else ds.edits.push_back({ page, o.objIndex, o.text, text, style, res, o.decos });
        g_dirty = true;
    }

    COLORREF Invert(COLORREF c)
    {
        return RGB(255 - GetRValue(c), 255 - GetGValue(c), 255 - GetBValue(c));
    }

    // 고친 글자 미리보기: 원래 글자를 바탕색으로 덮고 새 글자를 같은 기준선에 그린다.
    // 미리보기와 새 글자가 차지하는 영역을 돌려준다.
    RECT DrawPreview(HDC hdc, const TextObj& o, const PendingEdit& e, const PageGeo& g)
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
        const std::wstring& text = e.text;
        if (text.empty()) return cover;

        // 화면 회전(시계 방향 90° 단위)에 맞춰 글자 방향을 돌린다 (lfEscapement는 반시계 방향)
        const int esc = ((4 - g.rot) % 4) * 900;
        const double emPx = std::max(1.0, (double)EffSizePt(o, e.style) * g_zoom);
        HFONT font = CreateFontW(-(int)std::lround(emPx), 0, esc, esc, e.res.bold ? FW_BOLD : FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS | CLIP_LH_ANGLES, ANTIALIASED_QUALITY, 0, e.res.face.c_str());
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

    // ── 입력창 + 서식 막대 ───────────────────────────────────────────────────

    const TextObj* EditingObj()
    {
        auto it = s_docs.find(s_editDoc);
        if (it == s_docs.end() || s_editPage < 0 || s_editPage >= (int)it->second.pages.size()) return nullptr;
        const auto& objs = it->second.pages[s_editPage].objs;
        if (s_editObj < 0 || s_editObj >= (int)objs.size()) return nullptr;
        return &objs[s_editObj];
    }

    bool InEditor(HWND h)
    {
        return h && s_hEdit && (h == s_hEdit || (s_hBar && (h == s_hBar || IsChild(s_hBar, h))));
    }

    std::wstring WindowText(HWND h)
    {
        const int len = GetWindowTextLengthW(h);
        std::wstring s(len, L'\0');
        if (len > 0) GetWindowTextW(h, &s[0], len + 1);
        return s;
    }

    std::wstring FormatSize(float pt)
    {
        wchar_t buf[32];
        swprintf_s(buf, L"%g", std::round(pt * 10.0f) / 10.0f);
        return buf;
    }

    std::wstring SelectedFace()
    {
        const int sel = (int)SendMessageW(s_hFontCombo, CB_GETCURSEL, 0, 0);
        if (sel <= 0) return L"";
        const int len = (int)SendMessageW(s_hFontCombo, CB_GETLBTEXTLEN, sel, 0);
        std::wstring s(len > 0 ? len : 0, L'\0');
        if (len > 0) SendMessageW(s_hFontCombo, CB_GETLBTEXT, sel, (LPARAM)&s[0]);
        return s;
    }

    // 서식 막대의 현재 값. sizeText가 있으면 크기 칸 대신 그 값을 쓴다 (목록에서 막 고른 경우)
    EditStyle ReadEditorStyle(const std::wstring* sizeText = nullptr)
    {
        EditStyle st;
        st.face = SelectedFace();
        const std::wstring s = sizeText ? *sizeText : WindowText(s_hSizeCombo);
        float size = (float)_wtof(s.c_str());
        if (size > 0) size = std::clamp(size, 1.0f, 999.0f);
        if (size <= 0 || std::fabs(size - s_editOrigSize) < 0.05f) size = 0;
        st.size = size;
        st.bold = IsWindowEnabled(s_hBoldBtn)
            && SendMessageW(s_hBoldBtn, BM_GETCHECK, 0, 0) == BST_CHECKED;
        return st;
    }

    void LayoutEditor(const RECT& vp);

    // 고른 글꼴/크기/굵기를 입력창 글꼴과 경고 문구에 반영
    void ApplyEditorStyle(const EditStyle& st)
    {
        // 굵게: 자동이거나 진짜 굵은 꼴이 있는 글꼴만
        const bool boldOk = st.face.empty() || FaceHasRealBold(st.face);
        EnableWindow(s_hBoldBtn, boldOk);
        if (!boldOk) SendMessageW(s_hBoldBtn, BM_SETCHECK, BST_UNCHECKED, 0);

        const TextObj* o = EditingObj();
        const float pt = o ? EffSizePt(*o, st) : 12.0f;
        const int px = (int)std::clamp((double)pt * g_zoom, 14.0, 40.0);
        HFONT nf = CreateFontW(-px, 0, 0, 0, (st.bold && boldOk) ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
            st.face.empty() ? kFallbackFace : st.face.c_str());
        SendMessageW(s_hEdit, WM_SETFONT, (WPARAM)nf, TRUE);
        if (s_editFont) DeleteObject(s_editFont);
        s_editFont = nf;

        RECT er; GetWindowRect(s_hEdit, &er);
        SetWindowPos(s_hEdit, nullptr, 0, 0, er.right - er.left, px + 12,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        // 고른 글꼴에 없는 글자가 있으면 알린다
        const std::wstring text = WindowText(s_hEdit);
        const bool missing = !st.face.empty() && !FaceHasGlyphs(st.face, st.bold && boldOk, text);
        ShowWindow(s_hWarn, missing ? SW_SHOWNA : SW_HIDE);

        RECT cr; GetClientRect(s_hMain, &cr);
        LayoutEditor(GetViewportRect(cr));
    }

    void OnStyleChanged(const std::wstring* sizeText = nullptr)
    {
        if (s_hEdit) ApplyEditorStyle(ReadEditorStyle(sizeText));
    }

    // 글자를 입력하는 동안에는 '없는 글자' 경고만 갱신한다 (글꼴을 다시 만들면 깜빡인다)
    void UpdateGlyphWarning()
    {
        if (!s_hEdit || !s_hWarn) return;
        const EditStyle st = ReadEditorStyle();
        const bool missing = !st.face.empty() && !FaceHasGlyphs(st.face, st.bold, WindowText(s_hEdit));
        if ((IsWindowVisible(s_hWarn) != FALSE) == missing) return;
        ShowWindow(s_hWarn, missing ? SW_SHOWNA : SW_HIDE);
        RECT cr; GetClientRect(s_hMain, &cr);
        LayoutEditor(GetViewportRect(cr));
    }

    void FinishEdit(bool keep)
    {
        if (!s_hEdit || s_finishing) return;
        s_finishing = true;

        if (keep)
        {
            auto it = s_docs.find(s_editDoc);
            const TextObj* o = EditingObj();
            if (it != s_docs.end() && o)
            {
                const TextObj copy = *o;
                SetObjEdit(it->second, s_editPage, copy, WindowText(s_hEdit), ReadEditorStyle());
            }
        }

        HWND hEdit = s_hEdit, hBar = s_hBar;
        s_hEdit = s_hBar = s_hFontCombo = s_hSizeCombo = s_hBoldBtn = s_hWarn = nullptr;
        DestroyWindow(hEdit);
        if (hBar) DestroyWindow(hBar);
        if (s_editFont) { DeleteObject(s_editFont); s_editFont = nullptr; }
        s_editDoc  = nullptr;
        s_editPage = -1;
        s_editObj  = -1;
        s_finishing = false;
        if (s_hMain) SetFocus(s_hMain);
        RedrawMain();
    }

    // 포커스가 입력창/서식 막대 밖으로 나가면 확정한다.
    // 포커스는 컨트롤 사이를 옮겨 다니므로, 이동이 끝난 뒤(메시지 한 번 뒤)에 확인한다.
    void ScheduleFocusCheck()
    {
        if (s_hBar) PostMessageW(s_hBar, WM_TE_CHECKFOCUS, 0, 0);
    }

    // Tab 순서: 입력창 → 글꼴 → 크기 → 굵게
    void FocusNext(HWND from, bool back)
    {
        HWND order[4] = { s_hEdit, s_hFontCombo, s_hSizeCombo, s_hBoldBtn };
        int cur = 0;
        for (int i = 0; i < 4; ++i)
            if (order[i] == from || (order[i] && IsChild(order[i], from))) cur = i;
        for (int step = 1; step <= 4; ++step)
        {
            HWND h = order[(cur + (back ? 4 - step : step)) % 4];
            if (h && IsWindowEnabled(h)) { SetFocus(h); return; }
        }
    }

    bool ComboDropped(HWND h)
    {
        HWND combo = h;
        wchar_t cls[16] = {};
        GetClassNameW(h, cls, 16);
        if (_wcsicmp(cls, L"ComboBox") != 0) combo = GetParent(h);
        return SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0;
    }

    // 입력창과 서식 막대 컨트롤 공통 키 처리 (Enter=확정, Esc=취소, Tab=다음 칸)
    LRESULT CALLBACK ControlProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        WNDPROC orig = (WNDPROC)GetPropW(hWnd, L"PPV_TE_ORIG");
        switch (msg)
        {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN && !(hWnd != s_hEdit && ComboDropped(hWnd)))
            { FinishEdit(true); return 0; }
            if (wParam == VK_ESCAPE && !(hWnd != s_hEdit && ComboDropped(hWnd)))
            { FinishEdit(false); return 0; }
            if (wParam == VK_TAB)
            { FocusNext(hWnd, (GetKeyState(VK_SHIFT) & 0x8000) != 0); return 0; }
            break;
        case WM_CHAR:
            // Enter/Esc/Tab의 경고음 방지
            if (wParam == L'\r' || wParam == 27 || wParam == L'\t') return 0;
            break;
        case WM_KEYUP:
        case WM_PASTE:
        case WM_CUT:
        case WM_CLEAR:
        case WM_UNDO:
            if (hWnd == s_hEdit)
            {
                LRESULT r = CallWindowProcW(orig, hWnd, msg, wParam, lParam);
                UpdateGlyphWarning(); // 입력한 글자에 따라 '없는 글자' 경고를 갱신
                return r;
            }
            break;
        case WM_KILLFOCUS:
        {
            LRESULT r = CallWindowProcW(orig, hWnd, msg, wParam, lParam);
            ScheduleFocusCheck();
            return r;
        }
        case WM_NCDESTROY:
            RemovePropW(hWnd, L"PPV_TE_ORIG");
            break;
        }
        return CallWindowProcW(orig, hWnd, msg, wParam, lParam);
    }

    void Subclass(HWND h)
    {
        if (!h) return;
        SetPropW(h, L"PPV_TE_ORIG", (HANDLE)GetWindowLongPtrW(h, GWLP_WNDPROC));
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ControlProc);
    }

    HFONT SampleFont(const std::wstring& face)
    {
        static std::map<std::wstring, HFONT> cache;
        auto it = cache.find(face);
        if (it != cache.end()) return it->second;
        HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            0, 0, CLEARTYPE_QUALITY, 0, face.c_str());
        cache[face] = f;
        return f;
    }

    // 글꼴 목록 한 줄: 왼쪽에 이름, 오른쪽에 그 글꼴로 쓴 견본
    void DrawFontItem(const DRAWITEMSTRUCT* dis)
    {
        if (dis->itemID == (UINT)-1) return;
        const bool sel = (dis->itemState & ODS_SELECTED) != 0;
        HDC dc = dis->hDC;
        RECT rc = dis->rcItem;
        FillRect(dc, &rc, GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));

        const int len = (int)SendMessageW(dis->hwndItem, CB_GETLBTEXTLEN, dis->itemID, 0);
        std::wstring name(len > 0 ? len : 0, L'\0');
        if (len > 0) SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, (LPARAM)&name[0]);

        const bool inEditField = (dis->itemState & ODS_COMBOBOXEDIT) != 0;
        RECT nameRc = rc;
        nameRc.left += 6;
        if (!inEditField) nameRc.right -= 70;
        HGDIOBJ old = SelectObject(dc, s_uiFont);
        DrawTextW(dc, name.c_str(), -1, &nameRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (!inEditField)
        {
            RECT smp = rc;
            smp.left = rc.right - 68;
            smp.right -= 6;
            SelectObject(dc, SampleFont(dis->itemID == 0 ? kFallbackFace : name));
            DrawTextW(dc, L"가Ab", -1, &smp, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, old);
    }

    LRESULT CALLBACK BarProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_TE_CHECKFOCUS:
            if (s_hEdit && !InEditor(GetFocus())) FinishEdit(true);
            return 0;
        case WM_COMMAND:
        {
            const HWND from = (HWND)lParam;
            const int code = HIWORD(wParam);
            if (from == s_hFontCombo)
            {
                if (code == CBN_SELCHANGE) OnStyleChanged();
                else if (code == CBN_CLOSEUP) SetFocus(s_hEdit);
            }
            else if (from == s_hSizeCombo)
            {
                if (code == CBN_SELCHANGE)
                {
                    // 목록에서 고른 직후에는 칸의 글자가 아직 바뀌지 않았으므로 목록 값을 쓴다
                    const int sel = (int)SendMessageW(s_hSizeCombo, CB_GETCURSEL, 0, 0);
                    wchar_t buf[32] = {};
                    if (sel >= 0) SendMessageW(s_hSizeCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
                    const std::wstring s = buf;
                    OnStyleChanged(&s);
                }
                else if (code == CBN_EDITCHANGE) OnStyleChanged();
                else if (code == CBN_CLOSEUP) SetFocus(s_hEdit);
            }
            else if (from == s_hBoldBtn && code == BN_CLICKED)
            {
                OnStyleChanged();
                SetFocus(s_hEdit);
            }
            return 0;
        }
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = 24;
            return TRUE;
        case WM_DRAWITEM:
        {
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (dis->hwndItem == s_hFontCombo) { DrawFontItem(dis); return TRUE; }
            break;
        }
        case WM_CTLCOLORSTATIC:
        {
            HDC dc = (HDC)wParam;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(200, 60, 40));
            static HBRUSH bg = CreateSolidBrush(RGB(250, 250, 252));
            return (LRESULT)bg;
        }
        case WM_ERASEBKGND:
        {
            RECT rc; GetClientRect(hWnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(250, 250, 252));
            FillRect((HDC)wParam, &rc, bg);
            DeleteObject(bg);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(170, 175, 190));
            HGDIOBJ op = SelectObject((HDC)wParam, pen);
            HGDIOBJ ob = SelectObject((HDC)wParam, GetStockObject(NULL_BRUSH));
            Rectangle((HDC)wParam, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject((HDC)wParam, op); SelectObject((HDC)wParam, ob);
            DeleteObject(pen);
            return 1;
        }
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    const wchar_t* BarClass()
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc = {};
            wc.lpfnWndProc   = BarProc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = L"PPV_TextEditBar";
            RegisterClassW(&wc);
            registered = true;
        }
        return L"PPV_TextEditBar";
    }

    // 입력창 위치: 상자 왼쪽에 맞추고 세로 가운데 정렬
    POINT EditPos(const RECT& box, int editH)
    {
        return { box.left, box.top + ((box.bottom - box.top) - editH) / 2 };
    }

    int BarHeight()
    {
        int h = kBarPad * 2 + kRowH;
        if (s_hWarn && IsWindowVisible(s_hWarn)) h += kWarnH;
        return h;
    }

    // 스크롤/확대에 맞춰 입력창과 서식 막대를 따라 움직이고, 뷰포트를 벗어나면 숨긴다.
    // 서식 막대는 입력창 위에 두고, 위쪽 공간이 모자라면 아래에 둔다.
    void LayoutEditor(const RECT& vp)
    {
        if (!s_hEdit) return;
        bool show = false;
        const TextObj* o = (s_editDoc == g_pdfDoc) ? EditingObj() : nullptr;
        PageGeo g;
        if (o && GetPageGeo(s_editPage, vp, g))
        {
            RECT wr; GetWindowRect(s_hEdit, &wr);
            const int editH = wr.bottom - wr.top;
            const POINT pos = EditPos(BoxRect(*o, g), editH);
            POINT cur = { wr.left, wr.top };
            ScreenToClient(GetParent(s_hEdit), &cur);
            if (cur.x != pos.x || cur.y != pos.y)
                SetWindowPos(s_hEdit, nullptr, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            show = pos.y >= vp.top && pos.y + editH <= vp.bottom;

            if (s_hBar)
            {
                const int barH = BarHeight();
                RECT br; GetWindowRect(s_hBar, &br);
                int barY = pos.y - barH - 2;
                if (barY < vp.top) barY = pos.y + editH + 2;
                POINT bcur = { br.left, br.top };
                ScreenToClient(GetParent(s_hBar), &bcur);
                if (bcur.x != pos.x || bcur.y != barY || br.bottom - br.top != barH)
                    SetWindowPos(s_hBar, HWND_TOP, pos.x, barY, br.right - br.left, barH, SWP_NOACTIVATE);
            }
        }
        if ((IsWindowVisible(s_hEdit) != FALSE) != show)
            ShowWindow(s_hEdit, show ? SW_SHOWNA : SW_HIDE);
        if (s_hBar && (IsWindowVisible(s_hBar) != FALSE) != show)
            ShowWindow(s_hBar, show ? SW_SHOWNA : SW_HIDE);
    }

    void CreateFormatBar(HWND hWnd, const EditStyle& st, const TextObj& o)
    {
        if (!s_uiFont)
            s_uiFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                0, 0, CLEARTYPE_QUALITY, 0, L"Malgun Gothic");
        if (!s_uiBoldFont)
            s_uiBoldFont = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        const HINSTANCE hi = GetModuleHandleW(nullptr);
        const int fontW = 240, sizeW = 66, boldW = 30;
        const int barW = kBarPad * 2 + fontW + 4 + sizeW + 4 + boldW;
        s_hBar = CreateWindowExW(0, BarClass(), L"", WS_CHILD | WS_CLIPCHILDREN,
            0, 0, barW, kBarPad * 2 + kRowH, hWnd, nullptr, hi, nullptr);
        if (!s_hBar) return;

        int x = kBarPad;
        s_hFontCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
            x, kBarPad, fontW, 360, s_hBar, nullptr, hi, nullptr);
        x += fontW + 4;
        s_hSizeCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWN,
            x, kBarPad, sizeW, 300, s_hBar, nullptr, hi, nullptr);
        x += sizeW + 4;
        s_hBoldBtn = CreateWindowExW(0, L"BUTTON", L"B",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
            x, kBarPad, boldW, kRowH - 2, s_hBar, nullptr, hi, nullptr);
        s_hWarn = CreateWindowExW(0, L"STATIC", LS(STR_TEXTEDIT_NO_GLYPH),
            WS_CHILD | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
            kBarPad + 2, kBarPad + kRowH, barW - kBarPad * 2 - 4, kWarnH, s_hBar, nullptr, hi, nullptr);

        for (HWND h : { s_hFontCombo, s_hSizeCombo, s_hWarn })
            SendMessageW(h, WM_SETFONT, (WPARAM)s_uiFont, FALSE);
        SendMessageW(s_hBoldBtn, WM_SETFONT, (WPARAM)s_uiBoldFont, FALSE);

        // 글꼴 목록: 0번 = 자동, 그 뒤로 설치된 글꼴
        SendMessageW(s_hFontCombo, CB_ADDSTRING, 0, (LPARAM)LS(STR_TEXTEDIT_FONT_AUTO));
        for (const auto& f : InstalledFonts())
            SendMessageW(s_hFontCombo, CB_ADDSTRING, 0, (LPARAM)f.c_str());
        int sel = 0;
        if (!st.face.empty())
        {
            const LRESULT i = SendMessageW(s_hFontCombo, CB_FINDSTRINGEXACT, 0, (LPARAM)st.face.c_str());
            if (i != CB_ERR) sel = (int)i;
        }
        SendMessageW(s_hFontCombo, CB_SETCURSEL, sel, 0);
        SendMessageW(s_hFontCombo, CB_SETMINVISIBLE, 14, 0);

        static const wchar_t* sizes[] = { L"8", L"9", L"10", L"11", L"12", L"14", L"16", L"18",
            L"20", L"24", L"28", L"32", L"36", L"48", L"60", L"72" };
        for (const wchar_t* s : sizes)
            SendMessageW(s_hSizeCombo, CB_ADDSTRING, 0, (LPARAM)s);
        SetWindowTextW(s_hSizeCombo, FormatSize(EffSizePt(o, st)).c_str());

        SendMessageW(s_hBoldBtn, BM_SETCHECK, st.bold ? BST_CHECKED : BST_UNCHECKED, 0);

        Subclass(s_hFontCombo);
        COMBOBOXINFO ci = { sizeof(ci) };
        if (GetComboBoxInfo(s_hSizeCombo, &ci)) Subclass(ci.hwndItem);
        Subclass(s_hBoldBtn);
        ShowWindow(s_hBar, SW_SHOWNA);
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
        const std::wstring current = e ? e->text : o.text;
        const EditStyle style = e ? e->style : DefaultStyle(o);

        const int editW = std::max<int>((box.right - box.left) + 80, 260);
        s_hMain        = hWnd;
        s_editDoc      = g_pdfDoc;
        s_editPage     = page;
        s_editObj      = objIdx;
        s_editOrigSize = OrigSizePt(o);
        s_hEdit = CreateWindowExW(0, L"EDIT", current.c_str(),
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            box.left, box.top, editW, 30, hWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!s_hEdit) { s_editDoc = nullptr; s_editPage = s_editObj = -1; return; }
        SendMessageW(s_hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
        Subclass(s_hEdit);

        CreateFormatBar(hWnd, style, o);
        ApplyEditorStyle(style); // 입력창 글꼴·높이, 굵게 사용 가능 여부, 경고, 위치

        SetFocus(s_hEdit);
        SendMessageW(s_hEdit, EM_SETSEL, 0, -1);
        RedrawMain();
    }

    // ── 저장 시 글꼴 넣기 ────────────────────────────────────────────────────

    void* __cdecl FsAlloc(size_t n)            { return malloc(n); }
    void* __cdecl FsRealloc(void* p, size_t n) { return realloc(p, n); }
    void  __cdecl FsFree(void* p)              { free(p); }

    // ── cmap 다시 쓰기 ──────────────────────────────────────────────────────
    // 글꼴의 cmap에 여러 문자가 같은 글리프를 가리키면(예: Times New Roman의 공백 U+0020과
    // 줄바꿈 없는 공백 U+00A0) PDFium은 글리프→문자 표를 만들 때 뒤의 문자로 덮어써서,
    // 앞의 문자(보통 공백)를 쓸 때 글리프를 못 찾고 0번(빈 상자)을 넣는다.
    // 그래서 넣기 전에 cmap을 실제로 쓴 글자만 담은 format 4 표 하나로 바꾼다.

    void PutBE16(std::vector<unsigned char>& v, unsigned x) { v.push_back((unsigned char)(x >> 8)); v.push_back((unsigned char)x); }
    void PutBE32(std::vector<unsigned char>& v, unsigned x) { PutBE16(v, x >> 16); PutBE16(v, x & 0xFFFF); }
    void SetBE32(unsigned char* p, unsigned x) { p[0] = (unsigned char)(x >> 24); p[1] = (unsigned char)(x >> 16); p[2] = (unsigned char)(x >> 8); p[3] = (unsigned char)x; }

    // cmap 하위 표 하나에서 문자 → 글리프 번호 (없으면 0)
    unsigned CmapGlyph(const unsigned char* sub, size_t avail, unsigned c)
    {
        if (avail < 4) return 0;
        const unsigned format = BE16(sub);
        if (format == 4)
        {
            const size_t len = std::min<size_t>(BE16(sub + 2), avail);
            if (len < 14) return 0;
            const unsigned segX2 = BE16(sub + 6);
            const size_t endOff = 14, startOff = endOff + segX2 + 2, deltaOff = startOff + segX2, rangeOff = deltaOff + segX2;
            if (rangeOff + segX2 > len) return 0;
            for (unsigned i = 0; i < segX2 / 2; ++i)
            {
                const unsigned end = BE16(sub + endOff + 2 * i);
                if (end < c) continue;
                const unsigned start = BE16(sub + startOff + 2 * i);
                if (start > c) return 0;
                const unsigned delta = BE16(sub + deltaOff + 2 * i);
                const unsigned ro = BE16(sub + rangeOff + 2 * i);
                if (ro == 0) return (c + delta) & 0xFFFF;
                const size_t addr = rangeOff + 2 * i + ro + 2 * (c - start);
                if (addr + 2 > len) return 0;
                const unsigned g = BE16(sub + addr);
                return g ? ((g + delta) & 0xFFFF) : 0;
            }
            return 0;
        }
        if (format == 12)
        {
            if (avail < 16) return 0;
            const unsigned n = BE32(sub + 12);
            for (unsigned i = 0; i < n && 16 + 12 * (size_t)(i + 1) <= avail; ++i)
            {
                const unsigned char* g = sub + 16 + 12 * i;
                if (c >= BE32(g) && c <= BE32(g + 4)) return BE32(g + 8) + (c - BE32(g));
            }
        }
        return 0;
    }

    // font의 cmap을 chars만 담은 (3,1) format 4 표로 바꾼 새 글꼴. 실패하면 빈 벡터
    std::vector<unsigned char> RewriteCmap(const std::vector<unsigned char>& font, const std::set<wchar_t>& chars)
    {
        const size_t size = font.size();
        if (size < 12) return {};
        const unsigned char* p = font.data();
        const unsigned numTables = BE16(p + 4);
        if (12 + 16 * (size_t)numTables > size) return {};

        // cmap 표 찾기
        const unsigned char* cmap = nullptr;
        size_t cmapLen = 0;
        for (unsigned i = 0; i < numTables; ++i)
        {
            const unsigned char* rec = p + 12 + 16 * i;
            if (memcmp(rec, "cmap", 4) == 0)
            {
                const size_t off = BE32(rec + 8), len = BE32(rec + 12);
                if (off + len > size) return {};
                cmap = p + off; cmapLen = len;
            }
        }
        if (!cmap || cmapLen < 4) return {};

        // 유니코드 하위 표 고르기: (3,10) → (3,1) → (0,*)
        const unsigned char* sub = nullptr;
        size_t subAvail = 0;
        int bestRank = 99;
        const unsigned nSub = BE16(cmap + 2);
        for (unsigned i = 0; i < nSub && 4 + 8 * (size_t)(i + 1) <= cmapLen; ++i)
        {
            const unsigned char* er = cmap + 4 + 8 * i;
            const unsigned plat = BE16(er), enc = BE16(er + 2);
            const size_t off = BE32(er + 4);
            if (off + 4 > cmapLen) continue;
            const unsigned fmt = BE16(cmap + off);
            int rank = 99;
            if (plat == 3 && enc == 10 && fmt == 12) rank = 0;
            else if (plat == 3 && enc == 1 && fmt == 4) rank = 1;
            else if (plat == 0 && (fmt == 4 || fmt == 12)) rank = 2;
            if (rank < bestRank) { bestRank = rank; sub = cmap + off; subAvail = cmapLen - off; }
        }
        if (!sub) return {};

        std::vector<std::pair<unsigned, unsigned>> map; // 문자 → 글리프 (문자 순)
        for (wchar_t c : chars)
        {
            if (c >= 0xFFFF) continue;
            const unsigned g = CmapGlyph(sub, subAvail, c);
            if (g) map.push_back({ (unsigned)c, g });
        }
        if (map.empty()) return {};

        // format 4: 문자마다 한 구간 + 끝 구간(0xFFFF)
        const unsigned segCount = (unsigned)map.size() + 1;
        unsigned entrySel = 0;
        while ((2u << entrySel) <= segCount) ++entrySel;
        const unsigned searchRange = 2u << entrySel;
        std::vector<unsigned char> f4;
        PutBE16(f4, 4); PutBE16(f4, 16 + 8 * segCount); PutBE16(f4, 0);
        PutBE16(f4, segCount * 2); PutBE16(f4, searchRange); PutBE16(f4, entrySel); PutBE16(f4, segCount * 2 - searchRange);
        for (auto& m : map) PutBE16(f4, m.first);
        PutBE16(f4, 0xFFFF);
        PutBE16(f4, 0);                                            // reservedPad
        for (auto& m : map) PutBE16(f4, m.first);
        PutBE16(f4, 0xFFFF);
        for (auto& m : map) PutBE16(f4, (m.second - m.first) & 0xFFFF);
        PutBE16(f4, 1);
        for (unsigned i = 0; i < segCount; ++i) PutBE16(f4, 0);    // idRangeOffset

        std::vector<unsigned char> newCmap;
        PutBE16(newCmap, 0); PutBE16(newCmap, 1);                  // version, 하위 표 1개
        PutBE16(newCmap, 3); PutBE16(newCmap, 1); PutBE32(newCmap, 12);
        newCmap.insert(newCmap.end(), f4.begin(), f4.end());

        // 표 목록은 그대로 두고 cmap만 바꿔 글꼴을 다시 조립한다 (각 표는 4바이트 정렬)
        std::vector<unsigned char> out(p, p + 12 + 16 * numTables);
        for (unsigned i = 0; i < numTables; ++i)
        {
            unsigned char* rec = out.data() + 12 + 16 * i;
            const bool isCmap = memcmp(rec, "cmap", 4) == 0;
            const size_t off = BE32(rec + 8), len = BE32(rec + 12);
            if (!isCmap && off + len > size) return {};
            const unsigned char* src = isCmap ? newCmap.data() : p + off;
            const size_t n = isCmap ? newCmap.size() : len;
            const size_t newOff = out.size();
            out.insert(out.end(), src, src + n);
            while (out.size() % 4) out.push_back(0);
            rec = out.data() + 12 + 16 * i; // insert로 버퍼가 옮겨졌을 수 있다
            if (isCmap)
            {
                unsigned sum = 0;
                for (size_t k = newOff; k < out.size(); k += 4) sum += BE32(out.data() + k);
                SetBE32(rec + 4, sum);
            }
            SetBE32(rec + 8, (unsigned)newOff);
            SetBE32(rec + 12, (unsigned)n);
        }
        return out;
    }

    // 설치된 글꼴(face, bold)을 chars에 든 글자만 남긴 서브셋으로 doc에 불러온다.
    // 글꼴 파일은 GDI(GetFontData)로 읽으므로 .ttc 묶음 글꼴(굴림·바탕 등)도 그 안의 한 글꼴만 뽑아 쓴다.
    // 서브셋을 못 만들면 (.ttf일 때) 글꼴 전체를 넣는다.
    FPDF_FONT LoadSystemFont(FPDF_DOCUMENT doc, const std::wstring& face, bool bold, const std::set<wchar_t>& chars)
    {
        FontDC f(face, bold);
        if (!f.ok() || !EmbeddingAllowed(f.dc)) return nullptr;

        std::vector<unsigned char> file;
        bool isTtc = false;
        unsigned short ttcIndex = 0;
        const DWORD ttcSize = GetFontData(f.dc, TAG_TTCF, 0, nullptr, 0);
        if (ttcSize != GDI_ERROR && ttcSize > 12)
        {
            isTtc = true;
            file.resize(ttcSize);
            if (GetFontData(f.dc, TAG_TTCF, 0, file.data(), ttcSize) != ttcSize) return nullptr;
            // 지금 고른 글꼴의 테이블 목록과 같은 것을 묶음 안에서 찾아 번호를 얻는다
            unsigned char hdr[12];
            if (GetFontData(f.dc, 0, 0, hdr, 12) != 12) return nullptr;
            const DWORD dirLen = 12 + 16 * BE16(hdr + 4);
            std::vector<unsigned char> dir(dirLen);
            if (GetFontData(f.dc, 0, 0, dir.data(), dirLen) != dirLen) return nullptr;
            const unsigned numFonts = BE32(file.data() + 8);
            bool found = false;
            for (unsigned i = 0; i < numFonts && 12 + 4 * (i + 1) <= ttcSize; ++i)
            {
                const unsigned off = BE32(file.data() + 12 + 4 * i);
                if (off + dirLen <= ttcSize && memcmp(file.data() + off, dir.data(), dirLen) == 0)
                { ttcIndex = (unsigned short)i; found = true; break; }
            }
            if (!found) return nullptr;
        }
        else
        {
            const DWORD size = GetFontData(f.dc, 0, 0, nullptr, 0);
            if (size == GDI_ERROR || size == 0) return nullptr;
            file.resize(size);
            if (GetFontData(f.dc, 0, 0, file.data(), size) != size) return nullptr;
        }

        std::vector<unsigned short> keep(chars.begin(), chars.end());
        keep.push_back(L' ');
        unsigned char* sub = nullptr;
        unsigned long subSize = 0, written = 0;
        const bool subsetAllowed = !(ReadOS2U16(f.dc, 8) > 0 && (ReadOS2U16(f.dc, 8) & 0x0100));
        unsigned long rc = 1;
        if (subsetAllowed)
            rc = CreateFontPackage(file.data(), (unsigned long)file.size(), &sub, &subSize, &written,
                (unsigned short)(TTFCFP_FLAGS_SUBSET | (isTtc ? TTFCFP_FLAGS_TTC : 0)), ttcIndex,
                TTFCFP_SUBSET, 0, TTFCFP_MS_PLATFORMID, TTFCFP_UNICODE_CHAR_SET,
                keep.data(), (unsigned short)keep.size(), FsAlloc, FsRealloc, FsFree, nullptr);
        if (rc == 0 && sub && written > 0)
            s_fontKeepAlive.emplace_back(sub, sub + written);
        else if (!isTtc)
            s_fontKeepAlive.push_back(std::move(file));
        else
        {
            if (sub) FsFree(sub);
            return nullptr;
        }
        if (sub) FsFree(sub);

        std::set<wchar_t> used = chars;
        used.insert(L' ');
        std::vector<unsigned char> fixed = RewriteCmap(s_fontKeepAlive.back(), used);
        if (!fixed.empty()) s_fontKeepAlive.back() = std::move(fixed);

        const auto& data = s_fontKeepAlive.back();
        return FPDFText_LoadFont(doc, data.data(), (uint32_t)data.size(), FPDF_FONT_TRUETYPE, /*cid=*/1);
    }

    struct FontKey
    {
        bool standard; std::wstring face; bool bold;
        bool operator<(const FontKey& o) const
        {
            if (standard != o.standard) return standard < o.standard;
            if (bold != o.bold) return bold < o.bold;
            return face < o.face;
        }
    };
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
        if (!s_active && !hasEdits) { LayoutEditor(vp); return; }

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
                    RECT area = DrawPreview(hdc, o, *e, g);
                    InflateRect(&area, 2, 2);
                    UnionRect(&box, &box, &area);
                }
                if (s_active && !editing)
                    DrawBox(hdc, box, pi == s_hoverPage && k == s_hoverObj, e != nullptr);
            }
        }

        RestoreDC(hdc, saved);
        LayoutEditor(vp);
    }

    void ExcludeEditorFromPaint(HWND hWnd, HDC hdc)
    {
        for (HWND h : { s_hEdit, s_hBar })
        {
            if (!h || !IsWindowVisible(h) || GetParent(h) != hWnd) continue;
            RECT r;
            GetWindowRect(h, &r);
            MapWindowPoints(nullptr, hWnd, reinterpret_cast<POINT*>(&r), 2);
            ExcludeClipRect(hdc, r.left, r.top, r.right, r.bottom);
        }
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
        const EditStyle style = e ? e->style : DefaultStyle(o);

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
            SetObjEdit(ds, page, o, L"", style);
        else if (cmd == CMD_RESTORE)
            SetObjEdit(ds, page, o, o.text, DefaultStyle(o));
        RedrawMain();
        return true;
    }

    void CommitEdit() { FinishEdit(true); }

    bool IsEditFocused() { return InEditor(GetFocus()); }

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

        // 필요한 글꼴과 글꼴마다 쓰인 글자를 모은다
        std::map<FontKey, std::set<wchar_t>> need;
        for (const auto& e : edits)
        {
            if (e.text.empty()) continue;
            auto& chars = need[{ e.res.standard, e.res.standard ? L"" : e.res.face, e.res.bold }];
            chars.insert(e.text.begin(), e.text.end());
        }

        std::lock_guard<std::mutex> lk(g_pdfiumMtx);
        s_fontKeepAlive.clear(); // 이전 저장의 문서는 이미 닫혔다
        std::map<FontKey, FPDF_FONT> fonts;
        for (const auto& [key, chars] : need)
        {
            FPDF_FONT f = key.standard
                ? FPDFText_LoadStandardFont(doc, key.bold ? "Helvetica-Bold" : "Helvetica")
                : LoadSystemFont(doc, key.face, key.bold, chars);
            // 고른 글꼴을 넣지 못하면 맑은 고딕으로 대신한다
            if (!f && (key.standard || key.face != kFallbackFace))
                f = LoadSystemFont(doc, kFallbackFace, key.bold, chars);
            fonts[key] = f;
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
                    const Resolved& res = j.e->res;
                    auto fit = fonts.find({ res.standard, res.standard ? L"" : res.face, res.bold });
                    FPDF_FONT font = (fit != fonts.end()) ? fit->second : nullptr;
                    if (!font) continue; // 글꼴이 없으면 원래 글자를 그대로 둔다

                    FS_MATRIX m{};
                    float fontSize = 0;
                    unsigned int R = 0, G = 0, B = 0, A = 255;
                    FPDFPageObj_GetMatrix(j.obj, &m);
                    FPDFTextObj_GetFontSize(j.obj, &fontSize);
                    FPDFPageObj_GetFillColor(j.obj, &R, &G, &B, &A);
                    // 크기를 바꿨으면 행렬의 세로 배율을 감안해 글꼴 크기(Tf)를 정한다
                    if (j.e->style.size > 0 && m.d > 0)
                        fontSize = j.e->style.size / m.d;

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

        for (auto& [key, f] : fonts)
            if (f) FPDFFont_Close(f);
        return applied;
    }
}
