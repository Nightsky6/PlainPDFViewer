// ---------------------------------------------------------------
// GroupPanel.cpp - 오른쪽 그룹 패널 구현
// ---------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <shlobj.h>
#include <dwmapi.h>
#include "PanelTheme.h"
#include <commdlg.h>
#include <string>
#include <functional>
#include <vector>
#include <algorithm>
#include "GroupPanel.h"
#include "CleanPopup.h"
#include "Toolbar.h"
#include "PlainPDFViewer.h"
#include "Lang.h"
#include "Strings.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Comdlg32.lib")

// ── 내부 상태 ──────────────────────────────────────────────────
namespace
{
    // ── 데이터 ──────────────────────────────────────────────────
    std::vector<GroupDef> g_groups;
    int  g_selGroup   = -1;   // 선택된 그룹 인덱스
    int  g_scrollTab  = 0;    // 드롭다운 목록 스크롤 (첫 번째로 보이는 그룹 인덱스)
    int  g_scrollItem = 0;    // 항목 영역 스크롤 오프셋

    // ── 패널 표시 ────────────────────────────────────────────────
    bool g_visible = false;

    // ── 호버 상태 ────────────────────────────────────────────────
    bool g_hoverClose   = false;
    int  g_hoverTab     = -1;  // 드롭다운 목록 호버 인덱스
    bool g_hoverDropBtn = false;
    int  g_hoverItem    = -1;  // 호버된 항목 인덱스
    int  g_hoverBtn     = -1;  // 0=등록, 1=메모
    bool g_hoverFooter  = false;  // 하단 [+ 항목 추가] 버튼

    // ── 드롭다운 상태 ─────────────────────────────────────────────
    bool g_dropOpen   = false;  // 드롭다운 열림 여부
    int  g_hoverDrop  = -1;     // 드롭다운 목록 내 호버 인덱스

    // ── 클릭 추적 ────────────────────────────────────────────────
    bool g_pressedInPanel = false;  // 마우스 DOWN이 패널 위에서 일어났는지

    // ── 메모 인라인 편집 ─────────────────────────────────────────
    int  g_memoEditItem = -1;
    HWND g_hMemoEdit    = nullptr;

    // ── 팝업 공용 상태 ───────────────────────────────────────────
    const wchar_t* POPUP_CLASS = L"GpPopupClass";
    bool g_popupClassReg = false;

    struct PopupState {
        std::wstring result;
        bool closed  = false;
        bool ok      = false;
        HWND hEdit   = nullptr;
        HFONT hFont  = nullptr;
        RECT okBtn   = {};
        RECT cancelBtn = {};
        bool okHov   = false;
        bool canHov  = false;
        std::wstring title;
        std::wstring label;
    } g_popup;

    // ── 그리기 헬퍼 ─────────────────────────────────────────────

    void DrawRR(Gdiplus::Graphics& g, float x, float y, float w, float h,
                float r, Gdiplus::Brush* fill, Gdiplus::Pen* pen = nullptr)
    {
        using namespace Gdiplus;
        GraphicsPath path;
        float d = r * 2.0f;
        path.AddArc(x,       y,       d, d, 180, 90);
        path.AddArc(x+w-d,   y,       d, d, 270, 90);
        path.AddArc(x+w-d,   y+h-d,   d, d, 0,   90);
        path.AddArc(x,       y+h-d,   d, d, 90,  90);
        path.CloseFigure();
        if (fill) g.FillPath(fill, &path);
        if (pen)  g.DrawPath(pen,  &path);
    }

    void Btn(Gdiplus::Graphics& g, const Gdiplus::FontFamily& ff,
             float x, float y, float w, float h,
             const wchar_t* text, bool hov,
             Gdiplus::Color bg, Gdiplus::Color bgH,
             Gdiplus::Color fg, int ptSize = 8)
    {
        using namespace Gdiplus;
        Color c = hov ? bgH : bg;
        SolidBrush br(c);
        DrawRR(g, x, y, w, h, 4.0f, &br);
        Font f(&ff, (float)ptSize, FontStyleRegular, UnitPoint);
        SolidBrush tb(fg);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(text, -1, &f, RectF(x, y, w, h), &sf, &tb);
    }

    // ── 유틸리티 ────────────────────────────────────────────────

