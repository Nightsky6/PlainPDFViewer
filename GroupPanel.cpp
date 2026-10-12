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
    int  g_scrollTab  = 0;    // 탭 영역 스크롤 오프셋
    int  g_scrollItem = 0;    // 항목 영역 스크롤 오프셋

    // ── 패널 표시 ────────────────────────────────────────────────
    bool g_visible = false;

    // ── 호버 상태 ────────────────────────────────────────────────
    bool g_hoverClose   = false;
    int  g_hoverTab     = -1;  // 드롭다운 목록 호버 인덱스
    bool g_hoverDropBtn = false;
    int  g_hoverItem    = -1;  // 호버된 항목 인덱스
    int  g_hoverBtn     = -1;  // 0=등록, 1=메모

    // ── 드롭다운 상태 ─────────────────────────────────────────────
    bool g_dropOpen   = false;  // 드롭다운 열림 여부
    int  g_hoverDrop  = -1;     // 드롭다운 목록 내 호버 인덱스

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
                DestroyWindow(hWnd);
                if (par) SetForegroundWindow(par);
            };
            if (PtInRect(&g_popup.okBtn,     pt)) commit(true);
            if (PtInRect(&g_popup.cancelBtn, pt)) commit(false);
        } break;

        case WM_KEYDOWN:
            if (wParam==VK_RETURN) PostMessage(hWnd,WM_LBUTTONUP,0,
                MAKELPARAM(g_popup.okBtn.left+1, g_popup.okBtn.top+1));
            if (wParam==VK_ESCAPE) PostMessage(hWnd,WM_LBUTTONUP,0,
                MAKELPARAM(g_popup.cancelBtn.left+1, g_popup.cancelBtn.top+1));
            break;

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

        MSG msg2;
        while (!g_popup.closed && GetMessage(&msg2,nullptr,0,0)) {
            if (msg2.hwnd==parent || IsChild(parent,msg2.hwnd)) {
                if ((msg2.message>=WM_MOUSEFIRST && msg2.message<=WM_MOUSELAST)||
                    (msg2.message>=WM_KEYFIRST   && msg2.message<=WM_KEYLAST)||
                    msg2.message==WM_NCLBUTTONDOWN) {
                    if (msg2.message==WM_LBUTTONDOWN||msg2.message==WM_NCLBUTTONDOWN)
                        FlashWindow(hwnd,TRUE);
                    continue;
                }
            }
            TranslateMessage(&msg2);
            DispatchMessage(&msg2);
        }
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

    void ShowMemoDialog(HWND parent, int groupIdx, int itemIdx)
    {
        if (groupIdx<0 || groupIdx>=(int)g_groups.size()) return;
        if (itemIdx<0  || itemIdx>=(int)g_groups[groupIdx].items.size()) return;

        auto& item = g_groups[groupIdx].items[itemIdx];

        constexpr int DW=600, DH=480;
        constexpr int HDR_H=48, FOOT_H=52;

        // ── 창 클래스 등록 ───────────────────────────────────
        static bool sReg=false;
        const wchar_t* MEMO_CLASS=L"GpMemoDlgClass";

        struct MemoState {
            HWND hwnd=nullptr;
            HWND hEdit=nullptr;
            bool closed=false;
            std::wstring itemName;
            std::wstring* memoTarget=nullptr;
            int hoverBtn=-1; // 0=저장, 1=닫기(푸터), 2=헤더 ×
            bool savedFeedback=false; // 저장됨 피드백 표시 중
        };
        static MemoState* smp=nullptr;
        MemoState ms;
        ms.itemName   = item.name;
        ms.memoTarget = &item.memo;
        smp = &ms;

        using RenderFnM = std::function<void(HDC)>;
        static RenderFnM* sMemoRender=nullptr;

        auto Render=[&](HDC hdc){
            using namespace Gdiplus;
            Graphics g(hdc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            FontFamily ff(L"Segoe UI");
            StringFormat sfC; sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentCenter);
            StringFormat sfL; sfL.SetLineAlignment(StringAlignmentCenter);

            // 배경
            SolidBrush bg(Color(255,250,251,255));
            g.FillRectangle(&bg,0.0f,0.0f,(float)DW,(float)DH);

            // 헤더
            SolidBrush hdrBg(Color(255,237,239,250));
            g.FillRectangle(&hdrBg,0.0f,0.0f,(float)DW,(float)HDR_H);
            Pen hdrLine(Color(255,210,215,235),1.0f);
            g.DrawLine(&hdrLine,0.0f,(float)HDR_H,(float)DW,(float)HDR_H);

            // 헤더 제목
            Font titleF(&ff,11,FontStyleBold,UnitPoint);
            SolidBrush titleBr(Color(255,30,30,50));
            std::wstring title=std::wstring(LS(STR_GP_MEMO_TITLE_PFX))+ms.itemName;
            g.DrawString(title.c_str(),-1,&titleF,RectF(16,0,(float)(DW-60),(float)HDR_H),&sfL,&titleBr);

            // 헤더 × 버튼
            {
                int bx=DW-40,by2=(HDR_H-24)/2;
                bool hov=(ms.hoverBtn==2);
                SolidBrush bb(hov?Color(255,220,80,80):Color(255,200,60,60));
                g.FillEllipse(&bb,(float)bx,(float)by2,24.0f,24.0f);
                Font cf(&ff,10,FontStyleBold,UnitPoint);
                SolidBrush ct(Color(255,255,255,255));
                g.DrawString(L"\u00D7",-1,&cf,RectF((float)bx,(float)by2,24,24),&sfC,&ct);
            }

            // 푸터 구분선
            Pen footLine(Color(255,210,215,235),1.0f);
            g.DrawLine(&footLine,0.0f,(float)(DH-FOOT_H),(float)DW,(float)(DH-FOOT_H));

            // 저장 버튼
            {
                int bx=DW-220, by3=DH-FOOT_H+11;
                bool hov=(ms.hoverBtn==0);
                bool saved=ms.savedFeedback;
                Color bc=saved?Color(255,0,120,212):hov?Color(255,30,140,230):Color(255,0,120,212);
                SolidBrush b(bc);
                GraphicsPath p; float d=6.0f,fx=(float)bx,fy=(float)by3,fw=96.0f,fh=30.0f;
                p.AddArc(fx,fy,d,d,180,90); p.AddArc(fx+fw-d,fy,d,d,270,90);
                p.AddArc(fx+fw-d,fy+fh-d,d,d,0,90); p.AddArc(fx,fy+fh-d,d,d,90,90);
                p.CloseFigure(); g.FillPath(&b,&p);
                Font f2(&ff,9,FontStyleBold,UnitPoint);
                SolidBrush t(Color(255,255,255,255));
                g.DrawString(saved?LS(STR_GP_MEMO_SAVED):LS(STR_GP_MEMO_SAVE),-1,&f2,RectF(fx,fy,fw,fh),&sfC,&t);
            }
            // 닫기 버튼
            {
                int bx=DW-112, by3=DH-FOOT_H+11;
                bool hov=(ms.hoverBtn==1);
                Color bc=hov?Color(255,100,105,130):Color(255,130,135,160);
                SolidBrush b(bc);
                GraphicsPath p; float d=6.0f,fx=(float)bx,fy=(float)by3,fw=96.0f,fh=30.0f;
                p.AddArc(fx,fy,d,d,180,90); p.AddArc(fx+fw-d,fy,d,d,270,90);
                p.AddArc(fx+fw-d,fy+fh-d,d,d,0,90); p.AddArc(fx,fy+fh-d,d,d,90,90);
                p.CloseFigure(); g.FillPath(&b,&p);
                Font f3(&ff,10,FontStyleBold,UnitPoint);
                SolidBrush t(Color(255,255,255,255));
                g.DrawString(LS(STR_GP_MEMO_CLOSE),-1,&f3,RectF(fx,fy,fw,fh),&sfC,&t);
            }
        };

        RenderFnM renderFn; sMemoRender=&renderFn;

        struct PH {
            static LRESULT CALLBACK Proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
            {
                if (!smp) return DefWindowProc(hWnd,msg,wParam,lParam);
                constexpr int DW=600,DH=480,HDR_H=48,FOOT_H=52;

                auto SaveAndClose=[&]{
                    if (smp->hEdit && smp->memoTarget) {
                        int len=GetWindowTextLengthW(smp->hEdit);
                        std::wstring t(len+1,L'\0');
                        GetWindowTextW(smp->hEdit,t.data(),len+1);
                        while(!t.empty()&&t.back()==L'\0') t.pop_back();
                        // EDIT 컨트롤의 \r\n을 내부 표현 \n으로 정규화
                        std::wstring norm; norm.reserve(t.size());
                        for (size_t i=0;i<t.size();++i) {
                            if (t[i]==L'\r' && i+1<t.size() && t[i+1]==L'\n') continue;
                            norm+=t[i];
                        }
                        *smp->memoTarget=norm;
                    }
                    smp->closed=true; DestroyWindow(hWnd);
                };
                auto SaveOnly=[&]{
                    if (smp->hEdit && smp->memoTarget) {
                        int len=GetWindowTextLengthW(smp->hEdit);
                        std::wstring t(len+1,L'\0');
                        GetWindowTextW(smp->hEdit,t.data(),len+1);
                        while(!t.empty()&&t.back()==L'\0') t.pop_back();
                        std::wstring norm; norm.reserve(t.size());
                        for (size_t i=0;i<t.size();++i) {
                            if (t[i]==L'\r' && i+1<t.size() && t[i+1]==L'\n') continue;
                            norm+=t[i];
                        }
                        *smp->memoTarget=norm;
                    }
                    smp->savedFeedback=true;
                    RECT rFoot={0,DH-FOOT_H,DW,DH};
                    InvalidateRect(hWnd,&rFoot,FALSE);
                    SetTimer(hWnd,1,1000,nullptr);
                };
                auto JustClose=[&]{ smp->closed=true; DestroyWindow(hWnd); };

                switch(msg)
                {
                case WM_PAINT: {
                    PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps);
                    RECT rc; GetClientRect(hWnd,&rc);
                    HDC mem=CreateCompatibleDC(hdc);
                    HBITMAP bmp=CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
                    HBITMAP old=(HBITMAP)SelectObject(mem,bmp);
                    if(sMemoRender) (*sMemoRender)(mem);
                    BitBlt(hdc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
                    SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem);
                    EndPaint(hWnd,&ps); } break;

                case WM_MOUSEMOVE: {
                    int x=GET_X_LPARAM(lParam),y=GET_Y_LPARAM(lParam);
                    POINT pt={x,y};
                    int nb=-1;
                    auto inBtn=[&](int bx,int by,int bw)->bool{
                        RECT r={bx,by,bx+bw,by+30}; return PtInRect(&r,pt)!=FALSE;
                    };
                    if(inBtn(DW-220,DH-FOOT_H+11,96)) nb=0;
                    else if(inBtn(DW-112,DH-FOOT_H+11,96)) nb=1;
                    // 헤더 ×
                    {int bx=DW-40,by2=(HDR_H-24)/2;RECT r={bx,by2,bx+24,by2+24};if(PtInRect(&r,pt))nb=2;}
                    if(nb!=smp->hoverBtn){
                        smp->hoverBtn=nb;
                        // 헤더와 푸터 영역만 무효화 (편집 영역 깜빡임 방지)
                        RECT rHdr={0,0,DW,HDR_H};
                        RECT rFoot={0,DH-FOOT_H,DW,DH};
                        InvalidateRect(hWnd,&rHdr,FALSE);
                        InvalidateRect(hWnd,&rFoot,FALSE);
                    }
                    TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hWnd,0};TrackMouseEvent(&tme);
                } break;

                case WM_MOUSELEAVE:
                    if(smp->hoverBtn!=-1){
                        smp->hoverBtn=-1;
                        RECT rHdr={0,0,DW,HDR_H};
                        RECT rFoot={0,DH-FOOT_H,DW,DH};
                        InvalidateRect(hWnd,&rHdr,FALSE);
                        InvalidateRect(hWnd,&rFoot,FALSE);
                    } break;

                case WM_LBUTTONUP: {
                    int x=GET_X_LPARAM(lParam),y=GET_Y_LPARAM(lParam);
                    POINT pt={x,y};
                    // 헤더 ×
                    {int bx=DW-40,by2=(HDR_H-24)/2;RECT r={bx,by2,bx+24,by2+24};
                     if(PtInRect(&r,pt)){JustClose();return 0;}}
                    auto inBtn=[&](int bx,int by,int bw)->bool{
                        RECT r={bx,by,bx+bw,by+30}; return PtInRect(&r,pt)!=FALSE;
                    };
                    if(inBtn(DW-220,DH-FOOT_H+11,96)){SaveOnly();return 0;}
                    if(inBtn(DW-112,DH-FOOT_H+11,96)){JustClose();return 0;}
                } break;

                case WM_TIMER:
                    if(wParam==1){
                        KillTimer(hWnd,1);
                        if(smp){ smp->savedFeedback=false; }
                        RECT rFoot={0,DH-FOOT_H,DW,DH};
                        InvalidateRect(hWnd,&rFoot,FALSE);
                    } break;

                case WM_KEYDOWN:
                    if(wParam==VK_ESCAPE){JustClose();return 0;}
                    break;

                case WM_NCHITTEST: {
                    LRESULT hit=DefWindowProc(hWnd,msg,wParam,lParam);
                    if(hit==HTCLIENT){
                        POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
                        ScreenToClient(hWnd,&pt);
                        if(pt.y<HDR_H) {
                            int bx=DW-40, by2=(HDR_H-24)/2;
                            RECT xBtn={bx, by2, bx+24, by2+24};
                            if(PtInRect(&xBtn, pt)) return HTCLIENT;
                            return HTCAPTION;
                        }
                    }
                    return hit; }

                case WM_DESTROY:
                    smp->closed=true; break;

                default: return DefWindowProc(hWnd,msg,wParam,lParam);
                }
                return 0;
            }
        };

        if (!sReg) {
            WNDCLASSEXW wc={};
            wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW;
            wc.lpfnWndProc=PH::Proc; wc.hInstance=hInst;
            wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
            wc.hbrBackground=nullptr; wc.lpszClassName=MEMO_CLASS;
            RegisterClassExW(&wc);
            sReg=true;
        }

        RECT pr2; GetWindowRect(parent,&pr2);
        int wx=pr2.left+(pr2.right-pr2.left-DW)/2;
        int wy=pr2.top+(pr2.bottom-pr2.top-DH)/2;

        HWND hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST,
            MEMO_CLASS,nullptr,WS_POPUP,wx,wy,DW,DH,parent,nullptr,hInst,nullptr);
        if (!hwnd) { smp=nullptr; sMemoRender=nullptr; return; }
        ms.hwnd=hwnd;

        DWM_WINDOW_CORNER_PREFERENCE corner=DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd,DWMWA_WINDOW_CORNER_PREFERENCE,&corner,sizeof(corner));
        MARGINS margins={1,1,1,1};
        DwmExtendFrameIntoClientArea(hwnd,&margins);

        // 에디트 컨트롤 생성 (헤더·푸터 제외 영역)
        constexpr int EDIT_PAD=12;
        int editTop=HDR_H+EDIT_PAD, editH=DH-HDR_H-EDIT_PAD-FOOT_H-EDIT_PAD;
        std::wstring editMemo = ToEditNewlines(item.memo);
        ms.hEdit=CreateWindowExW(0,L"EDIT",editMemo.c_str(),
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN,
            EDIT_PAD,editTop,DW-EDIT_PAD*2,editH,hwnd,nullptr,hInst,nullptr);
        HFONT hf=CreateFontW(20,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        SendMessage(ms.hEdit,WM_SETFONT,(WPARAM)hf,TRUE);
        // 에디트 배경·테두리를 창과 어울리게
        SetWindowLongW(ms.hEdit,GWL_EXSTYLE,
            GetWindowLongW(ms.hEdit,GWL_EXSTYLE)&~WS_EX_CLIENTEDGE);
        SetWindowPos(ms.hEdit,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_FRAMECHANGED);

        SetLayeredWindowAttributes(hwnd,0,0,LWA_ALPHA);
        ShowWindow(hwnd,SW_SHOW);
        AnimateWindow(hwnd,200,AW_BLEND);
        SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA);
        SetForegroundWindow(hwnd);
        SetFocus(ms.hEdit);
        // 커서를 텍스트 끝으로
        SendMessage(ms.hEdit,EM_SETSEL,(WPARAM)-1,(LPARAM)-1);

        MSG msg2;
        while (!ms.closed && GetMessage(&msg2,nullptr,0,0)) {
            if (msg2.message==WM_KEYDOWN && msg2.wParam==VK_ESCAPE) {
                ms.closed=true; DestroyWindow(hwnd); break;
            }
            if (msg2.hwnd==parent||IsChild(parent,msg2.hwnd)) {
                if((msg2.message>=WM_MOUSEFIRST&&msg2.message<=WM_MOUSELAST)||
                   (msg2.message>=WM_KEYFIRST&&msg2.message<=WM_KEYLAST)||
                   msg2.message==WM_NCLBUTTONDOWN) {
                    if(msg2.message==WM_LBUTTONDOWN||msg2.message==WM_NCLBUTTONDOWN)
                        FlashWindow(hwnd,TRUE);
                    continue;
                }
            }
            TranslateMessage(&msg2);
            DispatchMessage(&msg2);
        }
        if (hf) DeleteObject(hf);
        smp=nullptr; sMemoRender=nullptr;
        SetForegroundWindow(parent);
        SetFocus(parent);
    }

    void OpenMemoEdit(HWND hParent, int itemIdx, const RECT& /*clientRect*/)
    {
        // 레거시 호출 → 새 팝업으로 위임
        ShowMemoDialog(hParent, g_selGroup, itemIdx);
    }

    // ── 히트 테스트 ─────────────────────────────────────────────

    // 드롭다운 버튼 영역
    RECT HitDropdownBtn(const RECT& pr)
    {
        int ddW = pr.right - pr.left;
        RECT r = {pr.left, pr.top + GROUP_HEADER_H,
                  pr.left + ddW, pr.top + GROUP_HEADER_H + GROUP_TAB_H};
        return r;
    }

    // 드롭다운 목록 항목 영역 (열렸을 때)
    RECT HitDropdownItem(const RECT& pr, int idx)
    {
        int ddW = pr.right - pr.left;
        int top = pr.top + GROUP_HEADER_H + GROUP_TAB_H + idx * GROUP_TAB_H;
        return {pr.left, top, pr.left + ddW, top + GROUP_TAB_H};
    }

    // HitTab: 드롭다운 목록 내 인덱스 반환 (-1=없음)
    int HitTab(int x, int y, const RECT& pr)
    {
        if (!g_dropOpen) return -1;
        if (g_groups.empty()) return -1;
        for (int i = 0; i < (int)g_groups.size(); ++i) {
            RECT r = HitDropdownItem(pr, i);
            if (PtInRect(&r, {x, y})) return i;
        }
        return -1;
    }

    // 반환: 항목 인덱스(-1=없음), sub=0등록/1메모/-1=항목자체
    int HitItem(int x, int y, const RECT& pr, int& sub)
    {
        sub = -1;
        if (g_selGroup < 0 || g_selGroup >= (int)g_groups.size()) return -1;
        auto& grp = g_groups[g_selGroup];
        int topY = pr.top + GROUP_HEADER_H + GROUP_TAB_H;

        for (int i=0; i<(int)grp.items.size(); ++i) {
            int iy = topY + i * GROUP_ITEM_H - g_scrollItem;
            if (iy > pr.bottom) break;
            if (y >= iy && y < iy + GROUP_ITEM_H && x >= pr.left && x < pr.right) {
                // 오른쪽 버튼들 (등록/메모)
                int bH = 22, bW = 52;
                int bY2 = iy + (GROUP_ITEM_H - bH)/2;
                // 메모
                RECT rm = {pr.right-bW-4, bY2, pr.right-4, bY2+bH};
                if (PtInRect(&rm, {x,y})) { sub=1; return i; }
                // 등록
                RECT rr = {pr.right-bW*2-8, bY2, pr.right-bW-8, bY2+bH};
                if (PtInRect(&rr, {x,y})) { sub=0; return i; }
                return i; // 항목 자체
            }
        }
        return -1;
    }

    bool HitClose(int x, int y, const RECT& pr)
    {
        int bX = pr.right - 34, bY = pr.top + (GROUP_HEADER_H-22)/2;
        RECT r = {bX, bY, bX+26, bY+22};
        return PtInRect(&r, {x,y}) != FALSE;
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

void GroupPanel::Toggle(HWND hParent)
{
    g_visible = !g_visible;
    if (!g_visible) { CloseMemoEdit(); g_dropOpen = false; }
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
                    } else {
                        fileLabel = LS(STR_GP_NO_PDF);
                    }
                    Font pf(&ff,8,FontStyleRegular,UnitPoint);
                    SolidBrush pBr(hasPdf ? Color(255,60,100,180) : Color(255,170,175,195));
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

            auto PickPdf = [&]() -> std::wstring {
                WCHAR buf[MAX_PATH]={};
                OPENFILENAMEW ofn={};
                ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=hWnd;
                ofn.lpstrFilter=L"PDF \ud30c\uc77c (*.pdf)\0*.pdf\0\ubaa8\ub4e0 \ud30c\uc77c\0*.*\0";
                ofn.lpstrFile=buf; ofn.nMaxFile=MAX_PATH;
                ofn.lpstrTitle=L"PDF \ub4f1\ub85d"; ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
                return GetOpenFileNameW(&ofn) ? buf : L"";
            };

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
                 if(PtInRect(&r,pt)){sp->closed=true;DestroyWindow(hWnd);return 0;}}

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
                if(inBtn2(DW-108,96)){sp->closed=true;DestroyWindow(hWnd);return 0;}
            } break;

            case WM_LBUTTONDBLCLK: {
                // 항목 더블클릭 → PDF 경로 등록
                int x=GET_X_LPARAM(lParam),y=GET_Y_LPARAM(lParam);
                POINT pt={x,y};
                for(auto& a:sp->itmAreas) if(PtInRect(&a.rc,pt)){
                    sp->selItem=a.idx;
                    if(sp->selGroup>=0 && sp->selGroup<(int)g_groups.size()){
                        WCHAR buf[MAX_PATH]={};
                        OPENFILENAMEW ofn={};
                        ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=hWnd;
                        ofn.lpstrFilter=L"PDF \ud30c\uc77c (*.pdf)\0*.pdf\0\ubaa8\ub4e0 \ud30c\uc77c\0*.*\0";
                        ofn.lpstrFile=buf;ofn.nMaxFile=MAX_PATH;
                        ofn.lpstrTitle=L"PDF \ub4f1\ub85d";ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
                        if(GetOpenFileNameW(&ofn)) g_groups[sp->selGroup].items[sp->selItem].pdfPath=buf;
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

    MSG msg2;
    while (!st.closed && GetMessage(&msg2,nullptr,0,0)) {
        if (msg2.hwnd==parent||IsChild(parent,msg2.hwnd)) {
            if ((msg2.message>=WM_MOUSEFIRST&&msg2.message<=WM_MOUSELAST)||
                (msg2.message>=WM_KEYFIRST&&msg2.message<=WM_KEYLAST)||
                msg2.message==WM_NCLBUTTONDOWN) {
                if(msg2.message==WM_LBUTTONDOWN||msg2.message==WM_NCLBUTTONDOWN)
                    FlashWindow(hwnd,TRUE);
                continue;
            }
        }
        TranslateMessage(&msg2);
        DispatchMessage(&msg2);
    }

    // 선택된 그룹을 패널에 반영
    g_selGroup = st.selGroup;
    sRender = nullptr;
    sp = nullptr;
    SetForegroundWindow(parent);
    SetFocus(parent);
}

// ── 그리기 ──────────────────────────────────────────────────────
void GroupPanel::Draw(HDC hdc, const RECT& clientRect)
{
    if (!g_visible) return;
    using namespace Gdiplus;

    RECT pr = GetPanelRect(clientRect);
    int W  = pr.right - pr.left;
    int H  = pr.bottom - pr.top;
    int ox = pr.left;
    int oy = pr.top;

    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // 배경
    SolidBrush bgBr(Color(255,247,248,252));
    g.FillRectangle(&bgBr,(float)ox,(float)oy,(float)W,(float)H);
    Pen leftBorder(Color(255,210,215,230),1.0f);
    g.DrawLine(&leftBorder,(float)ox,(float)oy,(float)ox,(float)(oy+H));

    FontFamily ff(L"Segoe UI");
    StringFormat sfC;
    sfC.SetAlignment(StringAlignmentCenter);
    sfC.SetLineAlignment(StringAlignmentCenter);

    // ── 헤더 ──────────────────────────────────────────────────
    SolidBrush hdrBg(Color(255,237,239,248));
    g.FillRectangle(&hdrBg,(float)ox,(float)oy,(float)W,(float)GROUP_HEADER_H);

    Font titleF(&ff,10,FontStyleBold,UnitPoint);
    SolidBrush titleBr(Color(255,40,40,55));
    g.DrawString(LS(STR_GP_PANEL_TITLE),-1,&titleF,
        RectF((float)ox,(float)oy,(float)W,(float)GROUP_HEADER_H),&sfC,&titleBr);

    Pen hdrLine(Color(255,210,215,230),1.0f);
    g.DrawLine(&hdrLine,(float)ox,(float)(oy+GROUP_HEADER_H),(float)(ox+W),(float)(oy+GROUP_HEADER_H));

    // 헤더 버튼: × 닫기
    {
        int bX=ox+W-34, bY=oy+(GROUP_HEADER_H-22)/2;
        Color cb = g_hoverClose ? Color(255,240,215,215) : Color(255,248,230,230);
        SolidBrush b(cb); DrawRR(g,(float)bX,(float)bY,26,22,4,&b);
        Font f(&ff,10,FontStyleBold,UnitPoint);
        SolidBrush t(Color(255,180,60,60));
        g.DrawString(L"\u00D7",-1,&f,RectF((float)bX,(float)bY,26,22),&sfC,&t);
    }

    // ── 드롭다운 행 ───────────────────────────────────────────
    int tabY = oy + GROUP_HEADER_H;
    int ddW  = W; // 패널 전체 너비

    // 드롭다운 버튼 배경
    {
        bool hov = g_hoverDropBtn;
        SolidBrush ddBg(hov ? Color(255,218,224,245) : Color(255,230,233,245));
        g.FillRectangle(&ddBg,(float)ox,(float)tabY,(float)ddW,(float)GROUP_TAB_H);

        // 선택된 그룹명 표시
        std::wstring label = (g_selGroup>=0 && g_selGroup<(int)g_groups.size())
            ? g_groups[g_selGroup].name : LS(STR_GP_SEL_DROP);
        StringFormat sfDD;
        sfDD.SetLineAlignment(StringAlignmentCenter);
        sfDD.SetFormatFlags(StringFormatFlagsNoWrap);
        sfDD.SetTrimming(StringTrimmingEllipsisCharacter);
        Font ddF(&ff,9,FontStyleBold,UnitPoint);
        SolidBrush ddTxt(g_selGroup>=0 ? Color(255,20,60,140) : Color(255,130,135,155));
        g.DrawString(label.c_str(),-1,&ddF,
            RectF((float)(ox+10),(float)tabY,(float)(ddW-28),(float)GROUP_TAB_H),&sfDD,&ddTxt);

        // 화살표 (▼ 또는 ▲)
        Font arrF(&ff,7,FontStyleRegular,UnitPoint);
        SolidBrush arrBr(Color(255,80,90,130));
        const wchar_t* arrow = g_dropOpen ? L"\u25b2" : L"\u25bc";
        StringFormat sfArr; sfArr.SetAlignment(StringAlignmentCenter); sfArr.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(arrow,-1,&arrF,
            RectF((float)(ox+ddW-20),(float)tabY,20,(float)GROUP_TAB_H),&sfArr,&arrBr);
    }


    Pen tabLine(Color(255,200,205,225),1.0f);
    g.DrawLine(&tabLine,(float)ox,(float)(tabY+GROUP_TAB_H),(float)(ox+W),(float)(tabY+GROUP_TAB_H));

    // ── 항목 목록 ─────────────────────────────────────────────
    int listTop = tabY + GROUP_TAB_H;
    int listH   = H - GROUP_HEADER_H - GROUP_TAB_H;

    // 클립
    Region oldClip; g.GetClip(&oldClip);
    g.SetClip(RectF((float)ox,(float)listTop,(float)W,(float)listH));

    if (g_selGroup>=0 && g_selGroup<(int)g_groups.size()) {
        auto& grp = g_groups[g_selGroup];
        if (grp.items.empty()) {
            Font ef(&ff,8,FontStyleRegular,UnitPoint);
            SolidBrush eb(Color(255,170,175,195));
            g.DrawString(LS(STR_GP_SEL_HINT),
                -1,&ef,RectF((float)ox,(float)listTop,(float)W,(float)listH),&sfC,&eb);
        } else {
            for (int i=0; i<(int)grp.items.size(); ++i) {
                int iy = listTop + i*GROUP_ITEM_H - g_scrollItem;
                if (iy+GROUP_ITEM_H < listTop) continue;
                if (iy > listTop+listH) break;

                auto& item = grp.items[i];
                bool hov = (g_hoverItem==i);

                Color ibg = hov ? Color(255,225,230,248)
                                : (i%2==0 ? Color(255,255,255,255) : Color(255,249,250,254));
                SolidBrush ib(ibg);
                g.FillRectangle(&ib,(float)(ox+1),(float)iy,(float)(W-2),(float)GROUP_ITEM_H);

                // 항목 이름 (PDF 있으면 파란색)
                Font nf(&ff,9,FontStyleRegular,UnitPoint);
                Color ncol = item.pdfPath.empty() ? Color(255,40,40,55) : Color(255,0,100,200);
                SolidBrush nb(ncol);
                StringFormat sfN;
                sfN.SetLineAlignment(StringAlignmentCenter);
                sfN.SetFormatFlags(StringFormatFlagsNoWrap);
                sfN.SetTrimming(StringTrimmingEllipsisCharacter);
                g.DrawString(item.name.c_str(),-1,&nf,
                    RectF((float)(ox+8),(float)iy,(float)(W-8*3-52*2+20),(float)GROUP_ITEM_H),
                    &sfN,&nb);

                // 메모 아이콘 표시 (메모 있으면)
                if (!item.memo.empty()) {
                    Font mif(&ff,7,FontStyleRegular,UnitPoint);
                    SolidBrush mib(Color(255,120,140,200));
                    g.DrawString(L"\u270f",-1,&mif,
                        RectF((float)(ox+8),(float)(iy+GROUP_ITEM_H/2),(float)12,(float)(GROUP_ITEM_H/2)),
                        nullptr,&mib);
                }

                // 우측 버튼 2개: 등록/메모
                int bH=22, bW=52, bY2=iy+(GROUP_ITEM_H-bH)/2;
                // 등록 (PDF 미등록 시 라이트 그레이)
                bool hr = (hov && g_hoverBtn==0);
                bool hasPdf = !item.pdfPath.empty();
                Btn(g,ff,(float)(ox+W-bW*2-8),(float)bY2,(float)bW,(float)bH,
                    LS(STR_GP_REG_BTN),hr,
                    hasPdf ? Color(255,200,230,210) : Color(255,218,220,225),
                    hasPdf ? Color(255,150,210,175) : Color(255,195,198,205),
                    hasPdf ? Color(255,20,100,50)   : Color(255,140,143,150),
                    8);
                // 메모
                bool hm = (hov && g_hoverBtn==1);
                Btn(g,ff,(float)(ox+W-bW-4),(float)bY2,(float)bW,(float)bH,
                    LS(STR_GP_MEMO_BTN),hm,
                    Color(255,200,215,240),Color(255,150,175,225),
                    Color(255,20,50,120),8);

                // 구분선
                Pen rowLine(Color(255,225,228,238),1.0f);
                g.DrawLine(&rowLine,(float)(ox+1),(float)(iy+GROUP_ITEM_H),
                           (float)(ox+W-1),(float)(iy+GROUP_ITEM_H));
            }
        }
    } else if (g_groups.empty()) {
        Font ef(&ff,8,FontStyleRegular,UnitPoint);
        SolidBrush eb(Color(255,160,165,185));
        g.DrawString(LS(STR_GP_ADD_FIRST),
            -1,&ef,RectF((float)ox,(float)listTop,(float)W,(float)listH),&sfC,&eb);
    }

    g.SetClip(&oldClip);

    // ── 드롭다운 오버레이 (열렸을 때, 클립 해제 후 최상단에 그리기) ──
    if (g_dropOpen && !g_groups.empty()) {
        int ddW = W - 34 - 4;
        int n   = (int)g_groups.size();
        // 그림자 효과
        SolidBrush shadow(Color(40,0,0,0));
        g.FillRectangle(&shadow,(float)(ox+2),(float)(tabY+GROUP_TAB_H+2),(float)(ddW),(float)(n*GROUP_TAB_H));
        // 배경
        SolidBrush listBg(Color(255,245,246,252));
        g.FillRectangle(&listBg,(float)ox,(float)(tabY+GROUP_TAB_H),(float)(ddW),(float)(n*GROUP_TAB_H));
        Pen listBorder(Color(255,180,185,215),1.0f);
        g.DrawRectangle(&listBorder,(float)ox,(float)(tabY+GROUP_TAB_H),(float)(ddW-1),(float)(n*GROUP_TAB_H-1));

        for (int i=0; i<n; ++i) {
            bool hov = (i == g_hoverDrop);
            bool sel = (i == g_selGroup);
            if (hov || sel) {
                SolidBrush rowBg(sel ? Color(255,215,228,255) : Color(255,232,237,252));
                g.FillRectangle(&rowBg,(float)(ox+1),(float)(tabY+GROUP_TAB_H+i*GROUP_TAB_H+1),
                    (float)(ddW-2),(float)(GROUP_TAB_H-1));
            }
            StringFormat sfItem;
            sfItem.SetLineAlignment(StringAlignmentCenter);
            sfItem.SetFormatFlags(StringFormatFlagsNoWrap);
            sfItem.SetTrimming(StringTrimmingEllipsisCharacter);
            Font itemF(&ff,9,sel?FontStyleBold:FontStyleRegular,UnitPoint);
            SolidBrush itemTxt(sel ? Color(255,0,70,180) : Color(255,40,45,70));
            g.DrawString(g_groups[i].name.c_str(),-1,&itemF,
                RectF((float)(ox+12),(float)(tabY+GROUP_TAB_H+i*GROUP_TAB_H),
                      (float)(ddW-16),(float)GROUP_TAB_H),
                &sfItem,&itemTxt);
        }
    }
}

// ── 마우스 이벤트 ────────────────────────────────────────────────

void GroupPanel::OnMouseMove(HWND hWnd, int x, int y, const RECT& clientRect)
{
    if (!g_visible) return;
    RECT pr = GetPanelRect(clientRect);
    if (x < pr.left) { OnMouseLeave(); return; }

    bool  nc  = HitClose(x,y,pr);
    int   nt  = HitTab(x,y,pr);       // 드롭다운 목록 내 인덱스 (열렸을 때)
    int   sub = -1;
    int   ni  = HitItem(x,y,pr,sub);

    // 드롭다운 버튼 호버
    RECT ddBtn = HitDropdownBtn(pr);
    bool hDropBtn = PtInRect(&ddBtn,{x,y}) != FALSE;

    bool changed = (nc!=g_hoverClose)||(nt!=g_hoverDrop)||(ni!=g_hoverItem)
                  ||(sub!=g_hoverBtn)||(hDropBtn!=g_hoverDropBtn);
    g_hoverClose  = nc;
    g_hoverDrop   = nt;
    g_hoverItem   = ni;
    g_hoverBtn    = sub;
    g_hoverDropBtn= hDropBtn;

    if (changed) {
        RECT inv={pr.left,pr.top,pr.right,pr.bottom};
        InvalidateRect(hWnd,&inv,FALSE);
    }

    // 커서: 클릭 가능한 요소 위에서는 손 모양
    bool isClickable = nc || hDropBtn || (nt>=0) || (ni>=0);
    SetCursor(LoadCursor(nullptr, isClickable ? IDC_HAND : IDC_ARROW));

    TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hWnd,0};
    TrackMouseEvent(&tme);
}

void GroupPanel::OnMouseLeave()
{
    g_hoverClose=false; g_hoverTab=-1; g_hoverDrop=-1;
    g_hoverDropBtn=false; g_hoverItem=-1; g_hoverBtn=-1;
}

void GroupPanel::OnLButtonDown(HWND hWnd, int x, int y, const RECT& clientRect)
{
    (void)hWnd; (void)x; (void)y; (void)clientRect;
}

void GroupPanel::OnLButtonUp(HWND hWnd, int x, int y, const RECT& clientRect)
{
    if (!g_visible) return;
    RECT pr = GetPanelRect(clientRect);
    if (x < pr.left) return;

    // 닫기
    if (HitClose(x,y,pr)) { Toggle(hWnd); return; }

    // 드롭다운 목록이 열려 있으면 목록 클릭 / 바깥 클릭으로 닫기
    if (g_dropOpen) {
        int ti = HitTab(x,y,pr);
        if (ti >= 0) {
            if (g_selGroup != ti) {
                CloseMemoEdit();
                g_scrollItem = 0;
            }
            g_selGroup = ti;
        }
        g_dropOpen = false;
        InvalidateRect(hWnd,nullptr,FALSE);
        return;
    }

    // 드롭다운 버튼 클릭 → 목록 열기
    {
        RECT ddBtn = HitDropdownBtn(pr);
        if (PtInRect(&ddBtn,{x,y})) {
            g_dropOpen = !g_groups.empty();
            InvalidateRect(hWnd,nullptr,FALSE);
            return;
        }
    }

    // 항목 클릭
    int sub=-1;
    int idx = HitItem(x,y,pr,sub);
    if (idx<0) return;

    auto& grp = g_groups[g_selGroup];

    if (sub==0) {
        // 등록: 파일 다이얼로그
        WCHAR szFile[MAX_PATH]={};
        OPENFILENAMEW ofn={};
        ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=hWnd;
        ofn.lpstrFilter=L"PDF \ud30c\uc77c (*.pdf)\0*.pdf\0\ubaa8\ub4e0 \ud30c\uc77c\0*.*\0";
        ofn.lpstrFile=szFile; ofn.nMaxFile=MAX_PATH;
        ofn.lpstrTitle=L"PDF \ub4f1\ub85d";
        ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
        ofn.lpstrDefExt=L"pdf";
        if (GetOpenFileNameW(&ofn)) {
            grp.items[idx].pdfPath = szFile;
        }
    } else if (sub==1) {
        // 메모
        if (g_memoEditItem==idx) CloseMemoEdit();
        else { CloseMemoEdit(); OpenMemoEdit(hWnd,idx,clientRect); }
    } else {
        // 항목 자체 클릭 → 등록된 PDF 열기
        CloseMemoEdit();
        if (!grp.items[idx].pdfPath.empty()) {
            OpenOrSwitchToTab(hWnd, grp.items[idx].pdfPath, true);
        }
    }
    InvalidateRect(hWnd,nullptr,FALSE);
}

void GroupPanel::OnMouseWheel(int delta)
{
    if (!g_visible) return;
    g_scrollItem -= delta/3;
    if (g_scrollItem<0) g_scrollItem=0;
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
}
