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
#include <mutex>
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"gdiplus.lib")
std::vector<KeywordEntry> g_keywordList;
extern std::mutex g_pdfiumMtx;   // PDFium은 스레드 안전하지 않음 → 백그라운드 렌더와 동시에 쓰지 않도록 잠금
static HFONT g_kwFont=nullptr, g_kwFontList=nullptr;   // 창을 닫을 때 해제
static WNDPROC g_kwOrigEditProc=nullptr;
static constexpr int KW_W=420,KW_H=490,KW_PADDING=20,KW_BTN_H=34,KW_BTN_W=110;
static HWND g_kwWnd=nullptr,g_kwEditBox=nullptr,g_kwCombo=nullptr,g_kwList=nullptr;
static bool g_kwDone=false,g_kwAddHover=false,g_kwDelHover=false,g_kwCloseHover=false,g_kwAddPressed=false;

#include "Strings.h"
#include "PopupAnim.h"

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

static void KwAddFromEdit(HWND hWnd);
static void KwDeleteSelected(HWND hWnd);
static void ApplyKeywordToDoc(const KeywordEntry& entry);
static void RemoveKeywordFromDoc(const KeywordEntry& entry);

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
        RECT ra2=GetAddBtnRect(),rd2=GetDelBtnRect(),rc2=GetCloseBtnRect();
        // 목록·삭제 버튼 밖을 누르면 목록 선택 해제
        // (삭제 버튼은 선택된 항목을 지워야 하므로 선택을 유지)
        if(g_kwList&&!PtInRect(&rd2,pt)){
            RECT lr; GetWindowRect(g_kwList,&lr); MapWindowPoints(nullptr,hWnd,(LPPOINT)&lr,2);
            if(!PtInRect(&lr,pt)){
                SendMessageW(g_kwList,LB_SETCURSEL,(WPARAM)-1,0);
                SetFocus(g_kwEditBox); InvalidateRect(g_kwList,nullptr,TRUE); UpdateWindow(g_kwList);
            }
        }
        if(PtInRect(&ra2,pt))      KwAddFromEdit(hWnd);
        else if(PtInRect(&rd2,pt)) KwDeleteSelected(hWnd);
        else if(PtInRect(&rc2,pt)) PostMessageW(hWnd,WM_CLOSE,0,0);
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
        PopupAnim::Hide(hWnd);
        DestroyWindow(hWnd); return 0;
    case WM_DESTROY:
    {
        HWND hPar = GetParent(hWnd);
        g_kwWnd = nullptr;
        g_kwEditBox = g_kwCombo = g_kwList = nullptr;
        if (g_kwFont)     { DeleteObject(g_kwFont);     g_kwFont = nullptr; }
        if (g_kwFontList) { DeleteObject(g_kwFontList); g_kwFontList = nullptr; }
        if (hPar) { SetForegroundWindow(hPar); SetFocus(hPar); }
        return 0;
    }
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}
// 페이지 전체 텍스트를 소문자로 (FPDFText_GetText 의 인덱스 = 글자 인덱스)
static std::wstring KwPageTextLower(FPDF_TEXTPAGE tp){
    const int total=FPDFText_CountChars(tp);
    if(total<=0) return {};
    std::wstring s(total+1,L'\0');
    FPDFText_GetText(tp,0,total,reinterpret_cast<unsigned short*>(&s[0]));
    s.resize(total);
    std::transform(s.begin(),s.end(),s.begin(),::towlower);
    return s;
}
static std::wstring KwLower(std::wstring s){
    std::transform(s.begin(),s.end(),s.begin(),::towlower);
    return s;
}

// 현재 문서에서 키워드를 찾아 일반 하이라이트로 추가 (이미 같은 자리·같은 색이면 건너뜀)
static void ApplyKeywordToDoc(const KeywordEntry& entry){
    if(!g_pdfDoc||entry.keyword.empty())return;
    const std::wstring lk=KwLower(entry.keyword);
    std::lock_guard<std::mutex> lk_(g_pdfiumMtx);
    for(int pg=0;pg<g_pageCount;pg++){
        FPDF_PAGE page=FPDF_LoadPage(g_pdfDoc,pg);if(!page)continue;
        FPDF_TEXTPAGE tp=FPDFText_LoadPage(page);
        if(tp){
            const std::wstring lp=KwPageTextLower(tp);
            size_t pos=0;
            while((pos=lp.find(lk,pos))!=std::wstring::npos){
                const int cs=(int)pos, ce=(int)(pos+lk.size()-1);
                bool exists=false;
                for(const auto& h:g_highlights)
                    if(h.pageIndex==pg&&h.charStart==cs&&h.charEnd==ce&&h.color==entry.color){exists=true;break;}
                if(!exists){
                    HighlightItem hl{};hl.pageIndex=pg;hl.charStart=cs;hl.charEnd=ce;hl.color=entry.color;
                    g_highlights.push_back(hl);g_dirty=true;
                }
                pos+=lk.size();
            }
            FPDFText_ClosePage(tp);
        }
        FPDF_ClosePage(page);
    }
}

