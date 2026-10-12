// WordFreqDialog.cpp
#include "framework.h"
#include "WordFreqDialog.h"
#include "PlainPDFViewer.h"
#include <fpdfview.h>
#include <fpdf_text.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cwctype>
#include "Lang.h"
#include "Strings.h"
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"gdiplus.lib")

static constexpr int WF_W=480,WF_H=596,WF_PAD=20,WF_BTN_H=34,WF_BTN_W=110,WF_EDIT_H=26;

static HWND g_wfWnd=nullptr,g_wfFromEdit=nullptr,g_wfToEdit=nullptr,g_wfTopNEdit=nullptr,g_wfList=nullptr,g_wfStopCheck=nullptr;
static bool g_wfAnalyzeHover=false,g_wfCloseHover=false;

struct WordFreqEntry { std::wstring word; int count; };
static std::vector<WordFreqEntry> g_wfResults;

static RECT GetAnalyzeBtnRect(){int y=WF_H-WF_PAD-WF_BTN_H;return{WF_W-WF_PAD-WF_BTN_W*2-8,y,WF_W-WF_PAD-WF_BTN_W-8,y+WF_BTN_H};}
static RECT GetCloseBtnRect(){int y=WF_H-WF_PAD-WF_BTN_H;return{WF_W-WF_PAD-WF_BTN_W,y,WF_W-WF_PAD,y+WF_BTN_H};}

static Gdiplus::GraphicsPath* WfRoundRect(float x,float y,float w,float h,float r){
    auto*p=new Gdiplus::GraphicsPath();
    p->AddArc(x,y,r*2,r*2,180,90);p->AddArc(x+w-r*2,y,r*2,r*2,270,90);
    p->AddArc(x+w-r*2,y+h-r*2,r*2,r*2,0,90);p->AddArc(x,y+h-r*2,r*2,r*2,90,90);
    p->CloseFigure();return p;
}

static void DrawWfButton(Gdiplus::Graphics&gfx,const RECT&rc,const wchar_t*text,bool hover,bool primary){
    float x=(float)rc.left,y=(float)rc.top,w=(float)(rc.right-rc.left),h=(float)(rc.bottom-rc.top);
    auto*path=WfRoundRect(x+.5f,y+.5f,w-1.f,h-1.f,4.f);
    if(primary){
        Gdiplus::Color top=hover?Gdiplus::Color(255,40,160,255):Gdiplus::Color(255,0,140,240);
        Gdiplus::Color bot=hover?Gdiplus::Color(255,0,110,200):Gdiplus::Color(255,0,80,180);
        Gdiplus::LinearGradientBrush br(Gdiplus::PointF(x,y),Gdiplus::PointF(x,y+h),top,bot);
        gfx.FillPath(&br,path);
    }else{
        Gdiplus::Color top=hover?Gdiplus::Color(255,252,252,252):Gdiplus::Color(255,255,255,255);
        Gdiplus::Color bot=hover?Gdiplus::Color(255,230,230,230):Gdiplus::Color(255,242,242,242);
        Gdiplus::LinearGradientBrush br(Gdiplus::PointF(x,y),Gdiplus::PointF(x,y+h),top,bot);
        gfx.FillPath(&br,path);
        Gdiplus::Pen bp(Gdiplus::Color(255,200,200,200),1.f);gfx.DrawPath(&bp,path);
    }
    delete path;
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff,9.f,Gdiplus::FontStyleBold,Gdiplus::UnitPoint);
    Gdiplus::SolidBrush tb(primary?Gdiplus::Color(255,255,255,255):Gdiplus::Color(255,45,45,45));
    Gdiplus::StringFormat sf;sf.SetAlignment(Gdiplus::StringAlignmentCenter);sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    gfx.DrawString(text,-1,&font,Gdiplus::RectF(x,y,w,h),&sf,&tb);
}

