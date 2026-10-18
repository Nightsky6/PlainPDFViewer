// KeywordDialog.cpp
#include "framework.h"
#include "Lang.h"
#include "KeywordDialog.h"
#include "PlainPDFViewer.h"
#include <fpdfview.h>
#include <fpdf_text.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <string>
#include <algorithm>
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"gdiplus.lib")
std::vector<KeywordEntry> g_keywordList;
static std::vector<HighlightItem> g_keywordHighlights;
static constexpr int KW_W=420,KW_H=490,KW_PADDING=20,KW_BTN_H=34,KW_BTN_W=110;
static HWND g_kwWnd=nullptr,g_kwEditBox=nullptr,g_kwCombo=nullptr,g_kwList=nullptr;
static bool g_kwDone=false,g_kwAddHover=false,g_kwDelHover=false,g_kwCloseHover=false,g_kwAddPressed=false;

#include "Strings.h"

static const COLORREF kColors[] = {
    RGB(255,255,80),
    RGB(144,238,144),
    RGB(135,206,235),
    RGB(255,182,193),
    RGB(255,200,100),
    RGB(210,180,255),
};
static constexpr int kColorCount = _countof(kColors);

static const wchar_t* KColorName(int i) {
    switch(i) {
    case 0: return LS(STR_KW_COL_YELLOW);
    case 1: return LS(STR_KW_COL_GREEN);
    case 2: return LS(STR_KW_COL_SKY);
    case 3: return LS(STR_KW_COL_PINK);
    case 4: return LS(STR_KW_COL_ORANGE);
    case 5: return LS(STR_KW_COL_PURPLE);
    default: return L"";
    }
}

static Gdiplus::GraphicsPath* KwRoundRect(float x,float y,float w,float h,float r){
    auto* p=new Gdiplus::GraphicsPath();
    p->AddArc(x,y,r*2,r*2,180,90); p->AddArc(x+w-r*2,y,r*2,r*2,270,90);
    p->AddArc(x+w-r*2,y+h-r*2,r*2,r*2,0,90); p->AddArc(x,y+h-r*2,r*2,r*2,90,90);
    p->CloseFigure(); return p;
}

static void DrawKwButton(Gdiplus::Graphics& gfx,const RECT& rc,const wchar_t* text,
    bool hover,bool pressed,Gdiplus::Color bg1,Gdiplus::Color bg2,Gdiplus::Color fg){
    float x=(float)rc.left,y=(float)rc.top,w=(float)(rc.right-rc.left),h=(float)(rc.bottom-rc.top);
    auto* path=KwRoundRect(x,y,w,h,6.f);
    if(pressed){Gdiplus::Color t=bg1;bg1=bg2;bg2=t;}
    Gdiplus::LinearGradientBrush br(Gdiplus::PointF(x,y),Gdiplus::PointF(x,y+h),bg1,bg2);
    gfx.FillPath(&br,path);
    if(hover){Gdiplus::Pen pen(Gdiplus::Color(180,255,255,255),1.f);gfx.DrawPath(&pen,path);}
    delete path;
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff,9.5f,Gdiplus::FontStyleRegular,Gdiplus::UnitPoint);
    Gdiplus::SolidBrush tb(fg);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    gfx.DrawString(text,-1,&font,Gdiplus::RectF(x,y,w,h),&sf,&tb);
}

static RECT GetAddBtnRect(){int y=KW_PADDING+32+6;return{KW_W-KW_PADDING-KW_BTN_W,y,KW_W-KW_PADDING,y+42};}
static RECT GetDelBtnRect(){int y=KW_H-KW_PADDING-KW_BTN_H;return{KW_PADDING,y,KW_PADDING+KW_BTN_W,y+KW_BTN_H};}
static RECT GetCloseBtnRect(){int y=KW_H-KW_PADDING-KW_BTN_H;return{KW_W-KW_PADDING-KW_BTN_W,y,KW_W-KW_PADDING,y+KW_BTN_H};}
static void InvalidateBtns(HWND hWnd){
    RECT ra=GetAddBtnRect(),rd=GetDelBtnRect(),rc=GetCloseBtnRect();
    InvalidateRect(hWnd,&ra,FALSE);
    InvalidateRect(hWnd,&rd,FALSE);
    InvalidateRect(hWnd,&rc,FALSE);
}