    std::wstring GetGroupFolder()
    {
        wchar_t path[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, path)))
            return std::wstring(path) + L"\\PlainPDFViewer\\Groups";
        return L"";
    }

    std::wstring SafeName(const std::wstring& n)
    {
        std::wstring s = n;
        for (auto& c : s)
            if (std::wstring(L"\\/:*?\"<>|").find(c) != std::wstring::npos) c = L'_';
        return s;
    }

    // ── PDF 경로 헬퍼 ───────────────────────────────────────────

    bool FileExists(const std::wstring& p)
    {
        if (p.empty()) return false;
        DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring DirOf(const std::wstring& p)
    {
        auto pos = p.find_last_of(L"\\/");
        return pos == std::wstring::npos ? L"" : p.substr(0, pos);
    }

    std::wstring FileNameOf(const std::wstring& p)
    {
        auto pos = p.find_last_of(L"\\/");
        return pos == std::wstring::npos ? p : p.substr(pos + 1);
    }

    // 항목들의 missing 캐시 갱신 (gi < 0 이면 전체 그룹)
    void RefreshMissing(int gi)
    {
        for (int g = 0; g < (int)g_groups.size(); ++g) {
            if (gi >= 0 && g != gi) continue;
            for (auto& it : g_groups[g].items)
                it.missing = !it.pdfPath.empty() && !FileExists(it.pdfPath);
        }
    }

    // PDF 파일 선택 대화상자. hintPath가 있으면 그 파일명과
    // 가장 가까운 존재하는 상위 폴더에서 시작한다. 취소 시 빈 문자열.
    std::wstring PickPdfFile(HWND owner, const std::wstring& hintPath)
    {
        WCHAR buf[MAX_PATH] = {};
        std::wstring initDir;
        if (!hintPath.empty()) {
            std::wstring fn = FileNameOf(hintPath);
            if (fn.size() < MAX_PATH) wcscpy_s(buf, fn.c_str());
            for (std::wstring d = DirOf(hintPath); !d.empty(); ) {
                DWORD a = GetFileAttributesW(d.c_str());
                if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { initDir = d; break; }
                std::wstring up = DirOf(d);
                if (up == d) break;
                d = up;
            }
        }
        OPENFILENAMEW ofn = {};
        ofn.lStructSize     = sizeof(ofn);
        ofn.hwndOwner       = owner;
        ofn.lpstrFilter     = L"PDF (*.pdf)\0*.pdf\0*.*\0*.*\0";
        ofn.lpstrFile       = buf;
        ofn.nMaxFile        = MAX_PATH;
        ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
        ofn.lpstrTitle      = LS(STR_GP_PICK_TITLE);
        ofn.lpstrDefExt     = L"pdf";
        ofn.Flags           = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
        return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
    }

    // 항목에 새 PDF 경로를 연결한다. 기존 파일이 사라진 상태였다면
    // 같은 폴더에 있다가 함께 사라진 다른 항목들 중 새 폴더에 같은 이름의
    // 파일이 있는 것을 찾아 함께 다시 연결할지 묻는다 (폴더째 옮긴 경우).
    void AssignPdf(HWND owner, int gi, int ii, const std::wstring& newPath)
    {
        if (gi < 0 || gi >= (int)g_groups.size()) return;
        if (ii < 0 || ii >= (int)g_groups[gi].items.size()) return;
        GroupItem* target = &g_groups[gi].items[ii];
        std::wstring oldPath = target->pdfPath;
        bool wasMissing = !oldPath.empty() && !FileExists(oldPath);
        target->pdfPath = newPath;
        target->missing = false;

        std::wstring oldDir = DirOf(oldPath), newDir = DirOf(newPath);
        if (wasMissing && !oldDir.empty() && !newDir.empty()
            && _wcsicmp(oldDir.c_str(), newDir.c_str()) != 0)
        {
            std::vector<std::pair<GroupItem*, std::wstring>> cands;
            for (auto& gd : g_groups) {
                for (auto& o : gd.items) {
                    if (&o == target || o.pdfPath.empty()) continue;
                    if (_wcsicmp(DirOf(o.pdfPath).c_str(), oldDir.c_str()) != 0) continue;
                    if (FileExists(o.pdfPath)) continue;
                    std::wstring c = newDir + L"\\" + FileNameOf(o.pdfPath);
                    if (FileExists(c)) cands.push_back({&o, c});
                }
            }
            if (!cands.empty()) {
                wchar_t msg[512];
                swprintf_s(msg, LS(STR_GP_RELINK_FMT), (int)cands.size());
                if (CleanPopup::Show(owner, LS(STR_GP_RELINK_TITLE), msg, true))
                    for (auto& c : cands) { c.first->pdfPath = c.second; c.first->missing = false; }
            }
        }
        GroupPanel::SaveGroups();  // 경로 변경은 바로 저장 (비정상 종료 대비)
    }

    // 등록된 PDF가 없을 때: 알리고, 원하면 새 위치를 골라 다시 연결한다.
    // 다시 연결했으면 true.
    bool PromptRelink(HWND owner, int gi, int ii)
    {
        std::wstring path = g_groups[gi].items[ii].pdfPath;
        std::wstring msg = std::wstring(LS(STR_GP_MISSING_MSG)) + L"\n\n" + path
                         + L"\n\n" + LS(STR_GP_MISSING_ASK);
        if (!CleanPopup::Show(owner, LS(STR_GP_MISSING_TITLE), msg, true)) return false;
        std::wstring picked = PickPdfFile(owner, path);
        if (picked.empty()) return false;
        AssignPdf(owner, gi, ii, picked);
        return true;
    }

    // ── 모달 루프 공용 ──────────────────────────────────────────
    // 호출 측은 루프 전에 EnableWindow(owner, FALSE)로 owner 입력을 막는다.
    // closed가 true가 될 때까지 메시지를 돌리고, filter가 true를 반환한
    // 메시지는 디스패치하지 않는다. WM_QUIT은 삼키지 않고 다시 게시한다.
    void RunModalLoop(const bool& closed,
                      const std::function<bool(const MSG&)>& filter = nullptr)
    {
        MSG msg;
        while (!closed) {
            BOOL r = GetMessage(&msg, nullptr, 0, 0);
            if (r == 0) { PostQuitMessage((int)msg.wParam); break; }
            if (r == -1) break;
            if (filter && filter(msg)) continue;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // 모달 창 닫기: 창을 없애기 전에 owner를 다시 활성화해야
    // 활성 창이 다른 앱으로 넘어가지 않는다.
    void CloseModal(HWND dlg)
    {
        if (HWND owner = GetWindow(dlg, GW_OWNER)) EnableWindow(owner, TRUE);
        DestroyWindow(dlg);
    }

    // ── 팝업 다이얼로그 ──────────────────────────────────────────

    void DrawPopup(HWND hwnd, HDC hdc)
    {
        using namespace Gdiplus;
        constexpr int W = 340, H = 170;
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        SolidBrush bg(Color(255,255,255,255));
        g.FillRectangle(&bg, 0.0f, 0.0f, (float)W, (float)H);
        Pen border(Color(255,218,218,218), 1.0f);
        g.DrawRectangle(&border, 0.5f, 0.5f, (float)(W-1), (float)(H-1));

        FontFamily ff(L"Segoe UI");
        Font titleF(&ff, 11, FontStyleBold, UnitPoint);
        SolidBrush dark(Color(255,20,20,20));
        g.DrawString(g_popup.title.c_str(), -1, &titleF,
                     RectF(20,18,(float)(W-40),26), nullptr, &dark);

        Font lblF(&ff, 9, FontStyleRegular, UnitPoint);
        SolidBrush gray(Color(255,90,90,100));
        g.DrawString(g_popup.label.c_str(), -1, &lblF,
                     RectF(20,52,(float)(W-40),18), nullptr, &gray);

        Pen sep(Color(255,228,228,232), 1.0f);
        g.DrawLine(&sep, 0.0f, (float)(H-56), (float)W, (float)(H-56));

        auto drawBtn = [&](const RECT& rc, const wchar_t* txt, bool hov, bool primary) {
            RectF rf((float)rc.left+0.5f, (float)rc.top+0.5f,
                     (float)(rc.right-rc.left)-1.0f, (float)(rc.bottom-rc.top)-1.0f);
            GraphicsPath* path = new GraphicsPath();
            float d = 8.0f;
            path->AddArc(rf.X,          rf.Y,          d,d,180,90);
            path->AddArc(rf.X+rf.Width-d,rf.Y,         d,d,270,90);
            path->AddArc(rf.X+rf.Width-d,rf.Y+rf.Height-d,d,d,0,90);
            path->AddArc(rf.X,           rf.Y+rf.Height-d,d,d,90,90);
            path->CloseFigure();
            if (primary) {
                Color c = hov ? Color(255,14,148,244) : Color(255,0,120,212);
                SolidBrush b(c); g.FillPath(&b, path);
                Font f(&ff,9,FontStyleBold,UnitPoint);
                SolidBrush t(Color(255,255,255,255));
                StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
                g.DrawString(txt,-1,&f,rf,&sf,&t);
            } else {
                Color c = hov ? Color(255,230,232,240) : Color(255,245,246,250);
                SolidBrush b(c); g.FillPath(&b, path);
                Pen p(Color(255,200,202,212),1.0f); g.DrawPath(&p, path);
                Font f(&ff,9,FontStyleRegular,UnitPoint);
                SolidBrush t(Color(255,50,50,60));
                StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
                g.DrawString(txt,-1,&f,rf,&sf,&t);
            }
            delete path;
        };
        drawBtn(g_popup.cancelBtn, LS(STR_GP_POPUP_CANCEL), g_popup.canHov, false);
        drawBtn(g_popup.okBtn,     LS(STR_GP_POPUP_OK),     g_popup.okHov,  true);
    }

    LRESULT CALLBACK PopupProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        constexpr int W = 340, H = 170;
        switch (msg)
        {
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            DrawPopup(hWnd, mem);
            BitBlt(hdc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
            SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem);
            EndPaint(hWnd, &ps); } break;

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, RGB(248,249,252)); SetTextColor(hdc, RGB(20,20,30));
            static HBRUSH hbr = CreateSolidBrush(RGB(248,249,252));
            return (LRESULT)hbr; }

        case WM_MOUSEMOVE: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            bool oh = PtInRect(&g_popup.okBtn,     pt) != FALSE;
            bool ch = PtInRect(&g_popup.cancelBtn, pt) != FALSE;
            if (oh != g_popup.okHov || ch != g_popup.canHov) {
                g_popup.okHov = oh; g_popup.canHov = ch;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hWnd, 0};
            TrackMouseEvent(&tme); } break;

        case WM_MOUSELEAVE:
            g_popup.okHov = g_popup.canHov = false;
            InvalidateRect(hWnd, nullptr, FALSE); break;

        case WM_LBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            auto commit = [&](bool ok) {
                if (ok && g_popup.hEdit) {
                    int len = GetWindowTextLengthW(g_popup.hEdit);
                    g_popup.result.resize(len+1, L'\0');
                    if (len) GetWindowTextW(g_popup.hEdit, g_popup.result.data(), len+1);
                    while (!g_popup.result.empty() && g_popup.result.back()==L'\0')
                        g_popup.result.pop_back();
                }
                g_popup.ok = ok; g_popup.closed = true;
                HWND par = GetParent(hWnd);
                for (int a=255; a>=0; a-=40) {
                    SetLayeredWindowAttributes(hWnd,0,(BYTE)a,LWA_ALPHA);
                    UpdateWindow(hWnd); Sleep(8);
                }
                if (g_popup.hFont) { DeleteObject(g_popup.hFont); g_popup.hFont=nullptr; }
                CloseModal(hWnd);
                if (par) SetForegroundWindow(par);
            };
            if (PtInRect(&g_popup.okBtn,     pt)) commit(true);
            else if (PtInRect(&g_popup.cancelBtn, pt)) commit(false);
        } break;
        // Enter/Esc는 포커스가 EDIT에 있어 여기로 오지 않으므로 모달 루프에서 처리한다.

        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProc(hWnd,msg,wParam,lParam);
            if (hit==HTCLIENT) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hWnd, &pt);
                if (!PtInRect(&g_popup.okBtn,pt) && !PtInRect(&g_popup.cancelBtn,pt)
                    && pt.y < H-56) return HTCAPTION;
            }
            return hit; }

        default: return DefWindowProc(hWnd,msg,wParam,lParam);
        }
        return 0;
    }

    // title / label / 기존값 → 팝업 표시, 결과 반환 (empty=취소)
    std::wstring ShowInputPopup(HWND parent, const wchar_t* title,
                                const wchar_t* label, const wchar_t* initVal = L"")
    {
        constexpr int W=340, H=170, BTN_W=100, BTN_H=36;
        if (!g_popupClassReg) {
            WNDCLASSEXW wc={};
            wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW;
            wc.lpfnWndProc=PopupProc; wc.hInstance=hInst;
            wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
            wc.hbrBackground=nullptr; wc.lpszClassName=POPUP_CLASS;
            RegisterClassExW(&wc);
            g_popupClassReg=true;
        }
        int gap=12, bY=H-56+(56-BTN_H)/2, bX=(W-BTN_W*2-gap)/2;
        g_popup.cancelBtn={bX,bY,bX+BTN_W,bY+BTN_H};
        g_popup.okBtn={bX+BTN_W+gap,bY,bX+BTN_W*2+gap,bY+BTN_H};
        g_popup.okHov=g_popup.canHov=false;
        g_popup.closed=false; g_popup.ok=false; g_popup.result.clear();
        g_popup.title=title; g_popup.label=label;

        RECT pr; GetWindowRect(parent,&pr);
        int x=pr.left+(pr.right-pr.left-W)/2;
        int y=pr.top+(pr.bottom-pr.top-H)/2;

        HWND hwnd = CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST,
            POPUP_CLASS,nullptr,WS_POPUP,x,y,W,H,parent,nullptr,hInst,nullptr);
        if (!hwnd) return L"";

        DWM_WINDOW_CORNER_PREFERENCE corner=DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd,DWMWA_WINDOW_CORNER_PREFERENCE,&corner,sizeof(corner));
        MARGINS margins={1,1,1,1};
        DwmExtendFrameIntoClientArea(hwnd,&margins);

        g_popup.hFont=CreateFontW(15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        g_popup.hEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",initVal,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            20,74,W-40,28,hwnd,nullptr,hInst,nullptr);
        SendMessage(g_popup.hEdit,WM_SETFONT,(WPARAM)g_popup.hFont,TRUE);

        SetLayeredWindowAttributes(hwnd,0,0,LWA_ALPHA);
        ShowWindow(hwnd,SW_SHOW);
        AnimateWindow(hwnd,180,AW_BLEND);
        SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA);
        SetFocus(g_popup.hEdit);
        // 커서를 텍스트 끝으로
        SendMessage(g_popup.hEdit,EM_SETSEL,0,-1);

        EnableWindow(parent, FALSE);
        RunModalLoop(g_popup.closed, [&](const MSG& m) -> bool {
            if (m.message!=WM_KEYDOWN) return false;
            if (m.hwnd!=hwnd && !IsChild(hwnd,m.hwnd)) return false;
            const RECT* btn = m.wParam==VK_RETURN ? &g_popup.okBtn
                            : m.wParam==VK_ESCAPE ? &g_popup.cancelBtn : nullptr;
            if (!btn) return false;
            SendMessage(hwnd,WM_LBUTTONUP,0,MAKELPARAM(btn->left+1, btn->top+1));
            return true;
        });
        if (IsWindow(hwnd)) CloseModal(hwnd);  // WM_QUIT으로 빠져나온 경우
        if (g_popup.hFont) { DeleteObject(g_popup.hFont); g_popup.hFont=nullptr; }
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
        SetFocus(parent);
        return g_popup.ok ? g_popup.result : L"";
    }

    // ── 메모 팝업 창 ────────────────────────────────────────────

    static std::wstring ToEditNewlines(const std::wstring& ws)
    {
        std::wstring out; out.reserve(ws.size()*2);
        for (size_t i=0;i<ws.size();++i) {
            if (ws[i]==L'\n') out+=L'\r';
            out+=ws[i];
        }
        return out;
    }

    void CloseMemoEdit()
    {
        // 인라인 에디트 더 이상 사용 안 함 – 하위 호환용 빈 함수
        if (g_hMemoEdit) { DestroyWindow(g_hMemoEdit); g_hMemoEdit=nullptr; }
        g_memoEditItem=-1;
    }

    // ── 메모 창 ─────────────────────────────────────────────────
    // 앱 공통 팝업(CleanPopup)과 같은 Fluent 스타일:
    //   흰 배경 + 옅은 테두리, 제목/부제 헤더, 테두리 있는 입력란(포커스 시 파란 테두리),
    //   회색 푸터 띠 + 상태 표시 + [닫기]/[저장] 버튼. 크기 조절 가능.
    // 단축키: Ctrl+S 저장(창 유지), Ctrl+Enter 저장 후 닫기, Esc 닫기, Ctrl+A 전체 선택

    enum { MEMO_HIT_NONE = -1, MEMO_HIT_X = 0, MEMO_HIT_CLOSE = 1, MEMO_HIT_SAVE = 2 };

    struct MemoDlg {
        HWND hwnd = nullptr, hEdit = nullptr;
        HFONT hFont = nullptr;
        HBRUSH hEditBr = nullptr;
        bool closed = false;
        bool editFocus = false;
        bool dirty = false;
        bool savedFlash = false;
        int  hover = MEMO_HIT_NONE, pressed = MEMO_HIT_NONE;
        int  charCount = 0;
        std::wstring itemName, fileName;
        std::wstring* target = nullptr;   // 저장 대상 (GroupItem::memo)
        std::wstring saved;               // 마지막으로 저장된 내용 (내부 표현 \n)
    };

    constexpr int MEMO_PAD    = 24;   // 좌우 여백
    constexpr int MEMO_HDR_H  = 78;   // 헤더 높이 (제목 + 부제)
    constexpr int MEMO_FOOT_H = 64;   // 푸터 높이
    constexpr int MEMO_BTN_W  = 92, MEMO_BTN_H = 32;
    constexpr int MEMO_MIN_W  = 420, MEMO_MIN_H = 300;
    constexpr int MEMO_GRIP   = 6;    // 크기 조절 테두리 두께

    int g_memoW = 560, g_memoH = 460;  // 마지막 창 크기 (세션 동안 유지)

    struct MemoLayout { RECT xBtn, frame, edit, closeBtn, saveBtn, status; int cw, ch; };

    MemoLayout MemoCalcLayout(HWND hwnd)
    {
        RECT rc; GetClientRect(hwnd, &rc);
        MemoLayout L;
        L.cw = rc.right; L.ch = rc.bottom;
        L.xBtn  = { L.cw - 16 - 32, 14, L.cw - 16, 46 };
        L.frame = { MEMO_PAD, MEMO_HDR_H, L.cw - MEMO_PAD, L.ch - MEMO_FOOT_H - 16 };
        // EDIT는 둥근 테두리 안쪽에 3px 들여서 배치 (모서리 곡선을 덮지 않도록)
        L.edit  = { L.frame.left + 3, L.frame.top + 3, L.frame.right - 3, L.frame.bottom - 3 };
        int by  = L.ch - MEMO_FOOT_H + (MEMO_FOOT_H - MEMO_BTN_H) / 2;
        L.saveBtn  = { L.cw - MEMO_PAD - MEMO_BTN_W, by, L.cw - MEMO_PAD, by + MEMO_BTN_H };
        L.closeBtn = { L.saveBtn.left - 8 - MEMO_BTN_W, by, L.saveBtn.left - 8, by + MEMO_BTN_H };
        L.status   = { MEMO_PAD, by, L.closeBtn.left - 12, by + MEMO_BTN_H };
        return L;
    }

    std::wstring MemoReadEdit(HWND hEdit)
    {
        int len = GetWindowTextLengthW(hEdit);
        std::wstring t(len + 1, L'\0');
        GetWindowTextW(hEdit, t.data(), len + 1);
        t.resize(len);
        // EDIT의 \r\n → 내부 표현 \n
        std::wstring norm; norm.reserve(t.size());
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] == L'\r' && i + 1 < t.size() && t[i + 1] == L'\n') continue;
            norm += t[i];
        }
        return norm;
    }

    void MemoInvalidateFooter(MemoDlg* d)
    {
        MemoLayout L = MemoCalcLayout(d->hwnd);
        RECT r = { 0, L.ch - MEMO_FOOT_H, L.cw, L.ch };
        InvalidateRect(d->hwnd, &r, FALSE);
    }

    // 현재 입력 내용을 저장 대상과 디스크에 기록
    void MemoSave(MemoDlg* d)
    {
        std::wstring text = MemoReadEdit(d->hEdit);
        if (d->target) *d->target = text;
        d->saved = text;
        d->dirty = false;
        GroupPanel::SaveGroups();
    }

    // 닫기 요청: 저장하지 않은 변경이 있으면 확인
    void MemoTryClose(MemoDlg* d)
    {
        if (d->dirty &&
            !CleanPopup::Show(d->hwnd, LS(STR_GP_MEMO_DISCARD_TITLE), LS(STR_GP_MEMO_DISCARD_MSG), true))
        {
            SetFocus(d->hEdit);
            return;
        }
        d->closed = true;
        CloseModal(d->hwnd);
    }

    void MemoRelayout(MemoDlg* d)
    {
        MemoLayout L = MemoCalcLayout(d->hwnd);
        MoveWindow(d->hEdit, L.edit.left, L.edit.top,
                   L.edit.right - L.edit.left, L.edit.bottom - L.edit.top, TRUE);
        // 글자 영역 안쪽 여백 (왼쪽 10, 위 8, 오른쪽 6)
        RECT er; GetClientRect(d->hEdit, &er);
        RECT fr = { er.left + 10, er.top + 8, er.right - 6, er.bottom - 4 };
        SendMessage(d->hEdit, EM_SETRECT, 0, (LPARAM)&fr);
        InvalidateRect(d->hwnd, nullptr, FALSE);
    }

    void MemoDrawButton(Gdiplus::Graphics& g, const Gdiplus::FontFamily& ff, const RECT& rc,
                        const wchar_t* text, bool primary, bool hov, bool down)
    {
        using namespace Gdiplus;
        RectF rf((float)rc.left + 0.5f, (float)rc.top + 0.5f,
                 (float)(rc.right - rc.left) - 1.0f, (float)(rc.bottom - rc.top) - 1.0f);
        GraphicsPath p; float d = 8.0f;
        p.AddArc(rf.X, rf.Y, d, d, 180, 90);
        p.AddArc(rf.X + rf.Width - d, rf.Y, d, d, 270, 90);
        p.AddArc(rf.X + rf.Width - d, rf.Y + rf.Height - d, d, d, 0, 90);
        p.AddArc(rf.X, rf.Y + rf.Height - d, d, d, 90, 90);
        p.CloseFigure();
        if (primary) {
            Color top = down ? Color(255,0,100,185) : hov ? Color(255,14,148,244) : Color(255,0,137,230);
            Color bot = down ? Color(255,0,88,165)  : hov ? Color(255,0,100,185)  : Color(255,0,108,196);
            LinearGradientBrush br(PointF(rf.X, rf.Y), PointF(rf.X, rf.Y + rf.Height), top, bot);
            g.FillPath(&br, &p);
            Pen hl(Color(55,255,255,255), 1.0f);
            g.DrawLine(&hl, rf.X + 5, rf.Y + 1.5f, rf.X + rf.Width - 5, rf.Y + 1.5f);
        } else {
            Color top = down ? Color(255,228,228,228) : hov ? Color(255,250,250,250) : Color(255,255,255,255);
            Color bot = down ? Color(255,220,220,220) : hov ? Color(255,232,232,232) : Color(255,242,242,242);
            LinearGradientBrush br(PointF(rf.X, rf.Y), PointF(rf.X, rf.Y + rf.Height), top, bot);
            g.FillPath(&br, &p);
            Pen bdr(Color(255,200,200,200), 1.0f);
            g.DrawPath(&bdr, &p);
        }
        Font f(&ff, 9.5f, primary ? FontStyleBold : FontStyleRegular, UnitPoint);
        SolidBrush t(primary ? Color(255,255,255,255) : Color(255,45,45,45));
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(text, -1, &f, rf, &sf, &t);
    }

    void MemoRender(MemoDlg* d, HDC hdc)
    {
        using namespace Gdiplus;
        MemoLayout L = MemoCalcLayout(d->hwnd);
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        FontFamily ff(L"Segoe UI");
        StringFormat sfL; sfL.SetLineAlignment(StringAlignmentCenter);
        sfL.SetFormatFlags(StringFormatFlagsNoWrap); sfL.SetTrimming(StringTrimmingEllipsisCharacter);
        StringFormat sfC; sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentCenter);

        // 배경 + 창 테두리
        SolidBrush bg(Color(255,255,255,255));
        g.FillRectangle(&bg, 0, 0, L.cw, L.ch);

        // ── 헤더: 항목 이름(제목) + "메모 · 파일명"(부제) ──
        Font titleF(&ff, 13, FontStyleBold, UnitPoint);
        SolidBrush titleBr(Color(255,20,20,20));
        g.DrawString(d->itemName.c_str(), -1, &titleF,
            RectF((float)MEMO_PAD, 14.0f, (float)(L.xBtn.left - 8 - MEMO_PAD), 30.0f), &sfL, &titleBr);

        std::wstring sub = std::wstring(LS(STR_GP_MEMO_HEADING)) + L"  ·  "
                         + (d->fileName.empty() ? std::wstring(LS(STR_GP_NO_PDF)) : d->fileName);
        Font subF(&ff, 9, FontStyleRegular, UnitPoint);
        SolidBrush subBr(Color(255,120,120,130));
        g.DrawString(sub.c_str(), -1, &subF,
            RectF((float)MEMO_PAD, 44.0f, (float)(L.cw - MEMO_PAD * 2), 20.0f), &sfL, &subBr);

        // × 버튼 (호버 시에만 둥근 배경)
        {
            RectF xr((float)L.xBtn.left, (float)L.xBtn.top, 32.0f, 32.0f);
            if (d->hover == MEMO_HIT_X || d->pressed == MEMO_HIT_X) {
                SolidBrush xb(d->pressed == MEMO_HIT_X ? Color(255,222,222,226) : Color(255,236,236,240));
                DrawRR(g, xr.X, xr.Y, xr.Width, xr.Height, 6.0f, &xb);
            }
            Pen xp(Color(255,95,95,105), 1.4f);
            float cx = xr.X + 16, cy = xr.Y + 16, s = 5.0f;
            g.DrawLine(&xp, cx - s, cy - s, cx + s, cy + s);
            g.DrawLine(&xp, cx + s, cy - s, cx - s, cy + s);
        }

        // ── 입력란 테두리 (포커스 시 파란색) ──
        {
            RectF fr((float)L.frame.left + 0.5f, (float)L.frame.top + 0.5f,
                     (float)(L.frame.right - L.frame.left) - 1.0f, (float)(L.frame.bottom - L.frame.top) - 1.0f);
            SolidBrush fbg(Color(255,255,255,255));
            Pen fpen(d->editFocus ? Color(255,0,120,212) : Color(255,208,208,212),
                     d->editFocus ? 1.5f : 1.0f);
            DrawRR(g, fr.X, fr.Y, fr.Width, fr.Height, 6.0f, &fbg, &fpen);
        }

        // ── 푸터 띠 ──
        SolidBrush footBg(Color(255,247,247,248));
        g.FillRectangle(&footBg, 0, L.ch - MEMO_FOOT_H, L.cw, MEMO_FOOT_H);
        Pen footLine(Color(255,232,232,234), 1.0f);
        g.DrawLine(&footLine, 0.0f, (float)(L.ch - MEMO_FOOT_H), (float)L.cw, (float)(L.ch - MEMO_FOOT_H));

        // 상태: 저장됨 / 저장되지 않음 / 글자 수
        {
            wchar_t cnt[64];
            swprintf_s(cnt, LS(STR_GP_MEMO_CHARS_FMT), d->charCount);
            std::wstring st; Color sc;
            if (d->savedFlash)      { st = std::wstring(L"✓  ") + LS(STR_GP_MEMO_SAVED); sc = Color(255,16,124,16); }
            else if (d->dirty)      { st = std::wstring(L"●  ") + LS(STR_GP_MEMO_UNSAVED) + L"   ·   " + cnt; sc = Color(255,176,100,0); }
            else                    { st = cnt; sc = Color(255,130,130,140); }
            Font stF(&ff, 8.5f, FontStyleRegular, UnitPoint);
            SolidBrush stBr(sc);
            RectF sr((float)L.status.left, (float)L.status.top,
                     (float)(L.status.right - L.status.left), (float)(L.status.bottom - L.status.top));
            g.DrawString(st.c_str(), -1, &stF, sr, &sfL, &stBr);
        }

        MemoDrawButton(g, ff, L.closeBtn, LS(STR_GP_MEMO_CLOSE), false,
                       d->hover == MEMO_HIT_CLOSE, d->pressed == MEMO_HIT_CLOSE && d->hover == MEMO_HIT_CLOSE);
        MemoDrawButton(g, ff, L.saveBtn, LS(STR_GP_MEMO_SAVE), true,
                       d->hover == MEMO_HIT_SAVE, d->pressed == MEMO_HIT_SAVE && d->hover == MEMO_HIT_SAVE);

        // 창 외곽선
        Pen border(Color(255,218,218,218), 1.0f);
        g.DrawRectangle(&border, 0.5f, 0.5f, (float)L.cw - 1.0f, (float)L.ch - 1.0f);
    }

    int MemoHit(MemoDlg* d, POINT pt)
    {
        MemoLayout L = MemoCalcLayout(d->hwnd);
        if (PtInRect(&L.xBtn, pt))     return MEMO_HIT_X;
        if (PtInRect(&L.closeBtn, pt)) return MEMO_HIT_CLOSE;
        if (PtInRect(&L.saveBtn, pt))  return MEMO_HIT_SAVE;
        return MEMO_HIT_NONE;
    }

    LRESULT CALLBACK MemoProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        MemoDlg* d = (MemoDlg*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            d = (MemoDlg*)((CREATESTRUCTW*)lParam)->lpCreateParams;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)d);
            d->hwnd = hWnd;
        }
        if (!d) return DefWindowProcW(hWnd, msg, wParam, lParam);

        switch (msg)
        {
        case WM_NCCALCSIZE:
            if (wParam) return 0;  // 클라이언트 = 창 전체 (WS_THICKFRAME 테두리 숨김)
            break;

        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            RECT rc; GetClientRect(hWnd, &rc);
            bool l = pt.x < MEMO_GRIP, r = pt.x >= rc.right - MEMO_GRIP;
            bool t = pt.y < MEMO_GRIP, b = pt.y >= rc.bottom - MEMO_GRIP;
            if (t && l) return HTTOPLEFT;    if (t && r) return HTTOPRIGHT;
            if (b && l) return HTBOTTOMLEFT; if (b && r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT; if (r) return HTRIGHT;
            if (t) return HTTOP;  if (b) return HTBOTTOM;
            if (pt.y < MEMO_HDR_H - 8 && MemoHit(d, pt) != MEMO_HIT_X) return HTCAPTION;
            return HTCLIENT; }

        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize = { MEMO_MIN_W, MEMO_MIN_H };
            return 0; }

        case WM_SIZE:
            if (d->hEdit) MemoRelayout(d);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            MemoRender(d, mem);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
            EndPaint(hWnd, &ps);
            return 0; }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, RGB(255,255,255));
            SetTextColor(hdc, RGB(28,28,32));
            return (LRESULT)d->hEditBr; }

        case WM_COMMAND:
            if ((HWND)lParam == d->hEdit) {
                switch (HIWORD(wParam)) {
                case EN_CHANGE: {
                    std::wstring now = MemoReadEdit(d->hEdit);
                    d->charCount = (int)now.size();
                    d->dirty = (now != d->saved);
                    if (d->dirty) d->savedFlash = false;
                    MemoInvalidateFooter(d);
                    break; }
                case EN_SETFOCUS: case EN_KILLFOCUS:
                    d->editFocus = HIWORD(wParam) == EN_SETFOCUS;
                    InvalidateRect(hWnd, nullptr, FALSE);
                    break;
                }
            }
            return 0;

        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int h = MemoHit(d, pt);
            if (h != d->hover) { d->hover = h; InvalidateRect(hWnd, nullptr, FALSE); }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            return 0; }

        case WM_MOUSELEAVE:
            if (d->hover != MEMO_HIT_NONE) { d->hover = MEMO_HIT_NONE; InvalidateRect(hWnd, nullptr, FALSE); }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && d->hover != MEMO_HIT_NONE) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            d->pressed = MemoHit(d, pt);
            if (d->pressed != MEMO_HIT_NONE) { SetCapture(hWnd); InvalidateRect(hWnd, nullptr, FALSE); }
            return 0; }

        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int p = d->pressed;
            d->pressed = MEMO_HIT_NONE;
            if (GetCapture() == hWnd) ReleaseCapture();
            InvalidateRect(hWnd, nullptr, FALSE);
            if (p == MEMO_HIT_NONE || p != MemoHit(d, pt)) return 0;  // 버튼 밖에서 떼면 취소
            if (p == MEMO_HIT_SAVE) { MemoSave(d); d->closed = true; CloseModal(hWnd); }
            else MemoTryClose(d);  // × 또는 [닫기]
            return 0; }

        case WM_TIMER:
            if (wParam == 1) {
                KillTimer(hWnd, 1);
                d->savedFlash = false;
                MemoInvalidateFooter(d);
            }
            return 0;

        case WM_CLOSE:
            MemoTryClose(d);
            return 0;

        case WM_DESTROY: {
            RECT wr; GetWindowRect(hWnd, &wr);
            g_memoW = wr.right - wr.left; g_memoH = wr.bottom - wr.top;
            d->closed = true;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
            return 0; }
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    void ShowMemoDialog(HWND parent, int groupIdx, int itemIdx)
    {
        if (groupIdx<0 || groupIdx>=(int)g_groups.size()) return;
        if (itemIdx<0  || itemIdx>=(int)g_groups[groupIdx].items.size()) return;
        auto& item = g_groups[groupIdx].items[itemIdx];

        static bool sReg = false;
        const wchar_t* MEMO_CLASS = L"GpMemoDlgClass2";
        if (!sReg) {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = MemoProc; wc.hInstance = hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr; wc.lpszClassName = MEMO_CLASS;
            RegisterClassExW(&wc);
            sReg = true;
        }

        MemoDlg d;
        d.itemName = item.name;
        d.fileName = item.pdfPath.empty() ? L"" : FileNameOf(item.pdfPath);
        d.target   = &item.memo;
        d.saved    = item.memo;
        d.charCount = (int)item.memo.size();
        d.hEditBr  = CreateSolidBrush(RGB(255,255,255));
        d.hFont    = CreateFontW(-19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        // 부모 창 가운데, 화면 작업 영역을 넘지 않게
        RECT pr; GetWindowRect(parent, &pr);
        int w = g_memoW, h = g_memoH;
        HMONITOR mon = MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        bool hasMon = GetMonitorInfoW(mon, &mi) != FALSE;
        if (hasMon) {
            int maxW = mi.rcWork.right - mi.rcWork.left, maxH = mi.rcWork.bottom - mi.rcWork.top;
            if (w > maxW) w = maxW;
            if (h > maxH) h = maxH;
        }
        int x = pr.left + (pr.right - pr.left - w) / 2;
        int y = pr.top  + (pr.bottom - pr.top - h) / 2;
        if (hasMon) {
            if (x < mi.rcWork.left) x = mi.rcWork.left;
            if (y < mi.rcWork.top)  y = mi.rcWork.top;
            if (x + w > mi.rcWork.right)  x = mi.rcWork.right - w;
            if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
        }

        HWND hwnd = CreateWindowExW(WS_EX_LAYERED, MEMO_CLASS, nullptr,
            WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
            x, y, w, h, parent, nullptr, hInst, &d);
        if (!hwnd) { DeleteObject(d.hEditBr); DeleteObject(d.hFont); return; }

        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        MARGINS margins = { 1, 1, 1, 1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

        std::wstring editText = ToEditNewlines(item.memo);
        d.hEdit = CreateWindowExW(0, L"EDIT", editText.c_str(),
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
            0, 0, 10, 10, hwnd, nullptr, hInst, nullptr);
        SendMessage(d.hEdit, WM_SETFONT, (WPARAM)d.hFont, TRUE);
        SendMessage(d.hEdit, EM_SETLIMITTEXT, 0, 0);  // 기본 32K 제한 해제
        MemoRelayout(&d);

        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        ShowWindow(hwnd, SW_SHOW);
        AnimateWindow(hwnd, 160, AW_BLEND);
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        SetForegroundWindow(hwnd);
        SetFocus(d.hEdit);
        SendMessage(d.hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);  // 커서를 끝으로
        SendMessage(d.hEdit, EM_SCROLLCARET, 0, 0);

        EnableWindow(parent, FALSE);
        RunModalLoop(d.closed, [&](const MSG& m) -> bool {
            if (m.message != WM_KEYDOWN) return false;
            if (m.hwnd != hwnd && !IsChild(hwnd, m.hwnd)) return false;
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (m.wParam == VK_ESCAPE) { MemoTryClose(&d); return true; }
            if (ctrl && m.wParam == 'S') {
                MemoSave(&d);
                d.savedFlash = true;
                MemoInvalidateFooter(&d);
                SetTimer(hwnd, 1, 1500, nullptr);
                return true;
            }
            if (ctrl && m.wParam == VK_RETURN) {
                MemoSave(&d); d.closed = true; CloseModal(hwnd);
                return true;
            }
            if (ctrl && m.wParam == 'A') { SendMessage(d.hEdit, EM_SETSEL, 0, -1); return true; }
            return false;
        });
        if (IsWindow(hwnd)) { d.closed = true; CloseModal(hwnd); }  // WM_QUIT으로 빠져나온 경우

        DeleteObject(d.hFont);
        DeleteObject(d.hEditBr);
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
        SetFocus(parent);
    }

    void OpenMemoEdit(HWND hParent, int itemIdx, const RECT& /*clientRect*/)
    {
        // 레거시 호출 → 새 팝업으로 위임
        ShowMemoDialog(hParent, g_selGroup, itemIdx);
    }

    // ── 레이아웃 (그리기·히트 테스트 공용) ─────────────────────
    // 패널 구성 (위→아래): 헤더 | 그룹 선택 상자 | 항목 목록 | 하단 [+ 항목 추가] 버튼

    // 헤더 오른쪽 × 버튼
    RECT CloseBtnRect(const RECT& pr)
    {
        const int s = 22, y = pr.top + (GROUP_HEADER_H - s) / 2;
        return { pr.right - 8 - s, y, pr.right - 8, y + s };
    }

    // 그룹 선택 행 전체 (클릭 영역)
    RECT HitDropdownBtn(const RECT& pr)
    {
        return { pr.left, pr.top + GROUP_HEADER_H, pr.right, pr.top + GROUP_HEADER_H + GROUP_TAB_H };
    }

    // 그룹 선택 상자 (행 안쪽에 여백을 둔 둥근 상자, 그리기용)
    RECT ComboRect(const RECT& pr)
    {
        RECT r = HitDropdownBtn(pr);
        return { r.left + 10, r.top + 6, r.right - 10, r.bottom - 6 };
    }

    // 항목 목록 영역
    RECT ListRect(const RECT& pr)
    {
        return { pr.left, pr.top + GROUP_HEADER_H + GROUP_TAB_H, pr.right, pr.bottom - GROUP_FOOTER_H };
    }

    // 하단 [+ 항목 추가] 버튼
    RECT FooterBtnRect(const RECT& pr)
    {
        return { pr.left + 8, pr.bottom - 4 - 28, pr.right - 8, pr.bottom - 4 };
    }

    constexpr int LIST_PAD_TOP = 4;   // 목록 위 여백

    int MaxItemScroll(const RECT& pr)
    {
        if (g_selGroup < 0 || g_selGroup >= (int)g_groups.size()) return 0;
        RECT L = ListRect(pr);
        int total = LIST_PAD_TOP + (int)g_groups[g_selGroup].items.size() * GROUP_ITEM_H + 4;
        int m = total - (L.bottom - L.top);
        return m > 0 ? m : 0;
    }

    void ClampItemScroll(const RECT& pr)
    {
        int m = MaxItemScroll(pr);
        if (g_scrollItem > m) g_scrollItem = m;
        if (g_scrollItem < 0) g_scrollItem = 0;
    }

    // i번째 항목 줄의 위쪽 y
    int ItemTop(const RECT& pr, int i)
    {
        return ListRect(pr).top + LIST_PAD_TOP + i * GROUP_ITEM_H - g_scrollItem;
    }

    // 항목 줄 오른쪽 아이콘 버튼: which 0 = PDF 연결(등록), 1 = 메모
    RECT ItemBtnRect(const RECT& pr, int iy, int which)
    {
        const int s = 26, cy = iy + GROUP_ITEM_H / 2;
        int right = pr.right - 10 - (which == 0 ? s + 2 : 0);
        return { right - s, cy - s / 2, right, cy + s / 2 };
    }

    // ── 드롭다운 카드 (그룹 선택 상자 바로 아래에 뜨는 목록) ──

    int DropListW(const RECT& pr) { return (pr.right - pr.left) - 20; }
    int DropTop(const RECT& pr)   { return ComboRect(pr).bottom + 4; }

    // 패널 안(하단 버튼 위)에 들어가는 드롭다운 최대 행 수
    int DropMaxRows(const RECT& pr)
    {
        int avail = (pr.bottom - GROUP_FOOTER_H) - DropTop(pr) - 8 - 4;
        int rows  = avail / GROUP_DROP_ROW_H;
        return rows < 1 ? 1 : rows;
    }

    // 실제로 보이는 드롭다운 행 수
    int DropVisibleRows(const RECT& pr)
    {
        int n = (int)g_groups.size(), m = DropMaxRows(pr);
        return n < m ? n : m;
    }

    void ClampDropScroll(const RECT& pr)
    {
        int maxS = (int)g_groups.size() - DropMaxRows(pr);
        if (maxS < 0) maxS = 0;
        if (g_scrollTab > maxS) g_scrollTab = maxS;
        if (g_scrollTab < 0)    g_scrollTab = 0;
    }

    RECT DropCardRect(const RECT& pr)
    {
        int n = DropVisibleRows(pr), t = DropTop(pr);
        return { pr.left + 10, t, pr.left + 10 + DropListW(pr), t + n * GROUP_DROP_ROW_H + 8 };
    }

    // 드롭다운 카드의 row번째 보이는 행 영역
    RECT HitDropdownItem(const RECT& pr, int row)
    {
        RECT c = DropCardRect(pr);
        int t = c.top + 4 + row * GROUP_DROP_ROW_H;
        return { c.left + 4, t, c.right - 4, t + GROUP_DROP_ROW_H };
    }

    // ── 히트 테스트 ─────────────────────────────────────────────

    // HitTab: 드롭다운 목록에서 가리킨 그룹 인덱스 반환 (-1=없음)
    int HitTab(int x, int y, const RECT& pr)
    {
        if (!g_dropOpen) return -1;
        if (g_groups.empty()) return -1;
        int n = DropVisibleRows(pr);
        for (int row = 0; row < n; ++row) {
            RECT r = HitDropdownItem(pr, row);
            if (PtInRect(&r, {x, y})) {
                int gi = g_scrollTab + row;
                return gi < (int)g_groups.size() ? gi : -1;
            }
        }
        return -1;
    }

    // 반환: 항목 인덱스(-1=없음), sub=0등록/1메모/-1=항목자체
    int HitItem(int x, int y, const RECT& pr, int& sub)
    {
        sub = -1;
        if (g_selGroup < 0 || g_selGroup >= (int)g_groups.size()) return -1;
        auto& grp = g_groups[g_selGroup];
        RECT L = ListRect(pr);
        // 목록 영역 밖(헤더·그룹 선택·하단 버튼)은 스크롤로 가려진 항목이 있어도 무시
        if (y < L.top || y >= L.bottom || x < pr.left || x >= pr.right) return -1;

        for (int i=0; i<(int)grp.items.size(); ++i) {
            int iy = ItemTop(pr, i);
            if (iy > L.bottom) break;
            if (y >= iy && y < iy + GROUP_ITEM_H) {
                RECT rm = ItemBtnRect(pr, iy, 1);
                if (PtInRect(&rm, {x,y})) { sub=1; return i; }
                RECT rr = ItemBtnRect(pr, iy, 0);
                if (PtInRect(&rr, {x,y})) { sub=0; return i; }
                return i; // 항목 자체
            }
        }
        return -1;
    }

    bool HitClose(int x, int y, const RECT& pr)
    {
        RECT r = CloseBtnRect(pr);
        return PtInRect(&r, {x,y}) != FALSE;
    }

    bool HitFooter(int x, int y, const RECT& pr)
    {
        RECT r = FooterBtnRect(pr);
        return PtInRect(&r, {x,y}) != FALSE;
    }

    // 하단 버튼이 하는 일: 그룹이 선택돼 있으면 항목 추가, 아니면 그룹 추가
    bool FooterAddsItem() { return g_selGroup >= 0 && g_selGroup < (int)g_groups.size(); }

    std::wstring TrimWs(const std::wstring& s)
    {
        size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
        return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
    }

    // 하단 [+ 항목 추가] / [+ 그룹 추가] 버튼 동작
    void AddFromFooter(HWND hWnd, const RECT& pr)
    {
        if (FooterAddsItem()) {
            std::wstring name = TrimWs(ShowInputPopup(hWnd, LS(STR_GP_DLG_ADD_ITEM), LS(STR_GP_DLG_ITEM_NAME)));
            if (name.empty()) return;
            GroupItem it; it.name = name;
            g_groups[g_selGroup].items.push_back(it);
            g_scrollItem = 1 << 30;   // 새 항목이 보이도록 맨 아래로
            ClampItemScroll(pr);
        } else {
            std::wstring name = TrimWs(ShowInputPopup(hWnd, LS(STR_GP_DLG_ADD_GROUP), LS(STR_GP_DLG_GROUP_NAME)));
            if (name.empty()) return;
            for (auto& gd : g_groups)
                if (gd.name == name) {
                    CleanPopup::Show(hWnd, LS(STR_GP_DLG_DUP_TITLE), LS(STR_GP_DLG_DUP));
                    return;
                }
            GroupDef gd; gd.name = name;
            g_groups.push_back(gd);
            g_selGroup = (int)g_groups.size() - 1;
            g_scrollItem = 0;
        }
        GroupPanel::SaveGroups();
    }

} // anonymous namespace

// ── 공개 API ────────────────────────────────────────────────────

RECT GroupPanel::GetPanelRect(const RECT& clientRect)
{
    int top    = GetBelowToolbarY();
    int bottom = clientRect.bottom - STATUSBAR_HEIGHT;
    int left   = clientRect.right  - GROUP_PANEL_WIDTH;
    return {left, top, clientRect.right, bottom};
}

bool GroupPanel::IsVisible() { return g_visible; }

// ── 열기/닫기 슬라이드 애니메이션 ────────────────────────────────
// 사이드바(SidebarToggleAnimated)와 같은 방식·속도:
//   레이아웃(뷰포트 폭)은 즉시 바꾸고, 패널 그림만 오른쪽에서 밀려 들어오거나 빠져나간다.
//   닫을 때는 이미 넓어진 페이지 위로 패널이 빠져나가므로 다시 맞추는 시점을 늦출 필요가 없다.
namespace
{
    constexpr UINT_PTR  GP_ANIM_TIMER = 0x4750;  // 'GP' (메인 창 타이머 ID와 겹치지 않게)
    constexpr ULONGLONG GP_ANIM_MS    = 160;     // 사이드바와 동일
    constexpr int       GP_SHADOW_W   = 8;       // 움직이는 동안 왼쪽에 드리우는 그림자 폭

    void TipHide();  // 아래 툴팁 구역에서 정의

    bool      g_animating   = false;
    bool      g_animClosing = false;  // 닫히는 중 (g_visible은 이미 false)
    ULONGLONG g_animStart   = 0;
    int       g_animOffset  = 0;      // 0 = 제자리, GROUP_PANEL_WIDTH = 화면 밖

    // 애니메이션 중 다시 그려야 하는 영역: 패널 띠 + 왼쪽 그림자
    void InvalidatePanelBand(HWND hWnd)
    {
        RECT cr; GetClientRect(hWnd, &cr);
        RECT pr = GroupPanel::GetPanelRect(cr);
        pr.left -= GP_SHADOW_W;
        InvalidateRect(hWnd, &pr, FALSE);
    }

    void StopPanelAnim(HWND hWnd)
    {
        KillTimer(hWnd, GP_ANIM_TIMER);
        g_animating = g_animClosing = false;
        g_animOffset = 0;
        InvalidateRect(hWnd, nullptr, FALSE);  // 그림자 등 잔상 정리
    }

    void CALLBACK PanelAnimTimerProc(HWND hWnd, UINT, UINT_PTR, DWORD)
    {
        if (!g_animating) { KillTimer(hWnd, GP_ANIM_TIMER); return; }
        double t = (double)(GetTickCount64() - g_animStart) / (double)GP_ANIM_MS;
        if (t >= 1.0) { StopPanelAnim(hWnd); return; }
        double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);   // ease-out cubic
        double shown = g_animClosing ? (1.0 - e) : e;
        g_animOffset = GROUP_PANEL_WIDTH - (int)(GROUP_PANEL_WIDTH * shown);
        InvalidatePanelBand(hWnd);
    }

    // Windows 설정 "애니메이션 효과"가 꺼져 있으면 애니메이션 생략
    bool AnimationsEnabled()
    {
        BOOL on = TRUE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return true;
        return on != FALSE;
    }
}