static void DrawWfDialog(HWND hWnd){
    PAINTSTRUCT ps;HDC hdc=BeginPaint(hWnd,&ps);
    RECT cr;GetClientRect(hWnd,&cr);
    HDC memDC=CreateCompatibleDC(hdc);
    HBITMAP bm=CreateCompatibleBitmap(hdc,cr.right,cr.bottom);
    HBITMAP old=(HBITMAP)SelectObject(memDC,bm);
    FillRect(memDC,&cr,(HBRUSH)GetStockObject(WHITE_BRUSH));
    Gdiplus::Graphics gfx(memDC);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    float W=(float)cr.right,H=(float)cr.bottom;
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::SolidBrush bgBr(Gdiplus::Color(255,255,255,255));
    gfx.FillRectangle(&bgBr,0.f,0.f,W,H);
    Gdiplus::Pen borderPen(Gdiplus::Color(255,218,218,218),1.f);
    gfx.DrawRectangle(&borderPen,0.5f,0.5f,W-1.f,H-1.f);
    // title
    {Gdiplus::Font font(&ff,11,Gdiplus::FontStyleBold,Gdiplus::UnitPoint);
    Gdiplus::SolidBrush br(Gdiplus::Color(255,20,20,20));
    gfx.DrawString(LS(STR_WF_TITLE),-1,&font,Gdiplus::RectF((float)WF_PAD,18.f,W-(float)WF_PAD*2,28.f),nullptr,&br);}
    // sep
    {Gdiplus::Pen sep(Gdiplus::Color(255,218,218,218),1.f);gfx.DrawLine(&sep,(float)WF_PAD,52.f,W-(float)WF_PAD,52.f);}
    // labels
    auto Label=[&](const wchar_t*text,float lx,float ly,float lw,float lh){
        Gdiplus::Font font(&ff,9.f,Gdiplus::FontStyleRegular,Gdiplus::UnitPoint);
        Gdiplus::SolidBrush br(Gdiplus::Color(255,80,80,80));
        Gdiplus::StringFormat sf;sf.SetAlignment(Gdiplus::StringAlignmentNear);
        gfx.DrawString(text,-1,&font,Gdiplus::RectF(lx,ly,lw,lh),&sf,&br);
    };
    Label(LS(STR_WF_PAGE_RANGE),(float)WF_PAD,62.f,120.f,18.f);
    Label(LS(STR_WF_FROM),(float)WF_PAD,84.f,50.f,18.f);
    Label(LS(STR_WF_TO),(float)WF_PAD+130,84.f,50.f,18.f);
    Label(LS(STR_WF_TOP_N),(float)WF_PAD+280,62.f,130.f,18.f);
    int listLY=62+18+WF_EDIT_H+14+20;
    Label(LS(STR_WF_RESULTS),(float)WF_PAD,(float)listLY,200.f,18.f);
    // buttons
    RECT ab=GetAnalyzeBtnRect(),cb2=GetCloseBtnRect();
    DrawWfButton(gfx,ab,LS(STR_WF_ANALYZE),g_wfAnalyzeHover,true);
    DrawWfButton(gfx,cb2,LS(STR_WF_CLOSE),g_wfCloseHover,false);
    BitBlt(hdc,
        ps.rcPaint.left, ps.rcPaint.top,
        ps.rcPaint.right  - ps.rcPaint.left,
        ps.rcPaint.bottom - ps.rcPaint.top,
        memDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(memDC,old);DeleteObject(bm);DeleteDC(memDC);
    EndPaint(hWnd,&ps);
}

static bool IsWordChar(wchar_t c){
    return std::iswalpha(c)||(c>=L'\uAC00'&&c<=L'\uD7A3')||(c>=L'\u1100'&&c<=L'\u11FF');
}

// 한국어 토큰인지 판별 (한글 문자 포함 여부) — 불용어 필터에서도 사용
static bool HasHangul(const std::wstring& s){
    for(wchar_t c:s)if(c>=L'\uAC00'&&c<=L'\uD7A3')return true;
    return false;
}

// ── 불용어 필터 ─────────────────────────────────────────────────────────────
static const wchar_t* s_stopEn[] = {
    // 관사
    L"a",L"an",L"the",
    // 전치사
    L"of",L"in",L"to",L"for",L"on",L"with",L"at",L"by",L"from",L"as",
    L"into",L"through",L"during",L"before",L"after",L"above",L"below",
    L"between",L"out",L"off",L"over",L"under",L"about",L"up",L"down",
    L"against",L"along",L"around",L"behind",L"beyond",L"near",L"since",
    L"toward",L"upon",L"within",L"without",
    // 접속사
    L"and",L"but",L"or",L"nor",L"so",L"yet",L"both",L"either",L"neither",
    L"although",L"because",L"since",L"while",L"if",L"unless",L"until",
    L"when",L"where",L"whether",L"which",L"who",L"whom",L"whose",L"that",
    // 조동사/be동사
    L"is",L"are",L"was",L"were",L"be",L"been",L"being",
    L"have",L"has",L"had",L"do",L"does",L"did",
    L"will",L"would",L"shall",L"should",L"may",L"might",
    L"must",L"can",L"could",L"need",L"dare",
    // 대명사
    L"i",L"you",L"he",L"she",L"it",L"we",L"they",
    L"me",L"him",L"her",L"us",L"them",
    L"my",L"your",L"his",L"its",L"our",L"their",
    L"this",L"that",L"these",L"those",
    L"what",L"each",L"all",L"any",L"some",L"no",L"not",
    // 부사/기타
    L"also",L"just",L"more",L"than",L"then",L"there",L"here",
    L"how",L"well",L"very",L"only",L"even",L"such",L"same",
    L"other",L"another",L"much",L"many",L"most",L"own",L"few",
    nullptr
};

static const wchar_t* s_stopKo[] = {
    // 조사
    L"\uc774",L"\uac00",L"\uc740",L"\ub294",L"\uc744",L"\ub97c",
    L"\uc758",L"\uc5d0",L"\ub85c",L"\uc640",L"\uacfc",L"\ub3c4",
    L"\ub9cc",L"\ub4e4",L"\uc11c",L"\uc694",L"\uc5d0\uc11c",
    L"\uc73c\ub85c",L"\uc774\ub77c",L"\uc5d0\uac8c",L"\ud55c\ud14c",
    L"\ubd80\ud130",L"\uae4c\uc9c0",L"\ubcf4\ub2e4",L"\ub9c8\ub2e4",
    // 어미/접속사
    L"\ud558\ub2e4",L"\ud558\uace0",L"\ud558\ub294",L"\ud558\uc9c0",
    L"\uc774\ub2e4",L"\uc774\uace0",L"\uc774\uba70",L"\uc774\ub098",
    L"\uadf8\ub9ac\uace0",L"\ud558\uc9c0\ub9cc",L"\uadf8\ub7ec\ub098",
    L"\ub610\ud55c",L"\uc989",L"\ub9cc\uc57d",L"\uc654\ub2e4",
    nullptr
};

static const wchar_t* s_stopJa[] = {
    // 조사
    L"\u306f",L"\u304c",L"\u3092",L"\u306b",L"\u3067",L"\u3068",
    L"\u304b\u3089",L"\u307e\u3067",L"\u3088\u308a",L"\u3078",
    L"\u306e",L"\u3082",L"\u3084",L"\u306b\u306f",L"\u3067\u306f",
    // 접속사/부사
    L"\u305d\u3057\u3066",L"\u3057\u304b\u3057",L"\u307e\u305f",
    L"\u3055\u3089\u306b",L"\u305f\u3060\u3057",L"\u306a\u304a",
    L"\u3064\u307e\u308a",L"\u3059\u306a\u308f\u3061",
    // be동사/조동사
    L"\u3067\u3059",L"\u307e\u3059",L"\u3067\u3042\u308b",
    L"\u3042\u308b",L"\u3044\u308b",L"\u306a\u308b",L"\u3059\u308b",
    nullptr
};

static bool IsStopWord(const std::wstring& w) {
    bool hasHangul = HasHangul(w);
    bool hasHiragana = false;
    for (wchar_t c : w)
        if (c >= L'\u3040' && c <= L'\u309F') { hasHiragana = true; break; }

    if (!hasHangul && !hasHiragana) {
        // 영어권 단어
        for (int i = 0; s_stopEn[i]; ++i)
            if (w == s_stopEn[i]) return true;
    } else if (hasHangul) {
        for (int i = 0; s_stopKo[i]; ++i)
            if (w == s_stopKo[i]) return true;
    } else {
        for (int i = 0; s_stopJa[i]; ++i)
            if (w == s_stopJa[i]) return true;
    }
    return false;
}

// 한국어 조사/어미/접사 후처리: 가능한 접미사를 길이 순으로 시도하여 제거
// 남은 어근이 2글자 이상일 때만 제거 적용
static std::wstring StripKoreanSuffix(const std::wstring& token){
    // 제거 대상 접미사 목록 (긴 것부터 순서대로)
    // 조사, 어미, 복수 접사 등
    static const wchar_t* kSuffixes[] = {
        // 3음절
        L"\uc774\ub77c\ub294",  // 이라는
        L"\uc774\ub77c\ub294",  // 이라는
        L"\uc5d0\uc11c\ub294",  // 에서는
        L"\uc73c\ub85c\ub294",  // 으로는
        L"\ub85c\ubd80\ud130",  // 로부터
        L"\uc73c\ub85c\uc368",  // 으로써
        L"\uc774\ub77c\uace0",  // 이라고
        L"\uc5d0\uac8c\ub294",  // 에게는
        L"\ub4e4\uc5d0\uac8c",  // 들에게
        L"\ub4e4\uc5d0\uc11c",  // 들에서
        // 2음절
        L"\uc774\ub77c",  // 이라
        L"\uc5d0\uc11c",  // 에서
        L"\uc73c\ub85c",  // 으로
        L"\uc5d0\uac8c",  // 에게
        L"\ud55c\ud14c",  // 한테
        L"\ub9cc\uc774",  // 만이
        L"\ubcf4\ub2e4",  // 보다
        L"\ub9cc\uc744",  // 만을
        L"\ub9cc\uc740",  // 만은
        L"\ub9cc\ub3c4",  // 만도
        L"\uc774\ub77c",  // 이라
        L"\uc774\uba70",  // 이며
        L"\uc774\uace0",  // 이고
        L"\uc774\ub098",  // 이나
        L"\uc774\ub77c",  // 이라
        L"\ub4e4\uc744",  // 들을
        L"\ub4e4\uc774",  // 들이
        L"\ub4e4\uc740",  // 들은
        L"\ub4e4\ub3c4",  // 들도
        L"\ub4e4\uc758",  // 들의
        L"\uc5d0\uc11c",  // 에서
        L"\uc5d0\ub294",  // 에는
        L"\uc5d0\ub3c4",  // 에도
        L"\uc758\ud55c",  // 의한
        // 1음절 조사
        L"\uc774",  // 이
        L"\uac00",  // 가
        L"\uc740",  // 은
        L"\ub294",  // 는
        L"\uc744",  // 을
        L"\ub97c",  // 를
        L"\uc758",  // 의
        L"\uc5d0",  // 에
        L"\ub85c",  // 로
        L"\uc640",  // 와
        L"\uacfc",  // 과
        L"\ub3c4",  // 도
        L"\ub9cc",  // 만
        L"\ub4e4",  // 들
        L"\uc11c",  // 서 (에서 단독)
        L"\uc694",  // 요
        L"\uc9c0",  // 지
        L"\ub2e4",  // 다
        L"\uace0",  // 고
        L"\ub098",  // 나
        L"\uba70",  // 며
    };
    for(const wchar_t* suf : kSuffixes){
        std::wstring s(suf);
        size_t slen = s.size();
        if(token.size() > slen + 1 && token.size() >= slen &&
           token.substr(token.size() - slen) == s){
            std::wstring stem = token.substr(0, token.size() - slen);
            if(stem.size() >= 2) return stem;
        }
    }
    return token;
}

static void RunAnalysis(){
    if(!g_pdfDoc||g_pageCount<=0){
        SendMessageW(g_wfList,LB_RESETCONTENT,0,0);
        SendMessageW(g_wfList,LB_ADDSTRING,0,(LPARAM)LS(STR_WF_NO_PDF));
        return;
    }
    wchar_t buf[64]={};
    GetWindowTextW(g_wfFromEdit,buf,64);int pageFrom=_wtoi(buf)-1;
    GetWindowTextW(g_wfToEdit,buf,64);int pageTo=_wtoi(buf)-1;
    GetWindowTextW(g_wfTopNEdit,buf,64);int topN=_wtoi(buf);
    pageFrom=std::max(0,std::min(pageFrom,g_pageCount-1));
    pageTo=std::max(pageFrom,std::min(pageTo,g_pageCount-1));
    if(topN<=0||topN>1000)topN=50;
    std::map<std::wstring,int> freq;
    constexpr int MIN_LEN=2;
    bool useStopFilter = g_wfStopCheck &&
                         (SendMessageW(g_wfStopCheck,BM_GETCHECK,0,0)==BST_CHECKED);
    for(int pi=pageFrom;pi<=pageTo;pi++){
        FPDF_PAGE page=FPDF_LoadPage(g_pdfDoc,pi);if(!page)continue;
        FPDF_TEXTPAGE tp=FPDFText_LoadPage(page);if(!tp){FPDF_ClosePage(page);continue;}
        int nChars=FPDFText_CountChars(tp);
        std::wstring text(nChars+1,L'\0');
        FPDFText_GetText(tp,0,nChars,(unsigned short*)text.data());
        text.resize(nChars);
        std::wstring token;
        for(wchar_t c:text){
            if(IsWordChar(c))token+=std::towlower(c);
            else{
                if((int)token.size()>=MIN_LEN){
                    std::wstring key = HasHangul(token) ? StripKoreanSuffix(token) : token;
                    if((int)key.size()>=MIN_LEN &&
                       (!useStopFilter || !IsStopWord(key))) freq[key]++;
                }
                token.clear();
            }
        }
        if((int)token.size()>=MIN_LEN){
            std::wstring key = HasHangul(token) ? StripKoreanSuffix(token) : token;
            if((int)key.size()>=MIN_LEN &&
               (!useStopFilter || !IsStopWord(key))) freq[key]++;
        }
        FPDFText_ClosePage(tp);FPDF_ClosePage(page);
    }
    g_wfResults.clear();
    for(auto&kv:freq)g_wfResults.push_back({kv.first,kv.second});
    std::sort(g_wfResults.begin(),g_wfResults.end(),[](const WordFreqEntry&a,const WordFreqEntry&b){
        return a.count!=b.count?a.count>b.count:a.word<b.word;});
    if((int)g_wfResults.size()>topN)g_wfResults.resize(topN);
    SendMessageW(g_wfList,LB_RESETCONTENT,0,0);
    if(g_wfResults.empty()){
        SendMessageW(g_wfList,LB_ADDSTRING,0,(LPARAM)LS(STR_WF_NO_WORDS));
        return;
    }
    int rank=1;
    for(auto&e:g_wfResults){
        wchar_t line[256];
        // 탭으로 열 구분: "순위\t단어\t빈도" - WM_DRAWITEM에서 각 열을 고정 위치에 그림
        swprintf_s(line,256,L"%d\t%s\t%d",rank++,e.word.c_str(),e.count);
        SendMessageW(g_wfList,LB_ADDSTRING,0,(LPARAM)line);
    }
}

static LRESULT CALLBACK WfWndProc(HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam){
    switch(msg){
    case WM_PAINT:DrawWfDialog(hWnd);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_MEASUREITEM:{
        MEASUREITEMSTRUCT*mis=(MEASUREITEMSTRUCT*)lParam;
        mis->itemHeight=26;return TRUE;}
    case WM_DRAWITEM:{
        DRAWITEMSTRUCT*dis=(DRAWITEMSTRUCT*)lParam;
        if(dis->hwndItem!=g_wfList||dis->itemID==(UINT)-1)break;
        // 배경 - 항상 흰색 (선택 강조 없음)
        HBRUSH bgBr=CreateSolidBrush(RGB(255,255,255));
        FillRect(dis->hDC,&dis->rcItem,bgBr);DeleteObject(bgBr);
        // 텍스트 읽기
        wchar_t buf[256]={};
        SendMessageW(g_wfList,LB_GETTEXT,dis->itemID,(LPARAM)buf);
        // 탭으로 분리: rank \t word \t count
        wchar_t*p1=buf,*p2=nullptr,*p3=nullptr;
        wchar_t*t1=wcschr(buf,L'\t');
        wchar_t rankStr[16]={},wordStr[128]={},cntStr[16]={};
        if(t1){*t1=L'\0';wcscpy_s(rankStr,buf);p2=t1+1;
            wchar_t*t2=wcschr(p2,L'\t');
            if(t2){*t2=L'\0';wcscpy_s(wordStr,p2);wcscpy_s(cntStr,t2+1);}}
        // 폰트 캐시 (한 번만 생성)
        static HFONT s_monoFont=nullptr;
        if(!s_monoFont)
            s_monoFont=CreateFontW(-MulDiv(11,GetDeviceCaps(dis->hDC,LOGPIXELSY),72),
                0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_MODERN,L"Consolas");
        HFONT oldF=(HFONT)SelectObject(dis->hDC,s_monoFont);
        SetBkMode(dis->hDC,TRANSPARENT);
        SetTextColor(dis->hDC,RGB(30,30,30));
        // 열 고정 위치 (픽셀): 순위=0~38, 단어=44~290, 빈도=294~끝
        RECT rc=dis->rcItem;
        // 순위: 오른쪽 정렬
        RECT rcRank={rc.left+2,rc.top,rc.left+38,rc.bottom};
        DrawTextW(dis->hDC,rankStr,-1,&rcRank,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        // 단어: 왼쪽 정렬
        RECT rcWord={rc.left+44,rc.top,rc.left+290,rc.bottom};
        DrawTextW(dis->hDC,wordStr,-1,&rcWord,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        // 빈도: 오른쪽 정렬
        RECT rcCnt={rc.left+294,rc.top,rc.right-4,rc.bottom};
        DrawTextW(dis->hDC,cntStr,-1,&rcCnt,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dis->hDC,oldF);
        return TRUE;}
    case WM_MOUSEMOVE:{
        POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        RECT ab=GetAnalyzeBtnRect(),cb2=GetCloseBtnRect();
        bool ah=PtInRect(&ab,pt)!=0,ch=PtInRect(&cb2,pt)!=0;
        if(ah!=g_wfAnalyzeHover||ch!=g_wfCloseHover){
            g_wfAnalyzeHover=ah;g_wfCloseHover=ch;
            InvalidateRect(hWnd,&ab,FALSE);InvalidateRect(hWnd,&cb2,FALSE);}
        TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,hWnd,0};TrackMouseEvent(&tme);
        return 0;}
    case WM_MOUSELEAVE:
        g_wfAnalyzeHover=g_wfCloseHover=false;
        {RECT ab=GetAnalyzeBtnRect(),cb2=GetCloseBtnRect();
        InvalidateRect(hWnd,&ab,FALSE);InvalidateRect(hWnd,&cb2,FALSE);}
        return 0;
    case WM_LBUTTONDOWN:{
        POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        RECT ab=GetAnalyzeBtnRect(),cb2=GetCloseBtnRect();
        if(PtInRect(&ab,pt)){RunAnalysis();return 0;}
        if(PtInRect(&cb2,pt)){DestroyWindow(hWnd);return 0;}
        return 0;}
    case WM_NCHITTEST:{
        POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        ScreenToClient(hWnd,&pt);
        // GDI+ 버튼 영역은 자식 윈도우가 없으므로 명시적으로 HTCLIENT 반환
        RECT ab=GetAnalyzeBtnRect(),cb2=GetCloseBtnRect();
        if(PtInRect(&ab,pt)||PtInRect(&cb2,pt))return HTCLIENT;
        HWND hit=ChildWindowFromPoint(hWnd,pt);
        if(!hit||hit==hWnd)return HTCAPTION;
        return HTCLIENT;}
    case WM_COMMAND:
        // 리스트박스 선택 즉시 해제 (선택 강조 표시 방지)
        if((HWND)lParam==g_wfList)
            SendMessageW(g_wfList,LB_SETCURSEL,(WPARAM)-1,0);
        break;
    case WM_KEYDOWN:
        if(wParam==VK_RETURN){RunAnalysis();return 0;}
        if(wParam==VK_ESCAPE){DestroyWindow(hWnd);return 0;}
        break;
    case WM_DESTROY:
    {
        HWND hPar = GetParent(hWnd);
        g_wfWnd = nullptr;
        if (hPar) { SetForegroundWindow(hPar); SetFocus(hPar); }
        return 0;
    }
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}

void ShowWordFreqDialog(HWND hParent){
    if(g_wfWnd){SetForegroundWindow(g_wfWnd);return;}
    static bool registered=false;
    if(!registered){
        WNDCLASSEXW wc={};
        wc.cbSize=sizeof(wc);wc.lpfnWndProc=WfWndProc;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH);
        wc.lpszClassName=L"WfDialog";
        RegisterClassExW(&wc);registered=true;
    }
    RECT pr;GetWindowRect(hParent,&pr);
    int cx=pr.left+(pr.right-pr.left)/2,cy=pr.top+(pr.bottom-pr.top)/2;
    g_wfWnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_LAYERED,L"WfDialog",LS(STR_WF_TITLE),
        WS_POPUP,cx-WF_W/2,cy-WF_H/2,WF_W,WF_H,
        hParent,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!g_wfWnd)return;
    DWORD cornerPref=2;
    DwmSetWindowAttribute(g_wfWnd,33,&cornerPref,sizeof(cornerPref));
    MARGINS margins={1,1,1,1};DwmExtendFrameIntoClientArea(g_wfWnd,&margins);
    HFONT hFont=CreateFontW(-MulDiv(10,GetDeviceCaps(GetDC(nullptr),LOGPIXELSY),72),
        0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
    int rowY=62+18+2;
    g_wfFromEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"1",
        WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_CENTER,WF_PAD,rowY,80,WF_EDIT_H,
        g_wfWnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(g_wfFromEdit,WM_SETFONT,(WPARAM)hFont,TRUE);
    HWND hTilde=CreateWindowExW(0,L"STATIC",L"~",WS_CHILD|WS_VISIBLE|SS_CENTER,
        WF_PAD+80+6,rowY+4,18,WF_EDIT_H-4,g_wfWnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(hTilde,WM_SETFONT,(WPARAM)hFont,TRUE);
    wchar_t defTo[16];swprintf_s(defTo,16,L"%d",std::max(1,g_pageCount));
    g_wfToEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",defTo,
        WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_CENTER,WF_PAD+80+6+18+6,rowY,80,WF_EDIT_H,
        g_wfWnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(g_wfToEdit,WM_SETFONT,(WPARAM)hFont,TRUE);
    g_wfTopNEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"50",
        WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_CENTER,WF_PAD+280,rowY,80,WF_EDIT_H,
        g_wfWnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(g_wfTopNEdit,WM_SETFONT,(WPARAM)hFont,TRUE);
    int listY=62+18+WF_EDIT_H+14+20+18+4;  // listLY(=62+18+EDIT_H+14+20) + 레이블높이(18) + 여백(4)
    // 체크박스: 리스트 아래, 버튼 위
    int chkY = WF_H - WF_PAD - WF_BTN_H - 8 - 28;
    int listH = chkY - listY - 6;
    g_wfList=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",nullptr,
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOSEL|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED,
        WF_PAD,listY,WF_W-WF_PAD*2,listH,g_wfWnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    HFONT hMono=CreateFontW(-MulDiv(9,GetDeviceCaps(GetDC(nullptr),LOGPIXELSY),72),
        0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_MODERN,L"Consolas");
    SendMessageW(g_wfList,WM_SETFONT,(WPARAM)(hMono?hMono:hFont),TRUE);
    g_wfStopCheck=CreateWindowExW(0,L"BUTTON",LS(STR_WF_STOPWORD),
        WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
        WF_PAD,chkY,WF_W-WF_PAD*2,22,
        g_wfWnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(g_wfStopCheck,WM_SETFONT,(WPARAM)hFont,TRUE);
    SendMessageW(g_wfStopCheck,BM_SETCHECK,BST_CHECKED,0);
    SetLayeredWindowAttributes(g_wfWnd,0,255,LWA_ALPHA);
    AnimateWindow(g_wfWnd,150,AW_BLEND);
}