static void RefreshList(){
    if(!g_kwList)return;
    SendMessageW(g_kwList,LB_RESETCONTENT,0,0);
    for(auto& e:g_keywordList){SendMessageW(g_kwList,LB_ADDSTRING,0,(LPARAM)e.keyword.c_str());}
}
static void DrawKwDialog(HWND hWnd){
    PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps);
    RECT cr; GetClientRect(hWnd,&cr);
    // 더블버퍼링
    HDC memDC=CreateCompatibleDC(hdc);
    HBITMAP memBM=CreateCompatibleBitmap(hdc,cr.right,cr.bottom);
    HBITMAP oldBM=(HBITMAP)SelectObject(memDC,memBM);
    FillRect(memDC,&cr,(HBRUSH)GetStockObject(WHITE_BRUSH));

    Gdiplus::Graphics gfx(memDC);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    float W=(float)cr.right, H=(float)cr.bottom;
    Gdiplus::FontFamily ff(L"Segoe UI");

    // 흰 배경
    Gdiplus::SolidBrush bgBr(Gdiplus::Color(255,255,255,255));
    gfx.FillRectangle(&bgBr,0.f,0.f,W,H);
    // 테두리
    Gdiplus::Pen borderPen(Gdiplus::Color(255,218,218,218),1.f);
    gfx.DrawRectangle(&borderPen,0.5f,0.5f,W-1.f,H-1.f);

    // 제목
    {Gdiplus::Font font(&ff,11,Gdiplus::FontStyleBold,Gdiplus::UnitPoint);
    Gdiplus::SolidBrush br(Gdiplus::Color(255,20,20,20));
    gfx.DrawString(LS(STR_KW_TITLE),-1,&font,Gdiplus::RectF((float)KW_PADDING,18.f,W-KW_PADDING*2.f,28.f),nullptr,&br);}

    // 구분선
    {Gdiplus::Pen sep(Gdiplus::Color(255,218,218,218),1.f);
    gfx.DrawLine(&sep,(float)KW_PADDING,52.f,W-(float)KW_PADDING,52.f);}

    // 레이블
    {Gdiplus::Font lbFont(&ff,9,Gdiplus::FontStyleRegular,Gdiplus::UnitPoint);
    Gdiplus::SolidBrush grayBr(Gdiplus::Color(255,95,95,95));
    Gdiplus::StringFormat sf; sf.SetAlignment(Gdiplus::StringAlignmentNear);
    gfx.DrawString(LS(STR_KW_LABEL_INPUT),-1,&lbFont,Gdiplus::RectF((float)KW_PADDING,58.f,200.f,18.f),&sf,&grayBr);
    int listLabelY=KW_PADDING+32+6+KW_BTN_H+10;
    gfx.DrawString(LS(STR_KW_LABEL_LIST),-1,&lbFont,Gdiplus::RectF((float)KW_PADDING,(float)listLabelY,200.f,18.f),&sf,&grayBr);}

    // 버튼 그리기 람다
    auto DrawBtn=[&](const RECT& rc2, const wchar_t* text, bool hover, bool pressed, bool isPrimary){
        float x=(float)rc2.left,y=(float)rc2.top,w=(float)(rc2.right-rc2.left),h=(float)(rc2.bottom-rc2.top);
        Gdiplus::RectF rf(x+0.5f,y+0.5f,w-1.f,h-1.f);
        auto* path=KwRoundRect(rf.X,rf.Y,rf.Width,rf.Height,4.f);
        if(isPrimary){
            Gdiplus::Color top=hover?Gdiplus::Color(255,40,160,255):Gdiplus::Color(255,0,140,240);
            Gdiplus::Color bot=hover?Gdiplus::Color(255,0,110,200):Gdiplus::Color(255,0,80,180);
            if(pressed){Gdiplus::Color t=top;top=bot;bot=t;}
            Gdiplus::LinearGradientBrush br(Gdiplus::PointF(x,y),Gdiplus::PointF(x,y+h),top,bot);
            gfx.FillPath(&br,path);
            Gdiplus::Pen hp(Gdiplus::Color(80,255,255,255),1.f);
            gfx.DrawLine(&hp,x+5,y+1.5f,x+w-5,y+1.5f);
        }else{
            Gdiplus::Color top=hover?Gdiplus::Color(255,252,252,252):Gdiplus::Color(255,255,255,255);
            Gdiplus::Color bot=hover?Gdiplus::Color(255,230,230,230):Gdiplus::Color(255,242,242,242);
            if(pressed){Gdiplus::Color t=top;top=bot;bot=t;}
            Gdiplus::LinearGradientBrush br(Gdiplus::PointF(x,y),Gdiplus::PointF(x,y+h),top,bot);
            gfx.FillPath(&br,path);
            Gdiplus::Pen bp(Gdiplus::Color(255,200,200,200),1.f);
            gfx.DrawPath(&bp,path);
        }
        delete path;
        Gdiplus::Font font(&ff,9,Gdiplus::FontStyleBold,Gdiplus::UnitPoint);
        Gdiplus::SolidBrush tb(isPrimary?Gdiplus::Color(255,255,255,255):Gdiplus::Color(255,45,45,45));
        Gdiplus::StringFormat sf; sf.SetAlignment(Gdiplus::StringAlignmentCenter); sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        gfx.DrawString(text,-1,&font,Gdiplus::RectF(x,y,w,h),&sf,&tb);
    };
    RECT a=GetAddBtnRect(),d=GetDelBtnRect(),cl=GetCloseBtnRect();
    DrawBtn(a,LS(STR_KW_BTN_ADD),g_kwAddHover,g_kwAddPressed,true);
    DrawBtn(d,LS(STR_KW_BTN_DEL),g_kwDelHover,false,false);
    DrawBtn(cl,LS(STR_KW_BTN_CLOSE),g_kwCloseHover,false,false);

    // 더티 영역만 복사 → 자식 컨트롤 영역을 건드리지 않아 깜빡임 방지
    BitBlt(hdc,
        ps.rcPaint.left, ps.rcPaint.top,
        ps.rcPaint.right  - ps.rcPaint.left,
        ps.rcPaint.bottom - ps.rcPaint.top,
        memDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(memDC,oldBM); DeleteObject(memBM); DeleteDC(memDC);
    EndPaint(hWnd,&ps);
}
static LRESULT CALLBACK KwWndProc(HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam){
    switch(msg){
    case WM_PAINT: DrawKwDialog(hWnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MEASUREITEM:{
        MEASUREITEMSTRUCT* mis=(MEASUREITEMSTRUCT*)lParam;
        mis->itemHeight=32; return TRUE;}
    case WM_DRAWITEM:{
        DRAWITEMSTRUCT* dis=(DRAWITEMSTRUCT*)lParam;
        if(dis->hwndItem!=g_kwList||dis->itemID==(UINT)-1) break;
        bool sel=(dis->itemState&ODS_SELECTED)!=0;
        // 배경
        COLORREF bgCol=sel?RGB(0,120,212):RGB(255,255,255);
        HBRUSH bgBr=CreateSolidBrush(bgCol);
        FillRect(dis->hDC,&dis->rcItem,bgBr);
        DeleteObject(bgBr);
        // 텍스트
        wchar_t buf[256]={};
        SendMessageW(g_kwList,LB_GETTEXT,dis->itemID,(LPARAM)buf);
        SetBkMode(dis->hDC,TRANSPARENT);
        SetTextColor(dis->hDC,sel?RGB(255,255,255):RGB(20,20,20));
        RECT tr=dis->rcItem; tr.left+=8; tr.right-=28;
        DrawTextW(dis->hDC,buf,-1,&tr,DT_SINGLELINE|DT_VCENTER|DT_LEFT);
        // 색상 점
        if(dis->itemID<g_keywordList.size()){
            COLORREF c=g_keywordList[dis->itemID].color;
            Gdiplus::Graphics gfx(dis->hDC);
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            float cy=(float)(dis->rcItem.top+dis->rcItem.bottom)/2.f;
            float cx=(float)(dis->rcItem.right-20);
            Gdiplus::SolidBrush cb(Gdiplus::Color(255,GetRValue(c),GetGValue(c),GetBValue(c)));
            Gdiplus::Pen cp(Gdiplus::Color(255,160,160,160),1.f);
            gfx.FillEllipse(&cb,cx-7,cy-7,14.f,14.f);
            gfx.DrawEllipse(&cp,cx-7,cy-7,14.f,14.f);
        }
        return TRUE;}
    case WM_MOUSEMOVE:{
        POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        RECT ra=GetAddBtnRect(),rd=GetDelBtnRect(),rc=GetCloseBtnRect();
        bool a=!!PtInRect(&ra,pt),d=!!PtInRect(&rd,pt),c=!!PtInRect(&rc,pt);
        if(a!=g_kwAddHover||d!=g_kwDelHover||c!=g_kwCloseHover){
            g_kwAddHover=a;g_kwDelHover=d;g_kwCloseHover=c;
            InvalidateBtns(hWnd);
        }
        TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hWnd,0};
        TrackMouseEvent(&tme);
        return 0;}
    case WM_MOUSELEAVE:
        g_kwAddHover=g_kwDelHover=g_kwCloseHover=false;
        InvalidateBtns(hWnd);
        return 0;
    case WM_PARENTNOTIFY:
        if(LOWORD(wParam)==WM_LBUTTONDOWN){
            POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            ScreenToClient(hWnd,&pt);
            if(g_kwList){
                RECT lr; GetWindowRect(g_kwList,&lr); MapWindowPoints(nullptr,hWnd,(LPPOINT)&lr,2);
                if(!PtInRect(&lr,pt)){
                    SendMessageW(g_kwList,LB_SETCURSEL,(WPARAM)-1,0);
                    SetFocus(g_kwEditBox); InvalidateRect(g_kwList,nullptr,TRUE); UpdateWindow(g_kwList);
                }
            }
        }
        return 0;
    case WM_LBUTTONDOWN:{
        POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        // 목록 외 클릭 시 선택 해제
        if(g_kwList){
            RECT lr; GetWindowRect(g_kwList,&lr); MapWindowPoints(nullptr,hWnd,(LPPOINT)&lr,2);
            if(!PtInRect(&lr,pt)){
                SendMessageW(g_kwList,LB_SETCURSEL,(WPARAM)-1,0);
                SetFocus(g_kwEditBox); InvalidateRect(g_kwList,nullptr,TRUE); UpdateWindow(g_kwList);
            }
        }
        RECT ra2=GetAddBtnRect(),rd2=GetDelBtnRect(),rc2=GetCloseBtnRect();
        if(PtInRect(&ra2,pt)){
            wchar_t buf[256]={};GetWindowTextW(g_kwEditBox,buf,256);
            std::wstring kw(buf);
            while(!kw.empty()&&iswspace(kw.front()))kw.erase(kw.begin());
            while(!kw.empty()&&iswspace(kw.back()))kw.pop_back();
            if(!kw.empty()){int sel=(int)SendMessageW(g_kwCombo,CB_GETCURSEL,0,0);if(sel<0)sel=0;
            COLORREF color=kColors[sel];bool dup=false;
            for(auto& e:g_keywordList)if(_wcsicmp(e.keyword.c_str(),kw.c_str())==0){dup=true;break;}
            if(!dup){g_keywordList.push_back({kw,color});RefreshList();SetWindowTextW(g_kwEditBox,L"");
                ApplyKeywordHighlights(GetParent(hWnd));
            }}
            InvalidateBtns(hWnd);
        }else if(PtInRect(&rd2,pt)){
            int sel=g_kwList?(int)SendMessageW(g_kwList,LB_GETCURSEL,0,0):-1;
            if(sel>=0&&sel<(int)g_keywordList.size()){g_keywordList.erase(g_keywordList.begin()+sel);RefreshList();
                ApplyKeywordHighlights(GetParent(hWnd));
                InvalidateBtns(hWnd);}
        }else if(PtInRect(&rc2,pt)){PostMessageW(hWnd,WM_CLOSE,0,0);}
        return 0;}
    case WM_NCHITTEST:{
        LRESULT hit=DefWindowProcW(hWnd,msg,wParam,lParam);
        if(hit==HTCLIENT){
            POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            ScreenToClient(hWnd,&pt);
            RECT ra=GetAddBtnRect(),rd=GetDelBtnRect(),rc=GetCloseBtnRect();
            if(!PtInRect(&ra,pt)&&!PtInRect(&rd,pt)&&!PtInRect(&rc,pt)&&!g_kwEditBox&&!g_kwCombo&&!g_kwList)
                return HTCAPTION;
            // 입력 컨트롤 영역이 아니면 드래그 가능
            RECT editRc,comboRc,listRc;
            if(g_kwEditBox){GetWindowRect(g_kwEditBox,&editRc);MapWindowPoints(nullptr,hWnd,(LPPOINT)&editRc,2);}
            if(g_kwCombo){GetWindowRect(g_kwCombo,&comboRc);MapWindowPoints(nullptr,hWnd,(LPPOINT)&comboRc,2);}
            if(g_kwList){GetWindowRect(g_kwList,&listRc);MapWindowPoints(nullptr,hWnd,(LPPOINT)&listRc,2);}
            if(!PtInRect(&ra,pt)&&!PtInRect(&rd,pt)&&!PtInRect(&rc,pt)
               &&(!g_kwEditBox||!PtInRect(&editRc,pt))
               &&(!g_kwCombo||!PtInRect(&comboRc,pt))
               &&(!g_kwList||!PtInRect(&listRc,pt)))
                return HTCAPTION;
        }
        return hit;}
    case WM_CLOSE:
        for(int a=255;a>=0;a-=30){SetLayeredWindowAttributes(hWnd,0,(BYTE)a,LWA_ALPHA);UpdateWindow(hWnd);Sleep(10);}
        DestroyWindow(hWnd); return 0;
    case WM_DESTROY:
    {
        HWND hPar = GetParent(hWnd);
        g_kwWnd = nullptr;
        if (hPar) { SetForegroundWindow(hPar); SetFocus(hPar); }
        return 0;
    }
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}
void ApplyKeywordHighlights(HWND hWnd){
    if(!g_pdfDoc||g_keywordList.empty())return;
    RemoveKeywordHighlights();
    for(auto& entry:g_keywordList){
        const std::wstring& kw=entry.keyword;if(kw.empty())continue;
        for(int pg=0;pg<g_pageCount;pg++){
            FPDF_PAGE page=FPDF_LoadPage(g_pdfDoc,pg);if(!page)continue;
            FPDF_TEXTPAGE tp=FPDFText_LoadPage(page);if(!tp){FPDF_ClosePage(page);continue;}
            int total=FPDFText_CountChars(tp);
            if(total>0){
                std::wstring pt2(total+1,L'\0');
                FPDFText_GetText(tp,0,total,reinterpret_cast<unsigned short*>(&pt2[0]));pt2.resize(total);
                std::wstring lp=pt2,lk=kw;
                std::transform(lp.begin(),lp.end(),lp.begin(),::towlower);
                std::transform(lk.begin(),lk.end(),lk.begin(),::towlower);
                size_t pos=0;
                while((pos=lp.find(lk,pos))!=std::wstring::npos){
                    HighlightItem hl{};hl.pageIndex=pg;hl.charStart=(int)pos;hl.charEnd=(int)(pos+kw.size()-1);hl.color=entry.color;
                    g_highlights.push_back(hl);g_keywordHighlights.push_back(hl);pos+=kw.size();
                }
            }
            FPDFText_ClosePage(tp);FPDF_ClosePage(page);
        }
    }
    g_dirty=true;InvalidateRect(hWnd,nullptr,TRUE);
}

void RemoveKeywordHighlights(){
    if(g_keywordHighlights.empty())return;
    g_highlights.erase(std::remove_if(g_highlights.begin(),g_highlights.end(),
        [](const HighlightItem& h){
            for(auto& kh:g_keywordHighlights)
                if(kh.pageIndex==h.pageIndex&&kh.charStart==h.charStart&&kh.charEnd==h.charEnd)return true;
            return false;}),g_highlights.end());
    g_keywordHighlights.clear();
}

void ShowKeywordDialog(HWND hParent){
    if(g_kwWnd){SetForegroundWindow(g_kwWnd);return;}
    static bool registered=false;
    if(!registered){
        WNDCLASSEXW wc={};wc.cbSize=sizeof(wc);wc.lpfnWndProc=KwWndProc;wc.hInstance=hInst;
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=nullptr;
        wc.lpszClassName=L"PlainPDFKeywordDlg";RegisterClassExW(&wc);registered=true;
    }
    RECT pr;GetWindowRect(hParent,&pr);
    int tx=pr.left+(pr.right-pr.left-KW_W)/2,ty=pr.top+(pr.bottom-pr.top-KW_H)/2;
    g_kwDone=false;
    g_kwWnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST,L"PlainPDFKeywordDlg",L"",WS_POPUP,
        tx,ty,KW_W,KW_H,hParent,nullptr,hInst,nullptr);
    if(!g_kwWnd)return;
    // DWM 둥근 모서리 + 그림자 (SetWindowRgn 없이)
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_kwWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    MARGINS margins = {1,1,1,1};
    DwmExtendFrameIntoClientArea(g_kwWnd, &margins);
    // 폰트 먼저 생성
    HFONT hFont = CreateFontW(21, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontList = CreateFontW(21, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    // 폰트 기준으로 Edit 높이 계산 (텍스트 높이 + 여백)
    HDC hdc = GetDC(g_kwWnd);
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
    TEXTMETRICW tm; GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, oldFont);
    ReleaseDC(g_kwWnd, hdc);
    int editH = tm.tmHeight + tm.tmExternalLeading + 10; // 여백 포함

    int editY=KW_PADDING+32+6,editW=KW_W-KW_PADDING*2-KW_BTN_W-10-95;
    g_kwEditBox=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
        KW_PADDING,editY,editW,editH,g_kwWnd,nullptr,hInst,nullptr);
    g_kwCombo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        KW_PADDING+editW+10,editY,85,200,g_kwWnd,nullptr,hInst,nullptr);
    for(int i=0;i<kColorCount;i++)SendMessageW(g_kwCombo,CB_ADDSTRING,0,(LPARAM)KColorName(i));
    SendMessageW(g_kwCombo,CB_SETCURSEL,0,0);
    int listTop=editY+editH+30,listBot=KW_H-KW_PADDING-KW_BTN_H-10;
    g_kwList=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|WS_VSCROLL,
        KW_PADDING,listTop,KW_W-KW_PADDING*2,listBot-listTop,g_kwWnd,nullptr,hInst,nullptr);
    RefreshList();
    // 폰트 적용
    SendMessageW(g_kwEditBox, WM_SETFONT, (WPARAM)hFont,     TRUE);
    SendMessageW(g_kwCombo,   WM_SETFONT, (WPARAM)hFont,     TRUE);
    SendMessageW(g_kwList,    WM_SETFONT, (WPARAM)hFontList, TRUE);
    SetLayeredWindowAttributes(g_kwWnd,0,0,LWA_ALPHA);
    ShowWindow(g_kwWnd,SW_SHOWNOACTIVATE);
    AnimateWindow(g_kwWnd,180,AW_BLEND);
    SetLayeredWindowAttributes(g_kwWnd,0,255,LWA_ALPHA);
}