bool GroupPanel::IsAnimating() { return g_animating; }

void GroupPanel::Toggle(HWND hParent)
{
    // 진행 중인 애니메이션은 즉시 마무리
    if (g_animating) StopPanelAnim(hParent);
    TipHide();

    g_visible = !g_visible;
    if (!g_visible) { CloseMemoEdit(); g_dropOpen = false; }
    else RefreshMissing(g_selGroup);  // 패널이 닫혀 있는 동안 파일이 옮겨졌을 수 있음

    if (AnimationsEnabled()) {
        g_animating   = true;
        g_animClosing = !g_visible;
        g_animOffset  = g_visible ? GROUP_PANEL_WIDTH : 0;
        g_animStart   = GetTickCount64();
        SetTimer(hParent, GP_ANIM_TIMER, 15, PanelAnimTimerProc);
    }
    InvalidateRect(hParent, nullptr, FALSE);
}

// ── 그룹/항목 관리 창 ────────────────────────────────────────────
void GroupPanel::ShowManageDialog(HWND parent)
{
    // ── 창 크기 / 레이아웃 상수 ─────────────────────────────────
    constexpr int DW = 820, DH = 580;
    constexpr int HDR_H = 48;          // 상단 헤더
    constexpr int FOOT_H = 52;         // 하단 버튼 행
    constexpr int LEFT_W = 220;        // 왼쪽 그룹 열 너비
    constexpr int ITEM_H = 36;         // 행 높이
    constexpr int BTN_W  = 88, BTN_H = 30;
    constexpr int SBTN_W = 100, SBTN_H = 28; // 소형 버튼

    // ── 창 클래스 등록 (최초 1회) ───────────────────────────────
    static bool sClassReg = false;
    const wchar_t* MGR_CLASS = L"GpMgrClass";

    // ── 관리 창 내부 상태 (람다로 공유) ─────────────────────────
    struct MgrState {
        int  selGroup  = -1;
        int  selItem   = -1;
        int  scrollGrp = 0;
        int  scrollItm = 0;
        bool closed    = false;
        HWND hwnd      = nullptr;

        // 히트 영역 캐시
        struct HitArea { RECT rc; int idx; int kind; };
        std::vector<HitArea> grpAreas;
        std::vector<HitArea> itmAreas;

        int hoverGrp  = -1;
        int hoverItm  = -1;
        int hoverBtn  = -1;  // 0=그룹추가 1=그룹삭제 2=항목추가 3=항목삭제 4=닫기(푸터) 5=×(헤더)
    };
    static MgrState* sp = nullptr;
    MgrState st;
    st.selGroup = g_selGroup;
    RefreshMissing(-1);  // 관리 창은 모든 그룹을 보여주므로 전체 확인
    sp = &st;

    // ── 그리기 함수 ─────────────────────────────────────────────
    auto Render = [&](HDC hdc)
    {
        using namespace Gdiplus;
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        FontFamily ff(L"Segoe UI");
        StringFormat sfC; sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentCenter);
        StringFormat sfL; sfL.SetLineAlignment(StringAlignmentCenter);
        StringFormat sfLN; sfLN.SetLineAlignment(StringAlignmentCenter);
        sfLN.SetFormatFlags(StringFormatFlagsNoWrap); sfLN.SetTrimming(StringTrimmingEllipsisCharacter);

        // 배경
        SolidBrush wBg(Color(255,255,255,255));
        g.FillRectangle(&wBg, 0.0f, 0.0f, (float)DW, (float)DH);

        // ── 헤더 ──────────────────────────────────────────────
        SolidBrush hdrBg(Color(255,243,243,243));
        g.FillRectangle(&hdrBg, 0.0f, 0.0f, (float)DW, (float)HDR_H);
        Pen hdrLine(Color(255,220,220,220), 1.0f);
        g.DrawLine(&hdrLine, 0.0f,(float)HDR_H,(float)DW,(float)HDR_H);
        Font titleF(&ff, 12, FontStyleBold, UnitPoint);
        SolidBrush titleBr(Color(255,30,30,30));
        g.DrawString(LS(STR_GP_MGR_TITLE), -1, &titleF,
                     RectF(16, 0, (float)(DW-60), (float)HDR_H), &sfL, &titleBr);

        // 닫기 버튼 (헤더 오른쪽)
        {
            int bx = DW-40, by2 = (HDR_H-24)/2;
            bool hov = (sp->hoverBtn==5);
            Color bc = hov ? Color(255,220,80,80) : Color(255,200,60,60);
            SolidBrush bb(bc);
            g.FillEllipse(&bb,(float)bx,(float)by2,24.0f,24.0f);
            Font cf(&ff,10,FontStyleBold,UnitPoint);
            SolidBrush ct(Color(255,255,255,255));
            g.DrawString(L"\u00D7",-1,&cf,RectF((float)bx,(float)by2,24,24),&sfC,&ct);
        }

        // ── 세로 구분선 ──────────────────────────────────────
        Pen vDiv(Color(255,220,220,220), 1.0f);
        g.DrawLine(&vDiv,(float)LEFT_W,(float)HDR_H,(float)LEFT_W,(float)(DH-FOOT_H));

        // ── 왼쪽: 그룹 목록 ─────────────────────────────────
        int gListTop = HDR_H;
        int gListH   = DH - HDR_H - FOOT_H;
        // 레이블
        Font lblF(&ff,8,FontStyleBold,UnitPoint);
        SolidBrush lblBr(Color(255,80,80,80));
        g.DrawString(LS(STR_GP_GRP_LIST),-1,&lblF,
            RectF(12,(float)gListTop,LEFT_W-24,22),&sfL,&lblBr);

        // 클립
        Region oldClip; g.GetClip(&oldClip);
        g.SetClip(RectF(0,(float)(gListTop+22),(float)LEFT_W,(float)(gListH-22)));

        sp->grpAreas.clear();
        for (int i=0; i<(int)g_groups.size(); ++i) {
            int iy = gListTop+22 + i*ITEM_H - sp->scrollGrp;
            if (iy+ITEM_H < gListTop+22 || iy > DH-FOOT_H) continue;
            bool sel = (i==sp->selGroup);
            bool hov = (i==sp->hoverGrp);
            Color bg = sel ? Color(255,210,228,255)
                           : hov ? Color(255,235,240,248) : Color(255,255,255,255);
            SolidBrush br(bg);
            g.FillRectangle(&br,1.0f,(float)iy,(float)(LEFT_W-2),(float)ITEM_H);
            if (sel) {
                SolidBrush accent(Color(255,0,112,200));
                g.FillRectangle(&accent,1.0f,(float)iy,4.0f,(float)ITEM_H);
            }
            Pen rowPen(Color(255,230,230,230),1.0f);
            g.DrawLine(&rowPen,1.0f,(float)(iy+ITEM_H),(float)(LEFT_W-1),(float)(iy+ITEM_H));
            Font nf(&ff,9,sel?FontStyleBold:FontStyleRegular,UnitPoint);
            SolidBrush nBr(sel?Color(255,0,90,180):Color(255,35,38,55));
            int cnt = (int)g_groups[i].items.size();
            std::wstring label = g_groups[i].name + L" (" + std::to_wstring(cnt) + L")";
            g.DrawString(label.c_str(),-1,&nf,RectF(10,(float)iy,(float)(LEFT_W-16),(float)ITEM_H),&sfLN,&nBr);
            RECT hitRc = {0,(int)iy,LEFT_W,(int)(iy+ITEM_H)};
            sp->grpAreas.push_back({hitRc,i,-1});
        }
        g.SetClip(&oldClip);

        if (g_groups.empty()) {
            Font ef(&ff,8,FontStyleRegular,UnitPoint);
            SolidBrush eb(Color(255,160,165,190));
            g.DrawString(LS(STR_GP_NO_GROUPS),-1,&ef,
                RectF(0,(float)(gListTop+22),(float)LEFT_W,(float)(gListH-22)),&sfC,&eb);
        }

        // 그룹 추가/삭제 버튼
        {
            int by = DH-FOOT_H+11;
            bool ha = (sp->hoverBtn==0);
            bool hd = (sp->hoverBtn==1);
            auto smallBtn = [&](int bx, const wchar_t* txt, bool hov2, bool del) {
                GraphicsPath p; float d=6.0f;
                float fx=(float)bx,fy=(float)by,fw=88.0f,fh=30.0f;
                p.AddArc(fx,fy,d,d,180,90); p.AddArc(fx+fw-d,fy,d,d,270,90);
                p.AddArc(fx+fw-d,fy+fh-d,d,d,0,90); p.AddArc(fx,fy+fh-d,d,d,90,90);
                p.CloseFigure();
                if (del) {
                    Color topC = hov2?Color(255,220,50,50) :Color(255,200,30,30);
                    Color botC = hov2?Color(255,180,20,20) :Color(255,160,10,10);
                    LinearGradientBrush grad(PointF(fx,fy),PointF(fx,fy+fh),topC,botC);
                    g.FillPath(&grad,&p);
                    Pen hl(Color(55,255,200,200),1.0f);
                    g.DrawLine(&hl,fx+5,fy+1.5f,fx+fw-5,fy+1.5f);
                } else {
                    Color topC = hov2?Color(255,14,148,244):Color(255,0,137,230);
                    Color botC = hov2?Color(255,0,100,185) :Color(255,0,108,196);
                    LinearGradientBrush grad(PointF(fx,fy),PointF(fx,fy+fh),topC,botC);
                    g.FillPath(&grad,&p);
                    Pen hl(Color(55,255,255,255),1.0f);
                    g.DrawLine(&hl,fx+5,fy+1.5f,fx+fw-5,fy+1.5f);
                }
                Font f(&ff,9,FontStyleBold,UnitPoint);
                SolidBrush t(Color(255,255,255,255));
                g.DrawString(txt,-1,&f,RectF(fx,fy,fw,fh),&sfC,&t);
            };
            smallBtn(8,   LS(STR_GP_ADD_GROUP), ha, false);
            smallBtn(102, LS(STR_GP_DEL_GROUP), hd, true);
        }

        // ── 오른쪽: 항목 목록 ───────────────────────────────
        int rx = LEFT_W+1;
        int rw = DW - LEFT_W - 1;
        // 레이블
        if (sp->selGroup>=0 && sp->selGroup<(int)g_groups.size()) {
            std::wstring hdr = g_groups[sp->selGroup].name + L"  " + LS(STR_GP_ITEMS_LABEL);
            g.DrawString(hdr.c_str(),-1,&lblF,
                RectF((float)(rx+12),(float)gListTop,(float)(rw-24),22),&sfL,&lblBr);
        } else {
            g.DrawString(LS(STR_GP_SEL_GROUP),-1,&lblF,
                RectF((float)(rx+12),(float)gListTop,(float)(rw-24),22),&sfL,&lblBr);
        }

        // 항목 목록
        g.SetClip(RectF((float)rx,(float)(gListTop+22),(float)rw,(float)(gListH-22)));
        sp->itmAreas.clear();
        if (sp->selGroup>=0 && sp->selGroup<(int)g_groups.size()) {
            auto& grp = g_groups[sp->selGroup];
            if (grp.items.empty()) {
                Font ef(&ff,8,FontStyleRegular,UnitPoint);
                SolidBrush eb(Color(255,160,165,190));
                g.DrawString(LS(STR_GP_NO_ITEMS),
                    -1,&ef,RectF((float)rx,(float)(gListTop+22),(float)rw,(float)(gListH-22)),&sfC,&eb);
            } else {
                for (int i=0; i<(int)grp.items.size(); ++i) {
                    int iy = gListTop+22 + i*ITEM_H - sp->scrollItm;
                    if (iy+ITEM_H < gListTop+22 || iy > DH-FOOT_H) continue;
                    bool sel = (i==sp->selItem);
                    bool hov = (i==sp->hoverItm);
                    Color bg = sel ? Color(255,210,228,255)
                                   : hov ? Color(255,235,240,248) : Color(255,255,255,255);
                    SolidBrush br(bg);
                    g.FillRectangle(&br,(float)(rx+1),(float)iy,(float)(rw-2),(float)ITEM_H);
                    if (sel) {
                        SolidBrush accent(Color(255,0,112,200));
                        g.FillRectangle(&accent,(float)(rx+1),(float)iy,4.0f,(float)ITEM_H);
                    }

                    // 항목 이름
                    Font nf2(&ff,9,FontStyleBold,UnitPoint);
                    SolidBrush nBr2(Color(255,25,30,55));
                    StringFormat sfName; sfName.SetLineAlignment(StringAlignmentCenter);
                    sfName.SetFormatFlags(StringFormatFlagsNoWrap); sfName.SetTrimming(StringTrimmingEllipsisCharacter);
                    float nameW = (float)(rw-12) * 0.42f;
                    g.DrawString(grp.items[i].name.c_str(),-1,&nf2,
                        RectF((float)(rx+12),(float)iy,nameW,(float)ITEM_H),&sfName,&nBr2);

                    // 연결된 파일명 (항목 이름 오른쪽)
                    bool hasPdf = !grp.items[i].pdfPath.empty();
                    std::wstring fileLabel;
                    if (hasPdf) {
                        auto& p2 = grp.items[i].pdfPath;
                        auto pos = p2.find_last_of(L"\\/");
                        fileLabel = pos!=std::wstring::npos ? p2.substr(pos+1) : p2;
                        if (grp.items[i].missing)
                            fileLabel = L"⚠ " + fileLabel + L"  (" + LS(STR_GP_FILE_MISSING) + L")";
                    } else {
                        fileLabel = LS(STR_GP_NO_PDF);
                    }
                    Font pf(&ff,8,FontStyleRegular,UnitPoint);
                    SolidBrush pBr(!hasPdf ? Color(255,170,175,195)
                                 : grp.items[i].missing ? Color(255,190,60,60)
                                                        : Color(255,60,100,180));
                    StringFormat sfFile; sfFile.SetLineAlignment(StringAlignmentCenter);
                    sfFile.SetFormatFlags(StringFormatFlagsNoWrap); sfFile.SetTrimming(StringTrimmingEllipsisCharacter);
                    float fileX = (float)(rx+12) + nameW + 8;
                    float fileW = (float)(rw-20) - nameW - 8;
                    g.DrawString(fileLabel.c_str(),-1,&pf,
                        RectF(fileX,(float)iy,fileW,(float)ITEM_H),&sfFile,&pBr);

                    // 구분선
                    Pen rowPen2(Color(255,230,230,230),1.0f);
                    g.DrawLine(&rowPen2,(float)(rx+1),(float)(iy+ITEM_H),(float)(rx+rw-2),(float)(iy+ITEM_H));
                    RECT hitRc2={(int)rx,(int)iy,(int)(rx+rw),(int)(iy+ITEM_H)};
                    sp->itmAreas.push_back({hitRc2,i,-1});
                }
            }
        }
        g.SetClip(&oldClip);

        // 항목 추가/삭제 버튼
        {
            bool en = (sp->selGroup>=0 && sp->selGroup<(int)g_groups.size());
            int by = DH-FOOT_H+11;
            bool ha2 = (sp->hoverBtn==2);
            bool hd2 = (sp->hoverBtn==3);
            auto smallBtn2 = [&](int bx, const wchar_t* txt, bool hov2, bool del, bool enabled) {
                GraphicsPath p; float d=6.0f;
                float fx=(float)bx,fy=(float)by,fw=100.0f,fh=30.0f;
                p.AddArc(fx,fy,d,d,180,90); p.AddArc(fx+fw-d,fy,d,d,270,90);
                p.AddArc(fx+fw-d,fy+fh-d,d,d,0,90); p.AddArc(fx,fy+fh-d,d,d,90,90);
                p.CloseFigure();
                if (!enabled) {
                    SolidBrush b(Color(255,190,195,210)); g.FillPath(&b,&p);
                } else if (del) {
                    Color topC = hov2?Color(255,220,50,50) :Color(255,200,30,30);
                    Color botC = hov2?Color(255,180,20,20) :Color(255,160,10,10);
                    LinearGradientBrush grad(PointF(fx,fy),PointF(fx,fy+fh),topC,botC);
                    g.FillPath(&grad,&p);
                    Pen hl(Color(55,255,200,200),1.0f);
                    g.DrawLine(&hl,fx+5,fy+1.5f,fx+fw-5,fy+1.5f);
                } else {
                    Color topC = hov2?Color(255,14,148,244):Color(255,0,137,230);
                    Color botC = hov2?Color(255,0,100,185) :Color(255,0,108,196);
                    LinearGradientBrush grad(PointF(fx,fy),PointF(fx,fy+fh),topC,botC);
                    g.FillPath(&grad,&p);
                    Pen hl(Color(55,255,255,255),1.0f);
                    g.DrawLine(&hl,fx+5,fy+1.5f,fx+fw-5,fy+1.5f);
                }
                Font f2(&ff,9,FontStyleBold,UnitPoint);
                SolidBrush t(Color(255,255,255,255));
                g.DrawString(txt,-1,&f2,RectF(fx,fy,fw,fh),&sfC,&t);
            };
            smallBtn2(LEFT_W+12, LS(STR_GP_ADD_ITEM), ha2, false, en);
            smallBtn2(LEFT_W+118, LS(STR_GP_DEL_ITEM), hd2, true,
                      en && sp->selItem>=0);
        }

        // 구분선 (푸터 위)
        Pen footLine(Color(255,220,220,220),1.0f);
        g.DrawLine(&footLine,0.0f,(float)(DH-FOOT_H),(float)DW,(float)(DH-FOOT_H));

        // 닫기 버튼 (푸터 오른쪽)
        {
            int bx=DW-108, by=DH-FOOT_H+11;
            bool hov=(sp->hoverBtn==4);
            GraphicsPath p; float d=6.0f;
            float fx=(float)bx,fy=(float)by,fw=96.0f,fh=30.0f;
            p.AddArc(fx,fy,d,d,180,90); p.AddArc(fx+fw-d,fy,d,d,270,90);
            p.AddArc(fx+fw-d,fy+fh-d,d,d,0,90); p.AddArc(fx,fy+fh-d,d,d,90,90);
            p.CloseFigure();
            Color topC = hov?Color(255,14,148,244):Color(255,0,137,230);
            Color botC = hov?Color(255,0,100,185) :Color(255,0,108,196);
            LinearGradientBrush grad(PointF(fx,fy),PointF(fx,fy+fh),topC,botC);
            g.FillPath(&grad,&p);
            Pen hl(Color(55,255,255,255),1.0f);
            g.DrawLine(&hl,fx+5,fy+1.5f,fx+fw-5,fy+1.5f);
            Font f3(&ff,10,FontStyleBold,UnitPoint);
            SolidBrush t(Color(255,255,255,255));
            g.DrawString(LS(STR_GP_CLOSE),-1,&f3,RectF(fx,fy,fw,fh),&sfC,&t);
        }
    };

    // ── WndProc ──────────────────────────────────────────────────
    struct ProcHelper {
        static LRESULT CALLBACK Proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            if (!sp) return DefWindowProc(hWnd,msg,wParam,lParam);
            constexpr int DW=820,DH=580,HDR_H=48,FOOT_H=52,LEFT_W=220,ITEM_H=36;

            auto Repaint = [&]{ InvalidateRect(hWnd,nullptr,FALSE); };

            switch (msg)
            {
            case WM_PAINT: {
                PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps);
                RECT rc; GetClientRect(hWnd,&rc);
                HDC mem=CreateCompatibleDC(hdc);
                HBITMAP bmp=CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
                HBITMAP old=(HBITMAP)SelectObject(mem,bmp);
                // Render 호출 - 전달된 람다가 없으므로 직접 GDI+ 호출
                // (렌더링은 별도 static 함수로 분리 필요 → 간단히 WM_USER로 위임)
                SendMessage(hWnd, WM_USER+1, (WPARAM)mem, 0);
                BitBlt(hdc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
                SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem);
                EndPaint(hWnd,&ps); } break;

            case WM_MOUSEMOVE: {
                int x=GET_X_LPARAM(lParam), y=GET_Y_LPARAM(lParam);
                int ng=-1,ni=-1,nb=-1;
                POINT pt={x,y};
                // 그룹 목록
                for (auto& a:sp->grpAreas) if(PtInRect(&a.rc,pt)){ng=a.idx;break;}
                // 항목 목록
                for (auto& a:sp->itmAreas) if(PtInRect(&a.rc,pt)){ni=a.idx;break;}
                // 버튼 (푸터)
                auto inBtn=[&](int bx,int bw)->bool{
                    RECT r={bx,DH-FOOT_H+11,bx+bw,DH-FOOT_H+11+30};
                    return PtInRect(&r,pt)!=FALSE;
                };
                if(inBtn(8,88))         nb=0;
                else if(inBtn(102,88))  nb=1;
                else if(inBtn(LEFT_W+12,100)) nb=2;
                else if(inBtn(LEFT_W+118,100))nb=3;
                else if(inBtn(DW-108,96))     nb=4;
                // 헤더 × 히트테스트
                {int bx=DW-40,by2=(HDR_H-24)/2;RECT r={bx,by2,bx+24,by2+24};if(PtInRect(&r,pt))nb=5;}
                bool changed=(ng!=sp->hoverGrp||ni!=sp->hoverItm||nb!=sp->hoverBtn);
                sp->hoverGrp=ng;sp->hoverItm=ni;sp->hoverBtn=nb;
                if(changed)Repaint();
                TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hWnd,0};TrackMouseEvent(&tme);
            } break;

            case WM_MOUSELEAVE:
                sp->hoverGrp=sp->hoverItm=sp->hoverBtn=-1; Repaint(); break;

            case WM_LBUTTONUP: {
                int x=GET_X_LPARAM(lParam),y=GET_Y_LPARAM(lParam);
                POINT pt={x,y};

                // 닫기 (헤더)
                {int bx=DW-40,by=(HDR_H-24)/2;RECT r={bx,by,bx+24,by+24};
                 if(PtInRect(&r,pt)){sp->closed=true;CloseModal(hWnd);return 0;}}

                // 그룹 선택
                for(auto& a:sp->grpAreas) if(PtInRect(&a.rc,pt)){
                    sp->selGroup=a.idx; sp->selItem=-1; sp->scrollItm=0; Repaint(); return 0;
                }
                // 항목 선택
                for(auto& a:sp->itmAreas) if(PtInRect(&a.rc,pt)){
                    sp->selItem=a.idx; Repaint(); return 0;
                }

                auto inBtn2=[&](int bx,int bw)->bool{
                    RECT r={bx,DH-FOOT_H+11,bx+bw,DH-FOOT_H+11+30};
                    return PtInRect(&r,pt)!=FALSE;
                };

                // 그룹 추가
                if(inBtn2(8,88)) {
                    // 이름 입력 팝업
                    std::wstring name = ShowInputPopup(hWnd,LS(STR_GP_DLG_ADD_GROUP),LS(STR_GP_DLG_GROUP_NAME));
                    if(!name.empty()) {
                        bool dup=false; for(auto&gd:g_groups) if(gd.name==name){dup=true;break;}
                        if(!dup){GroupDef gd;gd.name=name;g_groups.push_back(gd);sp->selGroup=(int)g_groups.size()-1;sp->selItem=-1;}
                        else MessageBoxW(hWnd,LS(STR_GP_DLG_DUP),LS(STR_GP_DLG_DUP_TITLE),MB_OK|MB_ICONWARNING);
                    }
                    Repaint(); return 0;
                }
                // 그룹 삭제
                if(inBtn2(102,88) && sp->selGroup>=0 && sp->selGroup<(int)g_groups.size()) {
                    std::wstring q=L"'"+g_groups[sp->selGroup].name+L"' ";
                    q+=LS(STR_GP_DLG_DEL_GROUP);
                    if(MessageBoxW(hWnd,q.c_str(),LS(STR_GP_DLG_DEL_GROUP),MB_YESNO|MB_ICONWARNING)==IDYES){
                        g_groups.erase(g_groups.begin()+sp->selGroup);
                        if(sp->selGroup>=(int)g_groups.size()) sp->selGroup=(int)g_groups.size()-1;
                        sp->selItem=-1;
                    }
                    Repaint(); return 0;
                }
                // 항목 추가
                if(inBtn2(LEFT_W+12,100) && sp->selGroup>=0 && sp->selGroup<(int)g_groups.size()) {
                    std::wstring name=ShowInputPopup(hWnd,LS(STR_GP_DLG_ADD_ITEM),LS(STR_GP_DLG_ITEM_NAME));
                    if(!name.empty()){GroupItem it;it.name=name;g_groups[sp->selGroup].items.push_back(it);sp->selItem=(int)g_groups[sp->selGroup].items.size()-1;}
                    Repaint(); return 0;
                }
                // 항목 삭제
                if(inBtn2(LEFT_W+118,100) && sp->selGroup>=0 && sp->selItem>=0
                   && sp->selItem<(int)g_groups[sp->selGroup].items.size()) {
                    auto&grp=g_groups[sp->selGroup];
                    std::wstring q=L"'"+grp.items[sp->selItem].name+L"' ";
                    q+=LS(STR_GP_DLG_DEL_ITEM);
                    if(MessageBoxW(hWnd,q.c_str(),LS(STR_GP_DLG_DEL_ITEM),MB_YESNO|MB_ICONWARNING)==IDYES){
                        grp.items.erase(grp.items.begin()+sp->selItem);
                        if(sp->selItem>=(int)grp.items.size()) sp->selItem=(int)grp.items.size()-1;
                    }
                    Repaint(); return 0;
                }
                // 닫기 버튼 (푸터)
                if(inBtn2(DW-108,96)){sp->closed=true;CloseModal(hWnd);return 0;}
            } break;

            case WM_LBUTTONDBLCLK: {
                // 항목 더블클릭 → PDF 경로 등록
                int x=GET_X_LPARAM(lParam),y=GET_Y_LPARAM(lParam);
                POINT pt={x,y};
                for(auto& a:sp->itmAreas) if(PtInRect(&a.rc,pt)){
                    sp->selItem=a.idx;
                    if(sp->selGroup>=0 && sp->selGroup<(int)g_groups.size()){
                        auto& path0 = g_groups[sp->selGroup].items[sp->selItem].pdfPath;
                        std::wstring picked = PickPdfFile(hWnd, path0);
                        if (!picked.empty()) AssignPdf(hWnd, sp->selGroup, sp->selItem, picked);
                    }
                    Repaint(); return 0;
                }
            } break;

            case WM_MOUSEWHEEL: {
                int delta=GET_WHEEL_DELTA_WPARAM(wParam);
                int x=GET_X_LPARAM(lParam),y=GET_Y_LPARAM(lParam);
                POINT spt={x,y}; ScreenToClient(hWnd,&spt);
                if(spt.x < LEFT_W) sp->scrollGrp=max(0,sp->scrollGrp-delta/3);
                else sp->scrollItm=max(0,sp->scrollItm-delta/3);
                Repaint();
            } break;

            case WM_NCHITTEST: {
                LRESULT hit=DefWindowProc(hWnd,msg,wParam,lParam);
                if(hit==HTCLIENT){
                    POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                    ScreenToClient(hWnd,&pt);
                    if(pt.y < HDR_H) {
                        // × 버튼 영역은 클라이언트로 처리
                        int bx=DW-40, by2=(HDR_H-24)/2;
                        RECT xBtn={bx, by2, bx+24, by2+24};
                        if(PtInRect(&xBtn, pt)) return HTCLIENT;
                        return HTCAPTION;
                    }
                }
                return hit; }

            case WM_DESTROY:
                sp->closed=true; break;

            default: return DefWindowProc(hWnd,msg,wParam,lParam);
            }
            return 0;
        }
    };

    // WM_USER+1: 렌더링 (Render 람다를 호출)
    // 람다를 WndProc에서 직접 호출할 수 없으므로 thread_local 포인터로 전달
    using RenderFn = std::function<void(HDC)>;
    static RenderFn* sRender = nullptr;
    RenderFn renderFn = Render;
    sRender = &renderFn;

    // WndProc 래퍼
    static WNDPROC sProcHelper = [](HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (msg == WM_USER+1 && sRender) {
            (*sRender)((HDC)wParam);
            return 0;
        }
        return ProcHelper::Proc(hWnd, msg, wParam, lParam);
    };

    if (!sClassReg) {
        WNDCLASSEXW wc={};
        wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW|CS_DBLCLKS;
        wc.lpfnWndProc=sProcHelper; wc.hInstance=hInst;
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        wc.hbrBackground=nullptr; wc.lpszClassName=MGR_CLASS;
        RegisterClassExW(&wc);
        sClassReg=true;
    }

    RECT pr2; GetWindowRect(parent,&pr2);
    int wx=pr2.left+(pr2.right-pr2.left-DW)/2;
    int wy=pr2.top+(pr2.bottom-pr2.top-DH)/2;

    HWND hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST,
        MGR_CLASS,nullptr,WS_POPUP,wx,wy,DW,DH,parent,nullptr,hInst,nullptr);
    if (!hwnd) return;
    st.hwnd=hwnd;

    DWM_WINDOW_CORNER_PREFERENCE corner=DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd,DWMWA_WINDOW_CORNER_PREFERENCE,&corner,sizeof(corner));
    MARGINS margins={1,1,1,1};
    DwmExtendFrameIntoClientArea(hwnd,&margins);

    SetLayeredWindowAttributes(hwnd,0,0,LWA_ALPHA);
    ShowWindow(hwnd,SW_SHOW);
    AnimateWindow(hwnd,200,AW_BLEND);
    SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA);
    SetForegroundWindow(hwnd);

    // 메인 창을 비활성화: 이 창에서 띄우는 입력 팝업·MessageBox·파일 대화상자 중에도
    // 메인 창 입력(메뉴로 관리 창 재진입 등)이 막힌다.
    EnableWindow(parent, FALSE);
    RunModalLoop(st.closed);
    if (IsWindow(hwnd)) CloseModal(hwnd);  // WM_QUIT으로 빠져나온 경우

    // 선택된 그룹을 패널에 반영
    g_selGroup = st.selGroup;
    if (g_selGroup >= (int)g_groups.size()) g_selGroup = (int)g_groups.size() - 1;
    g_scrollItem = 0;
    g_hoverItem = g_hoverBtn = g_hoverDrop = -1;
    sRender = nullptr;
    sp = nullptr;
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    SetFocus(parent);
}