// 현재 문서에서 이 키워드로 만든 하이라이트(같은 글자 + 같은 색)를 지움
// (따로 기록해 두지 않고 내용으로 찾으므로 탭을 바꾸거나 문서를 다시 열어도 정확)
static void RemoveKeywordFromDoc(const KeywordEntry& entry){
    if(!g_pdfDoc||entry.keyword.empty()||g_highlights.empty())return;
    const std::wstring lk=KwLower(entry.keyword);
    std::lock_guard<std::mutex> lk_(g_pdfiumMtx);
    std::vector<std::wstring> pageText(g_pageCount);
    std::vector<bool> loaded(g_pageCount,false);
    auto textOf=[&](int pg)->const std::wstring&{
        if(!loaded[pg]){
            loaded[pg]=true;
            if(FPDF_PAGE page=FPDF_LoadPage(g_pdfDoc,pg)){
                if(FPDF_TEXTPAGE tp=FPDFText_LoadPage(page)){pageText[pg]=KwPageTextLower(tp);FPDFText_ClosePage(tp);}
                FPDF_ClosePage(page);
            }
        }
        return pageText[pg];
    };
    const size_t before=g_highlights.size();
    g_highlights.erase(std::remove_if(g_highlights.begin(),g_highlights.end(),
        [&](const HighlightItem& h){
            if(h.color!=entry.color||h.pageIndex<0||h.pageIndex>=g_pageCount)return false;
            if(h.charEnd-h.charStart+1!=(int)lk.size())return false;
            const std::wstring& s=textOf(h.pageIndex);
            return h.charStart>=0&&h.charEnd<(int)s.size()&&s.compare(h.charStart,lk.size(),lk)==0;
        }),g_highlights.end());
    if(g_highlights.size()!=before)g_dirty=true;
}

// 목록의 모든 키워드를 현재 문서에 칠함
void ApplyKeywordHighlights(HWND hWnd){
    if(!g_pdfDoc)return;
    for(const auto& entry:g_keywordList)ApplyKeywordToDoc(entry);
    InvalidateRect(hWnd,nullptr,TRUE);
}

// 목록의 모든 키워드로 만든 하이라이트를 현재 문서에서 지움
void RemoveKeywordHighlights(){
    for(const auto& entry:g_keywordList)RemoveKeywordFromDoc(entry);
}

static void KwAddFromEdit(HWND hWnd){
    wchar_t buf[256]={};GetWindowTextW(g_kwEditBox,buf,256);
    std::wstring kw(buf);
    while(!kw.empty()&&iswspace(kw.front()))kw.erase(kw.begin());
    while(!kw.empty()&&iswspace(kw.back()))kw.pop_back();
    if(!kw.empty()){
        int sel=(int)SendMessageW(g_kwCombo,CB_GETCURSEL,0,0);if(sel<0)sel=0;
        bool dup=false;
        for(auto& e:g_keywordList)if(_wcsicmp(e.keyword.c_str(),kw.c_str())==0){dup=true;break;}
        if(!dup){
            KeywordEntry entry{kw,kColors[sel]};
            g_keywordList.push_back(entry);RefreshList();SetWindowTextW(g_kwEditBox,L"");
            HCURSOR oldCur=SetCursor(LoadCursor(nullptr,IDC_WAIT));   // 큰 문서는 잠깐 걸릴 수 있음
            ApplyKeywordToDoc(entry);
            SetCursor(oldCur);
            InvalidateRect(GetParent(hWnd),nullptr,TRUE);
        }
    }
    SetFocus(g_kwEditBox);
    InvalidateBtns(hWnd);
}

static void KwDeleteSelected(HWND hWnd){
    int sel=g_kwList?(int)SendMessageW(g_kwList,LB_GETCURSEL,0,0):-1;
    if(sel>=0&&sel<(int)g_keywordList.size()){
        const KeywordEntry entry=g_keywordList[sel];
        g_keywordList.erase(g_keywordList.begin()+sel);RefreshList();
        RemoveKeywordFromDoc(entry);
        InvalidateRect(GetParent(hWnd),nullptr,TRUE);
    }
    InvalidateBtns(hWnd);
}

// 입력 상자: Enter = 추가, Esc = 닫기
static LRESULT CALLBACK KwEditProc(HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam){
    if(msg==WM_KEYDOWN){
        if(wParam==VK_RETURN){KwAddFromEdit(GetParent(hWnd));return 0;}
        if(wParam==VK_ESCAPE){PostMessageW(GetParent(hWnd),WM_CLOSE,0,0);return 0;}
    }
    if(msg==WM_CHAR&&(wParam==VK_RETURN||wParam==VK_ESCAPE))return 0;   // 경고음 방지
    return CallWindowProcW(g_kwOrigEditProc,hWnd,msg,wParam,lParam);
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
    HFONT hFont = g_kwFont = CreateFontW(21, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontList = g_kwFontList = CreateFontW(21, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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
    g_kwOrigEditProc=(WNDPROC)SetWindowLongPtrW(g_kwEditBox,GWLP_WNDPROC,(LONG_PTR)KwEditProc);
    // 아래에서 살짝 떠오르며 나타남 (PopupAnim.h). 바로 입력할 수 있게 활성화 + 입력 상자 포커스
    PopupAnim::Show(g_kwWnd, SW_SHOW);
    SetForegroundWindow(g_kwWnd);
    SetFocus(g_kwEditBox);
}