// ── 그리기 ──────────────────────────────────────────────────────
void GroupPanel::Draw(HDC hdc, const RECT& clientRect)
{
    if (!g_visible && !g_animClosing) return;
    using namespace Gdiplus;

    RECT pr = GetPanelRect(clientRect);
    OffsetRect(&pr, g_animOffset, 0);   // 애니메이션 중이면 오른쪽으로 밀린 위치에 그림
    const int W  = pr.right - pr.left;
    const int H  = pr.bottom - pr.top;
    const int ox = pr.left;
    const int oy = pr.top;

    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // ── 팔레트: 왼쪽 사이드바와 같은 PanelPal() (밝은 테마 / 야간 모드에서는 어두운 테마) ──
    const PanelPalette& P = PanelPal();
    auto C = [](COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); };
    const Color cBg       = C(P.bg);
    const Color cLine     = C(P.line);
    const Color cHover    = C(P.hover);
    const Color cActive   = C(P.active);
    const Color cAccent   = C(P.accent);
    const Color cAccentLt = C(P.accentIcon);
    const Color cText     = C(P.text);
    const Color cTextHi   = C(P.textHi);
    const Color cDim      = C(P.textDim);
    const Color cMuted    = C(P.textMuted);
    const Color cField    = C(P.field);
    const Color cFieldLn  = C(P.fieldLine);
    const Color cPill     = C(P.pillOnField);
    const Color cBtnHov   = C(P.btnHover);
    const Color cWarn     = C(P.warn);
    const Color cFaint    = C(P.iconFaint);

    FontFamily ff(L"Segoe UI");
    FontFamily mdl(L"Segoe MDL2 Assets");
    FontFamily fluent(L"Segoe Fluent Icons");
    const FontFamily* icf = mdl.IsAvailable() ? &mdl : &fluent;

    StringFormat sfL;  sfL.SetLineAlignment(StringAlignmentCenter);
    sfL.SetFormatFlags(StringFormatFlagsNoWrap); sfL.SetTrimming(StringTrimmingEllipsisCharacter);
    StringFormat sfR;  sfR.SetAlignment(StringAlignmentFar); sfR.SetLineAlignment(StringAlignmentCenter);
    sfR.SetFormatFlags(StringFormatFlagsNoWrap);
    StringFormat sfC;  sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentCenter);
    sfC.SetFormatFlags(StringFormatFlagsNoWrap);

    auto rf = [](const RECT& r) {
        return RectF((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top));
    };
    auto fillRR = [&](const RECT& r, float rad, const Color& c, const Color* border = nullptr) {
        SolidBrush b(c);
        if (border) {
            Pen p(*border, 1.0f);
            DrawRR(g, r.left + 0.5f, r.top + 0.5f, (float)(r.right - r.left) - 1.0f,
                   (float)(r.bottom - r.top) - 1.0f, rad, &b, &p);
        } else {
            DrawRR(g, (float)r.left, (float)r.top, (float)(r.right - r.left),
                   (float)(r.bottom - r.top), rad, &b);
        }
    };
    auto glyph = [&](const wchar_t* ch, const RECT& r, float px, const Color& c) {
        Font f(icf, px, FontStyleRegular, UnitPixel);
        SolidBrush b(c);
        g.DrawString(ch, -1, &f, rf(r), &sfC, &b);
    };
    auto text = [&](const wchar_t* s, const RECT& r, float px, const Color& c,
                    const StringFormat& sf, int style = FontStyleRegular) {
        Font f(&ff, px, style, UnitPixel);
        SolidBrush b(c);
        g.DrawString(s, -1, &f, rf(r), &sf, &b);
    };
    auto textW = [&](const wchar_t* s, float px, int style = FontStyleRegular) -> int {
        Font f(&ff, px, style, UnitPixel);
        RectF box;
        g.MeasureString(s, -1, &f, PointF(0, 0), &box);
        return (int)(box.Width + 0.5f);
    };
    // 작은 숫자 배지 (그룹 수·항목 수)
    auto pill = [&](int n, int rightX, int cy, const Color& bg) -> int {
        wchar_t s[16]; swprintf_s(s, L"%d", n);
        int w = textW(s, 11.0f) + 10, h = 18;
        RECT r = { rightX - w, cy - h / 2, rightX, cy + h / 2 };
        fillRR(r, h / 2.0f, bg);
        text(s, r, 11.0f, cDim, sfC);
        return r.left;
    };

    // 움직이는 동안만 왼쪽에 옅은 그림자 (페이지 위로 떠 있는 느낌)
    if (g_animating && g_animOffset < W) {
        LinearGradientBrush sh(PointF((float)(ox - GP_SHADOW_W), 0.0f), PointF((float)ox, 0.0f),
                               Color(0,0,0,0), Color(P.slideShadow,0,0,0));
        sh.SetWrapMode(WrapModeTileFlipX);  // 그라데이션 끝 1px 이음새 방지
        g.FillRectangle(&sh, (float)(ox - GP_SHADOW_W), (float)oy, (float)GP_SHADOW_W, (float)H);
    }

    // 배경 + 왼쪽 경계선
    { SolidBrush b(cBg); g.FillRectangle(&b, ox, oy, W, H); }
    { Pen p(cLine, 1.0f); g.DrawLine(&p, (float)ox + 0.5f, (float)oy, (float)ox + 0.5f, (float)(oy + H)); }

    const bool hasSel = g_selGroup >= 0 && g_selGroup < (int)g_groups.size();

    // ── 헤더: [아이콘 배지] 그룹 (n) ............ [×] ──
    {
        const int bs = 22;
        RECT badge = { ox + 12, oy + (GROUP_HEADER_H - bs) / 2, ox + 12 + bs, oy + (GROUP_HEADER_H - bs) / 2 + bs };
        fillRR(badge, 6.0f, C(P.accent, P.badgeAlpha));
        glyph(L"\xE8B7", badge, 13.0f, cAccentLt);   // Folder

        RECT xr = CloseBtnRect(pr);
        int titleX = badge.right + 9, titleR = xr.left - 6;
        int tw = textW(LS(STR_GP_PANEL_TITLE), 14.0f, FontStyleBold);
        RECT tr = { titleX, oy, titleR, oy + GROUP_HEADER_H };
        text(LS(STR_GP_PANEL_TITLE), tr, 14.0f, cText, sfL, FontStyleBold);
        if (!g_groups.empty()) {
            int px = titleX + tw + 6;
            wchar_t s[16]; swprintf_s(s, L"%d", (int)g_groups.size());
            int pw = textW(s, 11.0f) + 10;
            if (px + pw <= titleR) pill((int)g_groups.size(), px + pw, oy + GROUP_HEADER_H / 2, C(P.pill));
        }

        if (g_hoverClose) fillRR(xr, 6.0f, cBtnHov);
        glyph(L"\xE711", xr, 10.0f, g_hoverClose ? C(P.closeHot) : cMuted);  // Cancel

        Pen sep(cLine, 1.0f);
        g.DrawLine(&sep, (float)ox, (float)(oy + GROUP_HEADER_H) - 0.5f,
                         (float)(ox + W), (float)(oy + GROUP_HEADER_H) - 0.5f);
    }

    // ── 그룹 선택 상자: [그룹 이름 ........ (n) ⌄] ──
    {
        RECT cb = ComboRect(pr);
        bool hot = g_hoverDropBtn || g_dropOpen;
        Color border = g_dropOpen ? cAccent : (g_hoverDropBtn ? C(P.fieldLineHot) : cFieldLn);
        fillRR(cb, 6.0f, hot ? cHover : cField, &border);

        RECT chev = { cb.right - 26, cb.top, cb.right - 6, cb.bottom };
        glyph(g_dropOpen ? L"\xE70E" : L"\xE70D", chev, 10.0f, cDim);   // ChevronUp / ChevronDown

        int nameR = chev.left - 4;
        if (hasSel) {
            int cnt = (int)g_groups[g_selGroup].items.size();
            nameR = pill(cnt, chev.left - 2, (cb.top + cb.bottom) / 2, cPill) - 6;
        }
        RECT nr = { cb.left + 12, cb.top, nameR, cb.bottom };
        if (hasSel)
            text(g_groups[g_selGroup].name.c_str(), nr, 13.0f, cText, sfL, FontStyleBold);
        else
            text(LS(STR_GP_SEL_DROP), nr, 13.0f, cMuted, sfL);
    }

    // ── 항목 목록 ──
    RECT L = ListRect(pr);
    {
        Region oldClip; g.GetClip(&oldClip);
        g.SetClip(rf(L));

        // 빈 상태: 큰 아이콘 + 안내 문구 (가운데)
        auto emptyState = [&](const wchar_t* icon, const wchar_t* msg) {
            int cy = (L.top + L.bottom) / 2;
            RECT ir = { L.left, cy - 44, L.right, cy - 8 };
            glyph(icon, ir, 28.0f, cFaint);
            RECT mr = { L.left + 20, cy - 2, L.right - 20, cy + 44 };
            StringFormat sfW; sfW.SetAlignment(StringAlignmentCenter);
            Font f(&ff, 12.0f, FontStyleRegular, UnitPixel);
            SolidBrush b(cMuted);
            g.DrawString(msg, -1, &f, rf(mr), &sfW, &b);
        };

        if (!hasSel) {
            emptyState(L"\xE8B7", g_groups.empty() ? LS(STR_GP_ADD_FIRST) : LS(STR_GP_SEL_DROP));
        } else if (g_groups[g_selGroup].items.empty()) {
            emptyState(L"\xE8A5", LS(STR_GP_NO_ITEMS));
        } else {
            ClampItemScroll(pr);
            auto& grp = g_groups[g_selGroup];
            for (int i = 0; i < (int)grp.items.size(); ++i) {
                int iy = ItemTop(pr, i);
                if (iy + GROUP_ITEM_H < L.top) continue;
                if (iy > L.bottom) break;

                const auto& item = grp.items[i];
                const bool hov    = (g_hoverItem == i) && !g_dropOpen;
                const bool hasPdf = !item.pdfPath.empty();
                const bool lost   = hasPdf && item.missing;
                const bool hasMemo = !item.memo.empty();

                if (hov) {
                    RECT rr = { ox + 6, iy + 1, ox + W - 6, iy + GROUP_ITEM_H - 1 };
                    fillRR(rr, 6.0f, cHover);
                }

                // 앞 아이콘: 연결됨=강조색 문서, 미연결=흐린 문서, 파일 없음=주황 경고
                RECT ic = { ox + 12, iy, ox + 32, iy + GROUP_ITEM_H };
                if (lost)        glyph(L"\xE7BA", ic, 14.0f, cWarn);                    // Warning
                else if (hasPdf) glyph(L"\xE8A5", ic, 14.0f, cAccentLt);                // Document
                else             glyph(L"\xE8A5", ic, 14.0f, cFaint);

                // 오른쪽 아이콘 버튼: 호버 중이거나 상태를 알려야 할 때만 보임
                const bool showLink = hov || !hasPdf || lost;
                const bool showMemo = hov || hasMemo;
                RECT rLink = ItemBtnRect(pr, iy, 0);
                RECT rMemo = ItemBtnRect(pr, iy, 1);
                int nameR = ox + W - 12;
                if (showMemo) nameR = rMemo.left - 4;
                if (showLink) nameR = rLink.left - 4;

                RECT nr = { ox + 38, iy, nameR, iy + GROUP_ITEM_H };
                text(item.name.c_str(), nr, 13.0f, lost ? cMuted : (hov ? cTextHi : cText), sfL);

                if (showLink) {
                    bool bh = hov && g_hoverBtn == 0;
                    if (bh) fillRR(rLink, 6.0f, cBtnHov);
                    Color c = lost ? cWarn : (bh ? cTextHi : (hasPdf ? cMuted : cDim));
                    glyph(L"\xE71B", rLink, 14.0f, c);                                    // Link
                }
                if (showMemo) {
                    bool bh = hov && g_hoverBtn == 1;
                    if (bh) fillRR(rMemo, 6.0f, cBtnHov);
                    Color c = hasMemo ? (bh ? C(P.accentTextHot) : cAccentLt) : (bh ? cTextHi : cMuted);
                    glyph(L"\xE70B", rMemo, 14.0f, c);                                    // QuickNote
                }
            }

            // 넘치면 오른쪽 끝에 얇은 스크롤 표시
            int maxS = MaxItemScroll(pr);
            if (maxS > 0) {
                float trackH = (float)(L.bottom - L.top) - 8.0f;
                float total  = trackH + maxS;
                float thumbH = trackH * trackH / total;
                if (thumbH < 16.0f) thumbH = 16.0f;
                float thumbY = L.top + 4.0f + (trackH - thumbH) * g_scrollItem / (float)maxS;
                SolidBrush tb(C(P.scroll));
                DrawRR(g, (float)(ox + W - 5), thumbY, 3.0f, thumbH, 1.5f, &tb);
            }
        }
        g.SetClip(&oldClip);
    }

    // ── 하단: 구분선 + [+ 항목 추가] 고스트 버튼 (사이드바 북마크 버튼과 같은 모양) ──
    {
        Pen sep(cLine, 1.0f);
        float sy = (float)(pr.bottom - GROUP_FOOTER_H) + 0.5f;
        g.DrawLine(&sep, (float)ox, sy, (float)(ox + W), sy);

        RECT fb = FooterBtnRect(pr);
        Color border = g_hoverFooter ? cAccent : cFieldLn;
        fillRR(fb, 6.0f, g_hoverFooter ? cActive : cField, &border);

        // 문자열 앞의 "+ "는 아이콘으로 대신 그린다
        const wchar_t* lbl = FooterAddsItem() ? LS(STR_GP_ADD_ITEM) : LS(STR_GP_ADD_GROUP);
        if (lbl[0] == L'+') { ++lbl; while (*lbl == L' ') ++lbl; }
        int lw = textW(lbl, 13.0f);
        const int plusW = 12, gap = 7;
        int bx = fb.left + max(6, ((fb.right - fb.left) - (plusW + gap + lw)) / 2);
        RECT plus = { bx, fb.top, bx + plusW, fb.bottom };
        glyph(L"\xE710", plus, 11.0f, g_hoverFooter ? cAccentLt : cDim);           // Add
        RECT lr = { bx + plusW + gap, fb.top, fb.right - 6, fb.bottom };
        text(lbl, lr, 13.0f, g_hoverFooter ? C(P.accentTextHot) : cDim, sfL);
    }

    // ── 드롭다운 카드 (열렸을 때, 목록 위에 띄움) ──
    if (g_dropOpen && !g_groups.empty()) {
        ClampDropScroll(pr);
        RECT pa = GetPanelRect(clientRect);   // 히트 테스트는 제자리 기준이므로 그림도 같은 기준 + 오프셋
        RECT card = DropCardRect(pa);  OffsetRect(&card, g_animOffset, 0);
        int n = DropVisibleRows(pa), total = (int)g_groups.size();

        // 그림자 2겹 + 카드
        for (int k = 2; k >= 1; --k) {
            RECT s = { card.left - k, card.top + k, card.right + k, card.bottom + 2 * k };
            fillRR(s, 9.0f, Color((BYTE)(P.cardShadow * (3 - k)), 0, 0, 0));
        }
        Color cardLn = cFieldLn;
        fillRR(card, 8.0f, C(P.card), &cardLn);

        for (int row = 0; row < n; ++row) {
            int i = g_scrollTab + row;
            if (i >= total) break;
            RECT rr = HitDropdownItem(pa, row);  OffsetRect(&rr, g_animOffset, 0);
            bool hov = (i == g_hoverDrop);
            bool sel = (i == g_selGroup);
            if (sel) {
                fillRR(rr, 6.0f, cActive);
                SolidBrush bar(cAccent);
                DrawRR(g, (float)rr.left, (float)rr.top + 9, 3.0f, (float)GROUP_DROP_ROW_H - 18, 1.5f, &bar);
            } else if (hov) {
                fillRR(rr, 6.0f, cHover);
            }
            wchar_t cs[16]; swprintf_s(cs, L"%d", (int)g_groups[i].items.size());
            RECT cr2 = { rr.right - 40, rr.top, rr.right - 10, rr.bottom };
            text(cs, cr2, 11.0f, cMuted, sfR);
            RECT nr = { rr.left + 12, rr.top, rr.right - 44, rr.bottom };
            text(g_groups[i].name.c_str(), nr, 13.0f,
                 sel ? C(P.textActive) : (hov ? cTextHi : cText), sfL,
                 sel ? FontStyleBold : FontStyleRegular);
        }

        // 넘치는 경우 오른쪽에 얇은 스크롤 표시
        if (total > n) {
            float trackH = (float)(n * GROUP_DROP_ROW_H);
            float thumbH = trackH * n / total;
            if (thumbH < 12.0f) thumbH = 12.0f;
            float thumbY = card.top + 4.0f + (trackH - thumbH) * g_scrollTab / (float)(total - n);
            SolidBrush thumb(C(P.scroll));
            DrawRR(g, (float)(card.right - 6), thumbY, 3.0f, thumbH, 1.5f, &thumb);
        }
    }
}

// ── 툴팁 ────────────────────────────────────────────────────────
// 툴바·탭과 같은 "PlainPDFTip" 창(밝은 둥근 툴팁, 야간 모드에서는 어두운 툴팁)을 쓴다. 표시할 글자는 창 텍스트로 넘긴다.
// 툴바처럼 잠시 머물러야(500ms) 뜨고, 이미 떠 있는 상태에서 옆 버튼으로 옮기면 바로 바뀐다.
namespace
{
    constexpr UINT_PTR GP_TIP_TIMER = 0x4751;
    constexpr UINT     GP_TIP_DELAY = 500;   // 툴바 툴팁과 동일

    HWND         g_hTip      = nullptr;
    HWND         g_tipOwner  = nullptr;
    std::wstring g_tipText;                  // 대기 중이거나 표시 중인 텍스트
    RECT         g_tipAnchor = {};           // 툴팁을 붙일 영역 (클라이언트 좌표)
    bool         g_tipShown  = false;

    void TipShowNow()
    {
        if (g_tipText.empty() || !g_tipOwner) return;
        if (!g_hTip) {
            g_hTip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                L"PlainPDFTip", L"", WS_POPUP, 0, 0, 10, 10, g_tipOwner, nullptr, hInst, nullptr);
            if (!g_hTip) return;
        }
        SetWindowTextW(g_hTip, g_tipText.c_str());

        // 크기: 툴바 툴팁과 같은 글꼴·여백
        int tipW, tipH;
        {
            HDC hdc = GetDC(g_hTip);
            Gdiplus::Graphics gfx(hdc);
            Gdiplus::FontFamily ff(L"Segoe UI");
            Gdiplus::Font font(&ff, 9.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
            Gdiplus::RectF layout(0, 0, 420, 400), bound;
            gfx.MeasureString(g_tipText.c_str(), -1, &font, layout, &bound);
            ReleaseDC(g_hTip, hdc);
            tipW = (int)bound.Width + 16 * 2;
            tipH = (int)bound.Height + 8 * 2;
        }

        // 위치: 대상 바로 아래 가운데. 화면을 넘으면 안쪽으로, 아래가 모자라면 위로
        POINT a = { (g_tipAnchor.left + g_tipAnchor.right) / 2, g_tipAnchor.bottom };
        POINT t = { 0, g_tipAnchor.top };
        ClientToScreen(g_tipOwner, &a);
        ClientToScreen(g_tipOwner, &t);
        int x = a.x - tipW / 2, y = a.y + 6;
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromPoint(a, MONITOR_DEFAULTTONEAREST), &mi)) {
            const RECT& w = mi.rcWork;
            if (x + tipW > w.right - 4) x = w.right - 4 - tipW;
            if (x < w.left + 4)         x = w.left + 4;
            if (y + tipH > w.bottom - 4) y = t.y - 6 - tipH;
        }
        SetWindowPos(g_hTip, HWND_TOPMOST, x, y, tipW, tipH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowRgn(g_hTip, CreateRoundRectRgn(0, 0, tipW + 1, tipH + 1, 12, 12), FALSE);
        InvalidateRect(g_hTip, nullptr, TRUE);
        g_tipShown = true;
    }

    void TipHide()
    {
        if (g_tipOwner) KillTimer(g_tipOwner, GP_TIP_TIMER);
        if (g_hTip && g_tipShown) ShowWindow(g_hTip, SW_HIDE);
        g_tipShown = false;
        g_tipText.clear();
    }

    void CALLBACK TipTimerProc(HWND hWnd, UINT, UINT_PTR, DWORD)
    {
        KillTimer(hWnd, GP_TIP_TIMER);
        if (g_visible && !g_animating) TipShowNow();
    }

    // 호버 대상이 바뀔 때 호출. text가 비어 있으면 숨김.
    void TipUpdate(HWND owner, const std::wstring& text, const RECT& anchor)
    {
        if (text == g_tipText && EqualRect(&anchor, &g_tipAnchor)) return;  // 같은 대상
        bool wasShown = g_tipShown;
        TipHide();
        g_tipOwner = owner;
        if (text.empty()) return;
        g_tipText   = text;
        g_tipAnchor = anchor;
        if (wasShown) TipShowNow();   // 툴팁이 떠 있던 중이면 옆으로 옮길 때 기다리지 않음
        else SetTimer(owner, GP_TIP_TIMER, GP_TIP_DELAY, TipTimerProc);
    }

    // 마우스 위치에 맞는 툴팁 글자와 붙일 영역
    std::wstring TipFor(int x, int y, const RECT& pr, RECT& anchor)
    {
        if (HitClose(x, y, pr)) { anchor = CloseBtnRect(pr); return LS(STR_GP_CLOSE); }
        if (g_dropOpen) return L"";

        int sub = -1;
        int ii = HitItem(x, y, pr, sub);
        if (ii < 0) return L"";
        const GroupItem& it = g_groups[g_selGroup].items[ii];
        int iy = ItemTop(pr, ii);

        if (sub == 0) {          // PDF 연결 버튼
            anchor = ItemBtnRect(pr, iy, 0);
            if (it.pdfPath.empty()) return LS(STR_GP_TIP_LINK);
            return it.missing ? LS(STR_GP_TIP_RELINK) : LS(STR_GP_TIP_CHANGE);
        }
        if (sub == 1) {          // 메모 버튼: 메모가 있으면 첫 줄 미리보기
            anchor = ItemBtnRect(pr, iy, 1);
            if (it.memo.empty()) return LS(STR_GP_TIP_MEMO_ADD);
            std::wstring first = it.memo.substr(0, it.memo.find(L'\n'));
            if (first.size() > 60) first = first.substr(0, 60) + L"…";
            std::wstring s = LS(STR_GP_TIP_MEMO_EDIT);
            if (!TrimWs(first).empty()) s += L"\n" + first;
            return s;
        }
        // 항목 줄: 연결된 PDF 경로 (탭 툴팁처럼 전체 경로)
        if (it.pdfPath.empty()) return L"";
        anchor = { pr.left + 6, iy, pr.right - 6, iy + GROUP_ITEM_H };
        if (it.missing) return std::wstring(L"⚠ ") + LS(STR_GP_FILE_MISSING) + L"\n" + it.pdfPath;
        return it.pdfPath;
    }
}

// ── 마우스 이벤트 ────────────────────────────────────────────────

void GroupPanel::OnMouseMove(HWND hWnd, int x, int y, const RECT& clientRect)
{
    if (!g_visible) return;
    RECT pr = GetPanelRect(clientRect);
    if (x < pr.left) { OnMouseLeave(hWnd); return; }

    bool  nc  = HitClose(x,y,pr);
    int   nt  = HitTab(x,y,pr);       // 드롭다운 목록 내 인덱스 (열렸을 때)
    int   sub = -1;
    // 드롭다운이 열려 있으면 다음 클릭은 목록 닫기이므로 아래 항목은 호버하지 않는다
    int   ni  = g_dropOpen ? -1 : HitItem(x,y,pr,sub);
    if (ni < 0) sub = -1;

    // 드롭다운 버튼 호버 (드롭다운 카드가 덮고 있는 곳은 제외)
    RECT ddBtn = HitDropdownBtn(pr);
    bool hDropBtn = PtInRect(&ddBtn,{x,y}) != FALSE;
    bool hFooter  = !g_dropOpen && HitFooter(x,y,pr);

    bool changed = (nc!=g_hoverClose)||(nt!=g_hoverDrop)||(ni!=g_hoverItem)
                  ||(sub!=g_hoverBtn)||(hDropBtn!=g_hoverDropBtn)||(hFooter!=g_hoverFooter);
    g_hoverClose  = nc;
    g_hoverDrop   = nt;
    g_hoverItem   = ni;
    g_hoverBtn    = sub;
    g_hoverDropBtn= hDropBtn;
    g_hoverFooter = hFooter;

    if (changed) {
        RECT inv={pr.left,pr.top,pr.right,pr.bottom};
        InvalidateRect(hWnd,&inv,FALSE);
    }

    OnSetCursor(x, y, clientRect);

    // 아이콘 버튼·항목 툴팁
    RECT anchor = {};
    std::wstring tip = TipFor(x, y, pr, anchor);
    TipUpdate(hWnd, tip, anchor);

    TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hWnd,0};
    TrackMouseEvent(&tme);
}

bool GroupPanel::OnSetCursor(int x, int y, const RECT& clientRect)
{
    if (!HitTest(x, y, clientRect)) return false;
    RECT pr = GetPanelRect(clientRect);

    // 실제로 클릭하면 동작하는 곳만 손 모양, 나머지 패널은 항상 화살표
    // (손 도구 커서가 패널 위에 남아 보이지 않도록)
    bool clickable = HitClose(x,y,pr) || HitTab(x,y,pr) >= 0
                  || (!g_dropOpen && HitFooter(x,y,pr));
    if (!clickable) {
        RECT ddBtn = HitDropdownBtn(pr);
        clickable = PtInRect(&ddBtn,{x,y}) != FALSE;
    }
    if (!clickable && !g_dropOpen) {
        int sub = -1;
        int ni = HitItem(x,y,pr,sub);
        if (ni >= 0)
            clickable = sub >= 0   // [등록]/[메모] 버튼
                     || !g_groups[g_selGroup].items[ni].pdfPath.empty();  // PDF가 등록된 항목
    }
    SetCursor(LoadCursor(nullptr, clickable ? IDC_HAND : IDC_ARROW));
    return true;
}

void GroupPanel::OnMouseLeave(HWND hWnd)
{
    bool hadHover = g_hoverClose || g_hoverDrop>=0 || g_hoverDropBtn
                 || g_hoverItem>=0 || g_hoverBtn>=0 || g_hoverFooter;
    g_hoverClose=false; g_hoverTab=-1; g_hoverDrop=-1;
    g_hoverDropBtn=false; g_hoverItem=-1; g_hoverBtn=-1; g_hoverFooter=false;
    TipHide();

    // 호버 강조가 화면에 남지 않도록 패널을 다시 그린다
    if (hadHover && g_visible && hWnd) {
        RECT cr; GetClientRect(hWnd, &cr);
        RECT pr = GetPanelRect(cr);
        InvalidateRect(hWnd, &pr, FALSE);
    }
}

bool GroupPanel::HitTest(int x, int y, const RECT& clientRect)
{
    if (!g_visible) return false;
    RECT pr = GetPanelRect(clientRect);
    return PtInRect(&pr, {x, y}) != FALSE;
}

bool GroupPanel::OnLButtonDown(HWND hWnd, int x, int y, const RECT& clientRect)
{
    g_pressedInPanel = false;
    if (!g_visible) return false;

    if (!HitTest(x, y, clientRect)) {
        // 패널 바깥 클릭 → 열린 드롭다운만 닫고 클릭은 페이지 쪽으로 넘긴다
        if (g_dropOpen) {
            g_dropOpen = false;
            RECT pr = GetPanelRect(clientRect);
            InvalidateRect(hWnd, &pr, FALSE);
        }
        return false;
    }
    TipHide();   // 누르면 툴팁은 사라진다 (대화상자가 뜰 수 있으므로)
    g_pressedInPanel = true;
    return true;
}

bool GroupPanel::OnLButtonUp(HWND hWnd, int x, int y, const RECT& clientRect)
{
    // 패널에서 누른 클릭만 처리 (페이지에서 드래그하다 패널 위에서 뗀 경우는 페이지 쪽 몫)
    if (!g_pressedInPanel) return false;
    g_pressedInPanel = false;
    if (!g_visible) return true;
    RECT pr = GetPanelRect(clientRect);
    if (!PtInRect(&pr, {x, y})) return true;  // 패널에서 눌렀다가 밖에서 뗌 → 무시

    // 닫기: 메뉴 명령으로 보내 메뉴 체크·레이아웃 재계산까지 같은 경로로 처리
    if (HitClose(x,y,pr)) {
        PostMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_GROUP_PANEL, 0), 0);
        return true;
    }

    // 드롭다운 목록이 열려 있으면 목록 클릭 / 바깥 클릭으로 닫기
    if (g_dropOpen) {
        int ti = HitTab(x,y,pr);
        if (ti >= 0) {
            if (g_selGroup != ti) {
                CloseMemoEdit();
                g_scrollItem = 0;
                RefreshMissing(ti);
            }
            g_selGroup = ti;
        }
        g_dropOpen = false;
        g_hoverDrop = -1;
        InvalidateRect(hWnd,nullptr,FALSE);
        return true;
    }

    // 드롭다운 버튼 클릭 → 목록 열기
    {
        RECT ddBtn = HitDropdownBtn(pr);
        if (PtInRect(&ddBtn,{x,y})) {
            g_dropOpen = !g_groups.empty();
            if (g_dropOpen) {
                // 선택된 그룹이 보이도록 스크롤 위치 조정
                int rows = DropMaxRows(pr);
                if (g_selGroup >= 0) {
                    if (g_selGroup < g_scrollTab) g_scrollTab = g_selGroup;
                    else if (g_selGroup >= g_scrollTab + rows) g_scrollTab = g_selGroup - rows + 1;
                }
                ClampDropScroll(pr);
                g_hoverItem = g_hoverBtn = -1;
            }
            InvalidateRect(hWnd,nullptr,FALSE);
            return true;
        }
    }

    // 하단 [+ 항목 추가] / [+ 그룹 추가]
    if (HitFooter(x,y,pr)) {
        g_hoverFooter = false;
        AddFromFooter(hWnd, pr);
        InvalidateRect(hWnd,nullptr,FALSE);
        return true;
    }

    // 항목 클릭
    int sub=-1;
    int idx = HitItem(x,y,pr,sub);
    if (idx<0) return true;

    auto& grp = g_groups[g_selGroup];

    if (sub==0) {
        // 등록: 파일 다이얼로그 (기존 경로가 있으면 그 폴더·파일명에서 시작)
        std::wstring picked = PickPdfFile(hWnd, grp.items[idx].pdfPath);
        if (!picked.empty()) AssignPdf(hWnd, g_selGroup, idx, picked);
    } else if (sub==1) {
        // 메모
        if (g_memoEditItem==idx) CloseMemoEdit();
        else { CloseMemoEdit(); OpenMemoEdit(hWnd,idx,clientRect); }
    } else {
        // 항목 자체 클릭 → 등록된 PDF 열기
        CloseMemoEdit();
        auto& item = grp.items[idx];
        if (!item.pdfPath.empty()) {
            if (FileExists(item.pdfPath)) {
                item.missing = false;
                OpenOrSwitchToTab(hWnd, item.pdfPath, true);
            } else {
                // 삭제·이동된 파일: 열기 오류 대신 안내하고 다시 연결할 기회를 준다
                item.missing = true;
                InvalidateRect(hWnd,nullptr,FALSE);
                if (PromptRelink(hWnd, g_selGroup, idx))
                    OpenOrSwitchToTab(hWnd, g_groups[g_selGroup].items[idx].pdfPath, true);
            }
        }
    }
    InvalidateRect(hWnd,nullptr,FALSE);
    return true;
}

namespace
{
    // 그룹 삭제 (확인 후). 삭제했으면 true
    bool DeleteGroupWithConfirm(HWND hWnd, int gi)
    {
        if (gi < 0 || gi >= (int)g_groups.size()) return false;
        const GroupDef& gd = g_groups[gi];

        std::wstring msg(gd.name.size() + 256, L'\0');
        swprintf_s(msg.data(), msg.size(), LS(STR_GP_DEL_GROUP_FMT), gd.name.c_str());
        msg.resize(wcslen(msg.c_str()));
        if (!gd.items.empty()) {
            wchar_t extra[256];
            swprintf_s(extra, LS(STR_GP_DEL_GROUP_ITEMS_FMT), (int)gd.items.size());
            msg += L"\n";
            msg += extra;
        }
        if (!CleanPopup::Show(hWnd, LS(STR_GP_DLG_DEL_GROUP), msg, true)) return false;

        g_groups.erase(g_groups.begin() + gi);

        // 선택 그룹 보정: 앞쪽이 지워지면 한 칸 당기고, 선택 그룹이 지워지면 같은 자리(없으면 이전) 그룹
        int oldSel = g_selGroup;
        if (gi < g_selGroup) --g_selGroup;
        else if (gi == g_selGroup)
            g_selGroup = gi < (int)g_groups.size() ? gi : (int)g_groups.size() - 1;
        if (g_selGroup != oldSel || gi == oldSel) {
            CloseMemoEdit();
            g_scrollItem = 0;
            RefreshMissing(g_selGroup);
        }
        g_hoverItem = g_hoverBtn = g_hoverDrop = -1;
        GroupPanel::SaveGroups();
        return true;
    }

    // 현재 선택 그룹의 항목 삭제 (확인 후). 삭제했으면 true
    bool DeleteItemWithConfirm(HWND hWnd, int ii, const RECT& pr)
    {
        if (g_selGroup < 0 || g_selGroup >= (int)g_groups.size()) return false;
        auto& items = g_groups[g_selGroup].items;
        if (ii < 0 || ii >= (int)items.size()) return false;

        std::wstring msg(items[ii].name.size() + 256, L'\0');
        swprintf_s(msg.data(), msg.size(), LS(STR_GP_DEL_ITEM_FMT), items[ii].name.c_str());
        msg.resize(wcslen(msg.c_str()));
        if (!items[ii].memo.empty()) {   // 메모가 있으면 함께 사라진다는 것을 알림
            msg += L"\n";
            msg += LS(STR_GP_DEL_ITEM_MEMO);
        }
        if (!CleanPopup::Show(hWnd, LS(STR_GP_DLG_DEL_ITEM), msg, true)) return false;

        items.erase(items.begin() + ii);

        // 목록이 짧아져 스크롤이 끝을 넘으면 당겨 온다
        ClampItemScroll(pr);

        g_hoverItem = g_hoverBtn = -1;
        GroupPanel::SaveGroups();
        return true;
    }
}

bool GroupPanel::OnRButtonDown(HWND hWnd, int x, int y, const RECT& clientRect)
{
    if (!HitTest(x, y, clientRect)) return false;
    RECT pr = GetPanelRect(clientRect);
    TipHide();

    // 대상 그룹: 펼친 목록의 줄, 또는 드롭다운 버튼(= 현재 선택 그룹)
    int gi = -1;
    if (g_dropOpen) gi = HitTab(x, y, pr);
    if (gi < 0) {
        RECT ddBtn = HitDropdownBtn(pr);
        if (PtInRect(&ddBtn, {x, y})) gi = g_selGroup;
    }
    // 그룹이 아니면 항목 줄인지 확인 (드롭다운이 펼쳐져 있을 땐 항목이 가려져 있으므로 제외)
    int ii = -1;
    if ((gi < 0 || gi >= (int)g_groups.size()) && !g_dropOpen) {
        int sub = -1;
        ii = HitItem(x, y, pr, sub);
    }
    if ((gi < 0 || gi >= (int)g_groups.size()) && ii < 0) return true;  // 그 밖의 곳: 메뉴 없음
    bool isItem = ii >= 0;

    // 메뉴가 떠 있는 동안 대상 줄을 강조
    if (isItem)          { g_hoverItem = ii; g_hoverBtn = -1; }
    else if (g_dropOpen) { g_hoverDrop = gi; }
    InvalidateRect(hWnd, &pr, FALSE);
    UpdateWindow(hWnd);

    enum { CMD_DELETE = 1, CMD_MANAGE = 2 };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CMD_DELETE,
                isItem ? LS(STR_GP_DLG_DEL_ITEM) : LS(STR_GP_DLG_DEL_GROUP));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_MANAGE, LS(STR_MENU_GROUPMANAGE));
    POINT spt = { x, y };
    ClientToScreen(hWnd, &spt);
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                  spt.x, spt.y, 0, hWnd, nullptr);
    DestroyMenu(menu);

    if (cmd == CMD_DELETE && isItem) {
        DeleteItemWithConfirm(hWnd, ii, pr);
    } else if (cmd == CMD_DELETE) {
        g_dropOpen = false;
        DeleteGroupWithConfirm(hWnd, gi);
        ClampDropScroll(pr);
    } else if (cmd == CMD_MANAGE) {
        g_dropOpen = false;
        PostMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_GROUP_MANAGE, 0), 0);
    }
    InvalidateRect(hWnd, nullptr, FALSE);
    return true;
}

void GroupPanel::OnMouseWheel(int delta, const RECT& clientRect)
{
    if (!g_visible) return;
    TipHide();   // 목록이 움직이면 툴팁 위치가 어긋나므로 숨김
    if (g_dropOpen) {
        // 드롭다운이 열려 있으면 목록을 한 행씩 스크롤
        int steps = delta / WHEEL_DELTA;
        if (steps == 0 && delta != 0) steps = delta > 0 ? 1 : -1;
        g_scrollTab -= steps;
        ClampDropScroll(GetPanelRect(clientRect));
        g_hoverDrop = -1;
        return;
    }
    g_scrollItem -= delta/3;
    ClampItemScroll(GetPanelRect(clientRect));  // 끝을 넘어 빈 화면이 되지 않도록
}

// ── 그룹 데이터 저장/불러오기 ────────────────────────────────────
// 저장 형식 (UTF-8 텍스트):
//   GROUP:<그룹이름>
//   ITEM:<항목이름>
//   PATH:<pdf경로>
//   MEMO:<메모(줄바꿈→\n 이스케이프)>
//   (빈 줄로 구분 없음, 순서 고정)

static std::wstring GetGroupDataPath()
{
    WCHAR appData[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    std::wstring dir = std::wstring(appData) + L"\\PlainPDFViewer";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\groups.dat";
}

// wstring → UTF-8
static std::string ToUtf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8,0,ws.c_str(),(int)ws.size(),nullptr,0,nullptr,nullptr);
    std::string s(n,'\0');
    WideCharToMultiByte(CP_UTF8,0,ws.c_str(),(int)ws.size(),s.data(),n,nullptr,nullptr);
    return s;
}

// UTF-8 → wstring
static std::wstring FromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),nullptr,0);
    std::wstring ws(n,L'\0');
    MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),ws.data(),n);
    return ws;
}

// 줄바꿈 이스케이프 (\n → \\n)
static std::string EscapeNewlines(const std::wstring& ws)
{
    std::string s = ToUtf8(ws);
    std::string out; out.reserve(s.size());
    for (char c : s) {
        if (c=='\n') { out += "\\n"; }
        else if (c=='\r') { /* skip */ }
        else if (c=='\\') { out += "\\\\"; }
        else out += c;
    }
    return out;
}

// 이스케이프 복원 (파일 → 내부 표현 \n)
static std::wstring UnescapeNewlines(const std::string& s)
{
    std::string out; out.reserve(s.size());
    for (size_t i=0;i<s.size();++i) {
        if (s[i]=='\\' && i+1<s.size()) {
            if (s[i+1]=='n') { out+='\n'; ++i; }
            else if (s[i+1]=='\\') { out+='\\'; ++i; }
            else out+=s[i];
        } else out+=s[i];
    }
    return FromUtf8(out);
}

void GroupPanel::SaveGroups()
{
    std::wstring path = GetGroupDataPath();
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) return;

    // UTF-8 BOM
    unsigned char bom[]={0xEF,0xBB,0xBF};
    fwrite(bom,1,3,f);

    for (auto& gd : g_groups) {
        std::string line = "GROUP:" + ToUtf8(gd.name) + "\n";
        fwrite(line.c_str(),1,line.size(),f);
        for (auto& it : gd.items) {
            std::string iline = "ITEM:" + ToUtf8(it.name) + "\n";
            fwrite(iline.c_str(),1,iline.size(),f);
            std::string pline = "PATH:" + ToUtf8(it.pdfPath) + "\n";
            fwrite(pline.c_str(),1,pline.size(),f);
            std::string mline = "MEMO:" + EscapeNewlines(it.memo) + "\n";
            fwrite(mline.c_str(),1,mline.size(),f);
        }
    }
    fclose(f);
}

void GroupPanel::LoadGroups()
{
    std::wstring path = GetGroupDataPath();
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return;

    // 파일 전체 읽기
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    if (sz<=0) { fclose(f); return; }
    std::string raw(sz,'\0');
    fread(raw.data(),1,sz,f);
    fclose(f);

    // UTF-8 BOM 제거
    size_t start = 0;
    if (raw.size()>=3 &&
        (unsigned char)raw[0]==0xEF &&
        (unsigned char)raw[1]==0xBB &&
        (unsigned char)raw[2]==0xBF) start=3;

    g_groups.clear();
    g_selGroup = -1;

    // 줄 단위 파싱
    size_t pos = start;
    GroupDef* curGroup = nullptr;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) nl = raw.size();
        std::string line = raw.substr(pos, nl-pos);
        if (!line.empty() && line.back()=='\r') line.pop_back();
        pos = nl+1;

        if (line.substr(0,6)=="GROUP:") {
            g_groups.push_back({FromUtf8(line.substr(6)),{}});
            curGroup = &g_groups.back();
        } else if (line.substr(0,5)=="ITEM:" && curGroup) {
            GroupItem it; it.name = FromUtf8(line.substr(5));
            curGroup->items.push_back(it);
        } else if (line.substr(0,5)=="PATH:" && curGroup && !curGroup->items.empty()) {
            curGroup->items.back().pdfPath = FromUtf8(line.substr(5));
        } else if (line.substr(0,5)=="MEMO:" && curGroup && !curGroup->items.empty()) {
            curGroup->items.back().memo = UnescapeNewlines(line.substr(5));
        }
    }

    if (!g_groups.empty()) g_selGroup = 0;
    RefreshMissing(g_selGroup);  // 시작 지연을 줄이려고 선택된 그룹만 확인
}
