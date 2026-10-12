// WordCloud.cpp  -  PDF 기반 워드 클라우드 (WordFreqDialog 동일 테마)
#define NOMINMAX
#include "WordCloud.h"
#include "Lang.h"
#include "Strings.h"
#include "CleanPopup.h"
#include "fpdf_text.h"
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"dwmapi.lib")
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>


// ── 크기 상수 ──────────────────────────────────────────────────
static const int WC_CLOUD_W = 820;
static const int WC_CLOUD_H = 440;
static const int WC_PAD     = 16;
static const int WC_CTL_H   = 24;
static const int WC_BTN_W   = 90;
static const int WC_BTN_H   = 30;
static const int WC_TOP_H   = 54;
static const int WC_BOT_H   = 128;
static const int WC_DLG_W   = WC_CLOUD_W + WC_PAD * 2;
static const int WC_DLG_H   = WC_TOP_H + WC_CLOUD_H + WC_BOT_H;

enum { WCM_SETVAL=WM_USER+101, WCM_GETVAL=WM_USER+102, WCM_SETRANGE=WM_USER+103 };
enum {
    WC_ID_CHK_STOPWORD  = 201,
    WC_ID_COMBO_PALETTE = 202,
    WC_ID_SLIDER_MAXW   = 203,
    WC_ID_SPIN_FROM     = 204,
    WC_ID_SPIN_TO       = 205,
    WC_ID_EDIT_FROM     = 206,
    WC_ID_EDIT_TO       = 207,
    WC_ID_CANVAS        = 210,
    WC_ID_LBL_STAT      = 211,
    WC_ID_CHK_ALLPAGES  = 212,
};

// ── 팔레트 ─────────────────────────────────────────────────────
struct WCPalette { const wchar_t* name; std::vector<DWORD> colors; };
static const WCPalette g_wcPalettes[] = {
    { L"Multi",  { 0xE63E6D,0x1E9E78,0x3B6FE0,0xF08A24,0x7C5CE0,0x14A5B8,0xD6409F,0x5DA832 } },
    { L"Blue",   { 0x0B2E6B,0x1A4FA0,0x2F73D0,0x4A93E6,0x2A9BC4,0x5B7FD6 } },
    { L"Warm",   { 0xB3261E,0xD9481F,0xF06A1E,0xE89A12,0xC2570C,0x8F3A12 } },
    { L"Green",  { 0x0B4A2A,0x147A45,0x1E9E5E,0x2DB87A,0x0F8F8A,0x5DA832 } },
    { L"Purple", { 0x3A2A8C,0x5A3FC0,0x7A56E0,0x9B6BE8,0xB04FC8,0x6C7BE0 } },
};

// ── 불용어 ─────────────────────────────────────────────────────
static const std::set<std::wstring> g_wcStopwords = {
    L"\uc774",L"\uadf8",L"\uc800",L"\uac83",L"\uc218",L"\ub4f1",L"\ubc0f",
    L"\ub97c",L"\uc744",L"\uc758",L"\uac00",L"\uc740",L"\ub294",L"\uc5d0",
    L"\uc5d0\uc11c",L"\ub85c",L"\uc73c\ub85c",L"\uc640",L"\uacfc",L"\ub3c4",
    L"\ub9cc",L"\ud558\ub2e4",L"\uc788\ub2e4",L"\uc5c6\ub2e4",L"\ub418\ub2e4",
    L"\uc54a\ub2e4",L"\uc774\ub2e4",L"\ud558\ub294",L"\uadf8\uac83",
    L"\uc774\uac83",L"\uc800\uac83",L"\ub354",L"\ub610",L"\ub2e4",L"\ud55c",
    L"\uc5b4",L"\uc5b4\uc11c",L"\uc5d0\uac8c",L"\uc758\ud574",L"\ubd80\ud130",
    L"\uae4c\uc9c0",L"\ub77c\uace0",L"\ub77c\ub294",L"\ud558\uace0",
    L"\uc774\uace0",L"\uc778",L"\ub41c",L"\ud560",L"\uc544",L"\uc57c",
    L"\uc5ec",L"\uc774\uc5ec",
    L"the",L"a",L"an",L"is",L"are",L"was",L"were",L"be",L"been",
    L"and",L"or",L"but",L"in",L"on",L"at",L"to",L"of",L"for",
    L"with",L"this",L"that",L"it",L"he",L"she",L"we",L"they",
    L"have",L"has",L"had",L"do",L"does",L"did",L"not",L"as",
    L"from",L"by",L"about",L"i",L"you",L"my",L"your",L"its",
    L"our",L"their",L"all",L"can",L"will",L"would",L"could",
    L"should",L"may",L"might",L"also",L"if",L"then",L"so",
    L"\u306e",L"\u306b",L"\u306f",L"\u3092",L"\u304c",L"\u3067",
    L"\u3068",L"\u3082",L"\u306a",L"\u304b\u3089",L"\u307e\u3067",
    L"\u3088\u308a",L"\u306a\u3069",L"\u3068\u3044\u3046",L"\u3068\u3057\u3066",
    L"\u3042\u308b",L"\u3044\u308b",L"\u3059\u308b",L"\u306a\u308b",
};

struct WCWordItem {
    std::wstring   text;
    int            freq;
    int            fontSize;
    DWORD          color;
    Gdiplus::RectF rect;      // AABB (배치 충돌용)
    bool           vertical;  // 90도 회전 여부
};
struct WCState {
    HWND hDlg=nullptr,hCanvas=nullptr,hChkStop=nullptr,hChkAll=nullptr;
    HWND hCombo=nullptr,hSlider=nullptr,hEditFrom=nullptr,hEditTo=nullptr;
    HWND hSpinFrom=nullptr,hSpinTo=nullptr,hLblStat=nullptr;
    FPDF_DOCUMENT    doc=nullptr;
    int              pageCount=0;
    Gdiplus::Bitmap* pBmp=nullptr;
    std::vector<WCWordItem> words;
    WNDPROC          origCanvas=nullptr;
    ULONG_PTR        gdipToken=0;
    bool*            pDone=nullptr;
    bool hoverGenerate=false,hoverSave=false,hoverClose=false;
};
static WCState* g_wc=nullptr;

static int g_wcW = WC_DLG_W, g_wcH = WC_DLG_H;   // 현재 클라이언트 크기
static RECT WC_GenBtnRect()  { int y=g_wcH-WC_PAD-WC_BTN_H; int x=g_wcW-WC_PAD-WC_BTN_W*2-8; return {x,y,x+WC_BTN_W,y+WC_BTN_H}; }
static RECT WC_SaveBtnRect() { int y=g_wcH-WC_PAD-WC_BTN_H; int x=g_wcW-WC_PAD-WC_BTN_W;    return {x,y,x+WC_BTN_W,y+WC_BTN_H}; }

static void WC_DrawButton(Gdiplus::Graphics& gfx,RECT r,const wchar_t* text,bool hover,bool primary)
{
    float x=(float)r.left,y=(float)r.top,w=(float)(r.right-r.left),h=(float)(r.bottom-r.top),rad=6.f;
    auto* path=new Gdiplus::GraphicsPath();
    path->AddArc(x,y,rad*2,rad*2,180.f,90.f);
    path->AddArc(x+w-rad*2,y,rad*2,rad*2,270.f,90.f);
    path->AddArc(x+w-rad*2,y+h-rad*2,rad*2,rad*2,0.f,90.f);
    path->AddArc(x,y+h-rad*2,rad*2,rad*2,90.f,90.f);
    path->CloseFigure();
    if(primary){
        Gdiplus::Color top=hover?Gdiplus::Color(255,40,160,255):Gdiplus::Color(255,0,140,240);
        Gdiplus::Color bot=hover?Gdiplus::Color(255,0,110,200):Gdiplus::Color(255,0,80,180);
        Gdiplus::LinearGradientBrush br(Gdiplus::PointF(x,y),Gdiplus::PointF(x,y+h),top,bot);
        gfx.FillPath(&br,path);
    } else {
        Gdiplus::Color top=hover?Gdiplus::Color(255,252,252,252):Gdiplus::Color(255,255,255,255);
        Gdiplus::Color bot=hover?Gdiplus::Color(255,230,230,230):Gdiplus::Color(255,242,242,242);
        Gdiplus::LinearGradientBrush br(Gdiplus::PointF(x,y),Gdiplus::PointF(x,y+h),top,bot);
        gfx.FillPath(&br,path);
        Gdiplus::Pen bp(Gdiplus::Color(255,200,200,200),1.f);
        gfx.DrawPath(&bp,path);
    }
    delete path;
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff,9.f,Gdiplus::FontStyleBold,Gdiplus::UnitPoint);
    Gdiplus::SolidBrush tb(primary?Gdiplus::Color(255,255,255,255):Gdiplus::Color(255,45,45,45));
    Gdiplus::StringFormat sf; sf.SetAlignment(Gdiplus::StringAlignmentCenter); sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    gfx.DrawString(text,-1,&font,Gdiplus::RectF(x,y,w,h),&sf,&tb);
}

static RECT WC_CloseBtnRect() { return { g_wcW-WC_PAD-20, 14, g_wcW-WC_PAD, 14+20 }; }

static void WC_DrawDialog(HWND hWnd)
{
    WCState* s=g_wc;
    PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps);
    RECT cr; GetClientRect(hWnd,&cr);
    HDC memDC=CreateCompatibleDC(hdc);
    HBITMAP bm=CreateCompatibleBitmap(hdc,cr.right,cr.bottom);
    HBITMAP old=(HBITMAP)SelectObject(memDC,bm);
    Gdiplus::Graphics gfx(memDC);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    float W=(float)cr.right,H=(float)cr.bottom;
    Gdiplus::SolidBrush bgBr(Gdiplus::Color(255,255,255,255));
    gfx.FillRectangle(&bgBr,0.f,0.f,W,H);
    Gdiplus::Pen borderPen(Gdiplus::Color(255,218,218,218),1.f);
    gfx.DrawRectangle(&borderPen,0.5f,0.5f,W-1.f,H-1.f);
    Gdiplus::FontFamily ff(L"Segoe UI");
    { Gdiplus::Font font(&ff,11,Gdiplus::FontStyleBold,Gdiplus::UnitPoint);
      Gdiplus::SolidBrush br(Gdiplus::Color(255,20,20,20));
      gfx.DrawString(LS(STR_WC_TITLE),-1,&font,Gdiplus::RectF((float)WC_PAD,14.f,W-(float)WC_PAD*2,28.f),nullptr,&br); }
    { Gdiplus::Pen sep(Gdiplus::Color(255,218,218,218),1.f);
      gfx.DrawLine(&sep,(float)WC_PAD,(float)(WC_TOP_H-4),W-(float)WC_PAD,(float)(WC_TOP_H-4)); }
    { int lineY=g_wcH-WC_PAD-WC_BTN_H-10;
      Gdiplus::Pen sep(Gdiplus::Color(255,230,230,230),1.f);
      gfx.DrawLine(&sep,(float)WC_PAD,(float)lineY,W-(float)WC_PAD,(float)lineY); }
    if(s){ WC_DrawButton(gfx,WC_GenBtnRect(), LS(STR_WC_GENERATE),s->hoverGenerate,true);
           WC_DrawButton(gfx,WC_SaveBtnRect(),LS(STR_WC_SAVE),    s->hoverSave,   false); }
    // 닫기 버튼 (X)
    { RECT cr2=WC_CloseBtnRect();
      float bx=(float)cr2.left,by=(float)cr2.top,bw=20.f,bh=20.f;
      bool hc=s&&s->hoverClose;
      Gdiplus::SolidBrush xbr(hc?Gdiplus::Color(255,220,50,50):Gdiplus::Color(255,180,80,80));
      Gdiplus::Pen xpen(Gdiplus::Color(255,255,255,255),hc?2.5f:2.f);
      gfx.FillEllipse(&xbr,bx,by,bw,bh);
      gfx.DrawLine(&xpen,bx+5.f,by+5.f,bx+bw-5.f,by+bh-5.f);
      gfx.DrawLine(&xpen,bx+bw-5.f,by+5.f,bx+5.f,by+bh-5.f); }
    BitBlt(hdc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,memDC,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);
    SelectObject(memDC,old); DeleteObject(bm); DeleteDC(memDC);
    EndPaint(hWnd,&ps);
}
static std::map<std::wstring,int> WC_CountWords(const std::wstring& text,bool filterStop)
{
    std::map<std::wstring,int> freq;
    std::wstring tok;
    for(wchar_t ch:text){
        bool isH=(ch>=0xAC00&&ch<=0xD7A3)||(ch>=0x3131&&ch<=0x3163);
        bool isL=(ch>=L'a'&&ch<=L'z')||(ch>=L'A'&&ch<=L'Z');
        bool isJ=(ch>=0x3040&&ch<=0x30FF)||(ch>=0x4E00&&ch<=0x9FFF);
        if(isH||isL||isJ){ tok+=isL?(wchar_t)towlower(ch):ch; }
        else{
            if(tok.size()>=2){
                if(!filterStop||g_wcStopwords.find(tok)==g_wcStopwords.end()) freq[tok]++;
            }
            tok.clear();
        }
    }
    if(tok.size()>=2){ if(!filterStop||g_wcStopwords.find(tok)==g_wcStopwords.end()) freq[tok]++; }
    return freq;
}

static void WC_PlaceWords(Gdiplus::Graphics& gfx,std::vector<WCWordItem>& items)
{
    std::vector<Gdiplus::RectF> placed;
    placed.reserve(items.size());
    const float cx=WC_CLOUD_W*0.5f, cy=WC_CLOUD_H*0.5f;
    Gdiplus::FontFamily ff(L"Malgun Gothic");

    for(auto& item:items){
        Gdiplus::Font font(&ff,(Gdiplus::REAL)item.fontSize,Gdiplus::FontStyleBold,Gdiplus::UnitPixel);
        Gdiplus::RectF bbox;
        gfx.MeasureString(item.text.c_str(),-1,&font,Gdiplus::PointF(0.f,0.f),&bbox);
        float tw=bbox.Width+1.f, th=bbox.Height-2.f;

        // 세로 배치 시 가로/세로 교환
        float pw = item.vertical ? th : tw;
        float ph = item.vertical ? tw : th;

        bool found=false;
        // 촘촘한 아르키메데스 나선: 각도 step 크게, radius 증가 작게
        float angle=0.f, radius=0.f;
        const float angleStep=0.35f, radiusStep=0.55f;

        while(radius < (float)(WC_CLOUD_W + WC_CLOUD_H)){
            float x = cx + 1.9f*radius*(float)cos(angle) - pw*0.5f;
            float y = cy + radius*(float)sin(angle) - ph*0.5f;
            if(x>=2.f && y>=2.f && x+pw<=WC_CLOUD_W-2.f && y+ph<=WC_CLOUD_H-2.f){
                Gdiplus::RectF cand(x,y,pw,ph);
                bool overlap=false;
                for(const auto& p:placed)
                    if(cand.IntersectsWith(p)){overlap=true;break;}
                if(!overlap){
                    item.rect=cand;
                    placed.push_back(cand);
                    found=true; break;
                }
            }
            angle+=angleStep;
            radius+=radiusStep*(angleStep/(float)(2*3.14159f));
        }
        if(!found) item.rect=Gdiplus::RectF(-9999.f,-9999.f,0.f,0.f);
    }
}

static void WC_RenderWords(Gdiplus::Graphics& gfx, std::vector<WCWordItem>& items)
{
    Gdiplus::FontFamily ff(L"Malgun Gothic");
    for(const auto& item:items){
        if(item.rect.X<-100.f) continue;
        Gdiplus::Font font(&ff,(Gdiplus::REAL)item.fontSize,item.fontSize>=22?Gdiplus::FontStyleBold:Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
        BYTE r2=(BYTE)((item.color>>16)&0xFF),g2=(BYTE)((item.color>>8)&0xFF),b2=(BYTE)(item.color&0xFF);
        // 작은 단어일수록 살짝 옅게 해서 크기 위계를 강조
        BYTE alpha=(BYTE)std::min(255,150+item.fontSize*2);
        Gdiplus::SolidBrush brush(Gdiplus::Color(alpha,r2,g2,b2));
        Gdiplus::SolidBrush shadow(Gdiplus::Color(item.fontSize>=30?36:0,0,0,0));

        if(item.vertical){
            // 90도 회전: 중심 기준으로 회전 변환
            float cx2=item.rect.X+item.rect.Width*0.5f;
            float cy2=item.rect.Y+item.rect.Height*0.5f;
            Gdiplus::Matrix mat;
            mat.RotateAt(-90.f, Gdiplus::PointF(cx2,cy2));
            gfx.SetTransform(&mat);
            // 세로 배치: rect는 (pw=th, ph=tw), 실제 텍스트 폭=tw, 높=th
            float ox=cx2-item.rect.Height*0.5f;
            float oy=cy2-item.rect.Width*0.5f;
            if(item.fontSize>=30) gfx.DrawString(item.text.c_str(),-1,&font,Gdiplus::PointF(ox+3.f,oy+2.f),&shadow);
            gfx.DrawString(item.text.c_str(),-1,&font,Gdiplus::PointF(ox+2.f,oy+1.f),&brush);
            gfx.ResetTransform();
        } else {
            if(item.fontSize>=30) gfx.DrawString(item.text.c_str(),-1,&font,
                Gdiplus::PointF(item.rect.X+3.f,item.rect.Y+2.f),&shadow);
            gfx.DrawString(item.text.c_str(),-1,&font,
                Gdiplus::PointF(item.rect.X+2.f,item.rect.Y+1.f),&brush);
        }
    }
}

static void WC_Build(WCState* s)
{
    if(!s->doc) return;
    bool allPages=(SendMessageW(s->hChkAll,BM_GETCHECK,0,0)==BST_CHECKED);
    int fromPage=1,toPage=s->pageCount;
    if(!allPages){
        wchar_t buf[16];
        fromPage=(int)SendMessageW(s->hEditFrom,WCM_GETVAL,0,0);
        toPage  =(int)SendMessageW(s->hEditTo,  WCM_GETVAL,0,0);
        fromPage=std::max(1,std::min(fromPage,s->pageCount));
        toPage  =std::max(fromPage,std::min(toPage,s->pageCount));
    }
    std::wstring fullText; fullText.reserve(65536);
    for(int pi=fromPage-1;pi<toPage;++pi){
        FPDF_PAGE page=FPDF_LoadPage(s->doc,pi); if(!page) continue;
        FPDF_TEXTPAGE tp=FPDFText_LoadPage(page);
        if(tp){
            int n=FPDFText_CountChars(tp);
            if(n>0){
                std::wstring buf2((size_t)n+1,L'\0');
                FPDFText_GetText(tp,0,n,reinterpret_cast<unsigned short*>(&buf2[0]));
                buf2.resize((size_t)n); fullText+=buf2; fullText+=L' ';
            }
            FPDFText_ClosePage(tp);
        }
        FPDF_ClosePage(page);
    }
    if(fullText.empty()){ CleanPopup::Show(s->hDlg,LS(STR_WC_TITLE),LS(STR_WC_NO_TEXT)); return; }
    bool filterStop=(SendMessageW(s->hChkStop,BM_GETCHECK,0,0)==BST_CHECKED);
    int  maxWords  =(int)SendMessageW(s->hSlider,WCM_GETVAL,0,0);
    int  palIdx    =(int)SendMessageW(s->hCombo, CB_GETCURSEL,0,0);
    if(palIdx<0) palIdx=0;
    const WCPalette& pal=g_wcPalettes[palIdx];
    auto freq=WC_CountWords(fullText,filterStop);
    if(freq.empty()){ CleanPopup::Show(s->hDlg,LS(STR_WC_TITLE),LS(STR_WC_NO_TEXT)); return; }
    std::vector<std::pair<int,std::wstring>> sorted; sorted.reserve(freq.size());
    for(const auto& kv:freq) sorted.push_back({kv.second,kv.first});
    std::sort(sorted.rbegin(),sorted.rend());
    if((int)sorted.size()>maxWords) sorted.resize((size_t)maxWords);
    int maxFreq=sorted.front().first,minFreq=sorted.back().first;
    s->words.clear();
    // srand로 세로/가로 결정을 매번 동일하게 (재현성)
    srand(42);
    for(int i=0;i<(int)sorted.size();++i){
        WCWordItem w; w.text=sorted[i].second; w.freq=sorted[i].first;
        // 비선형(제곱근) 스케일로 크기 차이를 더 극적으로
        float t=(maxFreq==minFreq)?0.5f:(float)(w.freq-minFreq)/(float)(maxFreq-minFreq);
        t = sqrtf(t);  // 비선형 스케일
        w.fontSize=(int)(11.f+t*65.f);
        w.color=pal.colors[(size_t)i%pal.colors.size()];
        // 상위 5개는 수평 고정, 이후는 30% 확률로 세로
        w.vertical = (i>=5) && ((rand()%10)<3);
        s->words.push_back(w);
    }
    delete s->pBmp;
    s->pBmp=new Gdiplus::Bitmap(WC_CLOUD_W,WC_CLOUD_H,PixelFormat32bppARGB);
    { Gdiplus::Graphics gfx(s->pBmp);
      gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
      gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      { Gdiplus::LinearGradientBrush bg(Gdiplus::PointF(0.f,0.f),Gdiplus::PointF(0.f,(float)WC_CLOUD_H),
            Gdiplus::Color(255,252,253,255),Gdiplus::Color(255,238,243,251));
        gfx.FillRectangle(&bg,0,0,WC_CLOUD_W,WC_CLOUD_H); }
      WC_PlaceWords(gfx,s->words);
      WC_RenderWords(gfx,s->words);
    }
    wchar_t statBuf[256];
    swprintf_s(statBuf,256,LS(STR_WC_STAT_FMT),(int)s->words.size(),
        s->words.empty()?L"":s->words[0].text.c_str(),
        s->words.empty()?0:s->words[0].freq);
    SetWindowTextW(s->hLblStat,statBuf);
    InvalidateRect(s->hCanvas,nullptr,TRUE);
}

static bool WC_GetEncoderClsid(const WCHAR* mime,CLSID* out)
{
    UINT num=0,sz=0; Gdiplus::GetImageEncodersSize(&num,&sz); if(!sz) return false;
    auto* info=(Gdiplus::ImageCodecInfo*)malloc(sz); if(!info) return false;
    Gdiplus::GetImageEncoders(num,sz,info); bool found=false;
    for(UINT i=0;i<num;++i) if(wcscmp(info[i].MimeType,mime)==0){*out=info[i].Clsid;found=true;break;}
    free(info); return found;
}
static void WC_Save(WCState* s)
{
    if(!s->pBmp) return;
    wchar_t path[MAX_PATH]=L"wordcloud.png";
    OPENFILENAMEW ofn{}; ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=s->hDlg;
    ofn.lpstrFilter=L"PNG\0*.png\0All\0*.*\0"; ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH;
    ofn.Flags=OFN_OVERWRITEPROMPT; ofn.lpstrDefExt=L"png";
    if(!GetSaveFileNameW(&ofn)) return;
    CLSID clsid; if(!WC_GetEncoderClsid(L"image/png",&clsid)) return;
    s->pBmp->Save(path,&clsid);
}
static LRESULT CALLBACK WC_CanvasProc(HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
    if(msg==WM_ERASEBKGND) return 1;
    if(msg==WM_PAINT){
        PAINTSTRUCT ps2; HDC hdcWin=BeginPaint(hWnd,&ps2);
        RECT rc; GetClientRect(hWnd,&rc);
        HDC hdc2=CreateCompatibleDC(hdcWin);
        HBITMAP hMemBm=CreateCompatibleBitmap(hdcWin,rc.right>0?rc.right:1,rc.bottom>0?rc.bottom:1);
        HBITMAP hMemOld=(HBITMAP)SelectObject(hdc2,hMemBm);
        { HBRUSH w=CreateSolidBrush(RGB(255,255,255)); FillRect(hdc2,&rc,w); DeleteObject(w); }
        HPEN   hPen =CreatePen(PS_SOLID,1,RGB(218,218,218));
        HBRUSH hBrsh=CreateSolidBrush(RGB(248,249,250));
        HPEN   hOP=(HPEN)  SelectObject(hdc2,hPen);
        HBRUSH hOB=(HBRUSH)SelectObject(hdc2,hBrsh);
        RoundRect(hdc2,rc.left,rc.top,rc.right,rc.bottom,8,8);
        SelectObject(hdc2,hOP); DeleteObject(hPen);
        SelectObject(hdc2,hOB); DeleteObject(hBrsh);
        if(g_wc&&g_wc->pBmp){
            Gdiplus::Graphics gfx2(hdc2);
            gfx2.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            int cw=rc.right-1,ch=rc.bottom-1;
            float sx=(float)cw/WC_CLOUD_W,sy=(float)ch/WC_CLOUD_H,sc=sx<sy?sx:sy;
            int dw=(int)(WC_CLOUD_W*sc),dh=(int)(WC_CLOUD_H*sc);
            gfx2.DrawImage(g_wc->pBmp,(cw-dw)/2+1,(ch-dh)/2+1,dw,dh);
        } else {
            SetTextColor(hdc2,RGB(160,160,170)); SetBkMode(hdc2,TRANSPARENT);
            HFONT hFont=CreateFontW(16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
            HFONT hOF=(HFONT)SelectObject(hdc2,hFont);
            DrawTextW(hdc2,LS(STR_WC_HINT),-1,&rc,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(hdc2,hOF); DeleteObject(hFont);
        }
        BitBlt(hdcWin,0,0,rc.right,rc.bottom,hdc2,0,0,SRCCOPY);
        SelectObject(hdc2,hMemOld); DeleteObject(hMemBm); DeleteDC(hdc2);
        EndPaint(hWnd,&ps2); return 0;
    }
    return (g_wc&&g_wc->origCanvas)?CallWindowProcW(g_wc->origCanvas,hWnd,msg,wParam,lParam):DefWindowProcW(hWnd,msg,wParam,lParam);
}

struct WCCtlData { int v=0,mn=0,mx=100,hot=0; bool drag=false,track=false; };

static void WC_RoundPath(Gdiplus::GraphicsPath& p,float x,float y,float w,float h,float r)
{
    p.AddArc(x,y,r*2,r*2,180.f,90.f); p.AddArc(x+w-r*2,y,r*2,r*2,270.f,90.f);
    p.AddArc(x+w-r*2,y+h-r*2,r*2,r*2,0.f,90.f); p.AddArc(x,y+h-r*2,r*2,r*2,90.f,90.f);
    p.CloseFigure();
}
static void WC_CtlTrack(HWND hWnd,WCCtlData* c)
{
    if(!c->track){ TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,hWnd,0}; TrackMouseEvent(&t); c->track=true; }
}
template<class F> static void WC_CtlPaint(HWND hWnd,F fn)
{
    PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps);
    RECT rc; GetClientRect(hWnd,&rc);
    HDC m=CreateCompatibleDC(hdc); HBITMAP bm=CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
    HBITMAP old=(HBITMAP)SelectObject(m,bm);
    { Gdiplus::Graphics g(m);
      g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
      g.Clear(Gdiplus::Color(255,255,255,255));
      fn(g,(float)rc.right,(float)rc.bottom); }
    BitBlt(hdc,0,0,rc.right,rc.bottom,m,0,0,SRCCOPY);
    SelectObject(m,old); DeleteObject(bm); DeleteDC(m);
    EndPaint(hWnd,&ps);
}
static LRESULT CALLBACK WC_StepperProc(HWND hWnd,UINT msg,WPARAM wp,LPARAM lp)
{
    WCCtlData* c=(WCCtlData*)GetWindowLongPtrW(hWnd,GWLP_USERDATA);
    auto setv=[&](int v){ v=std::max(c->mn,std::min(c->mx,v)); if(v!=c->v){ c->v=v; InvalidateRect(hWnd,nullptr,FALSE);} };
    auto hit=[&](int x){ RECT r; GetClientRect(hWnd,&r); int h=r.bottom; return x<h?1:(x>=r.right-h?2:0); };
    switch(msg){
    case WM_NCCREATE: SetWindowLongPtrW(hWnd,GWLP_USERDATA,(LONG_PTR)new WCCtlData()); break;
    case WM_NCDESTROY: delete c; SetWindowLongPtrW(hWnd,GWLP_USERDATA,0); break;
    case WM_ERASEBKGND: return 1;
    case WCM_SETRANGE: if(c){ c->mn=(int)wp; c->mx=(int)lp; setv(c->v); } return 0;
    case WCM_SETVAL: if(c) setv((int)wp); return 0;
    case WCM_GETVAL: return c?c->v:0;
    case WM_ENABLE: InvalidateRect(hWnd,nullptr,FALSE); return 0;
    case WM_MOUSEMOVE: if(c&&IsWindowEnabled(hWnd)){ WC_CtlTrack(hWnd,c); int h=hit(GET_X_LPARAM(lp)); if(h!=c->hot){ c->hot=h; InvalidateRect(hWnd,nullptr,FALSE);} } return 0;
    case WM_MOUSELEAVE: if(c){ c->track=false; c->hot=0; InvalidateRect(hWnd,nullptr,FALSE);} return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if(c&&IsWindowEnabled(hWnd)){ int h=hit(GET_X_LPARAM(lp)); if(h==1) setv(c->v-1); else if(h==2) setv(c->v+1); }
        return 0;
    case WM_MOUSEWHEEL:
        if(c&&IsWindowEnabled(hWnd)) setv(c->v+(GET_WHEEL_DELTA_WPARAM(wp)>0?1:-1));
        return 0;
    case WM_PAINT: {
        if(!c) break;
        bool en=IsWindowEnabled(hWnd)!=0;
        WC_CtlPaint(hWnd,[&](Gdiplus::Graphics& g,float w,float h){
            Gdiplus::GraphicsPath p; WC_RoundPath(p,0.5f,0.5f,w-1.f,h-1.f,5.f);
            Gdiplus::SolidBrush bg(en?Gdiplus::Color(255,255,255,255):Gdiplus::Color(255,246,246,246));
            g.FillPath(&bg,&p);
            if(en&&c->hot){ Gdiplus::SolidBrush hb(Gdiplus::Color(255,230,240,252));
                Gdiplus::GraphicsPath hp; float bw=h;
                if(c->hot==1) WC_RoundPath(hp,1.f,1.f,bw,h-2.f,4.f); else WC_RoundPath(hp,w-bw-1.f,1.f,bw,h-2.f,4.f);
                g.FillPath(&hb,&hp); }
            Gdiplus::Pen bp(en?Gdiplus::Color(255,200,200,205):Gdiplus::Color(255,226,226,226),1.f);
            g.DrawPath(&bp,&p);
            Gdiplus::Color sc=en?Gdiplus::Color(255,70,70,70):Gdiplus::Color(255,190,190,190);
            Gdiplus::Pen sp(sc,1.6f); float cy=h/2.f,r=4.f;
            g.DrawLine(&sp,h/2.f-r,cy,h/2.f+r,cy);
            float px=w-h/2.f; g.DrawLine(&sp,px-r,cy,px+r,cy); g.DrawLine(&sp,px,cy-r,px,cy+r);
            wchar_t t[16]; swprintf_s(t,16,L"%d",c->v);
            Gdiplus::FontFamily ff(L"Segoe UI"); Gdiplus::Font f(&ff,9.f,Gdiplus::FontStyleRegular,Gdiplus::UnitPoint);
            Gdiplus::SolidBrush tb(sc); Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter); sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(t,-1,&f,Gdiplus::RectF(h,0,w-h*2,h),&sf,&tb);
        });
        return 0; }
    }
    return DefWindowProcW(hWnd,msg,wp,lp);
}
static LRESULT CALLBACK WC_SliderProc(HWND hWnd,UINT msg,WPARAM wp,LPARAM lp)
{
    WCCtlData* c=(WCCtlData*)GetWindowLongPtrW(hWnd,GWLP_USERDATA);
    const float X0=9.f,TXT=36.f;
    auto trackW=[&](){ RECT r; GetClientRect(hWnd,&r); return (float)r.right-TXT-X0*2; };
    auto setv=[&](int v){ v=std::max(c->mn,std::min(c->mx,v)); if(v!=c->v){ c->v=v; InvalidateRect(hWnd,nullptr,FALSE);} };
    auto fromX=[&](int x){ float tw=trackW(); float t=tw>0?((float)x-X0)/tw:0.f; t=std::max(0.f,std::min(1.f,t)); setv(c->mn+(int)(t*(c->mx-c->mn)+0.5f)); };
    switch(msg){
    case WM_NCCREATE: SetWindowLongPtrW(hWnd,GWLP_USERDATA,(LONG_PTR)new WCCtlData()); break;
    case WM_NCDESTROY: delete c; SetWindowLongPtrW(hWnd,GWLP_USERDATA,0); break;
    case WM_ERASEBKGND: return 1;
    case WCM_SETRANGE: if(c){ c->mn=(int)wp; c->mx=(int)lp; setv(c->v); } return 0;
    case WCM_SETVAL: if(c) setv((int)wp); return 0;
    case WCM_GETVAL: return c?c->v:0;
    case WM_MOUSEMOVE: if(c){ WC_CtlTrack(hWnd,c); if(c->drag) fromX(GET_X_LPARAM(lp)); if(!c->hot){ c->hot=1; InvalidateRect(hWnd,nullptr,FALSE);} } return 0;
    case WM_MOUSELEAVE: if(c){ c->track=false; c->hot=0; InvalidateRect(hWnd,nullptr,FALSE);} return 0;
    case WM_LBUTTONDOWN: if(c){ c->drag=true; SetCapture(hWnd); fromX(GET_X_LPARAM(lp)); InvalidateRect(hWnd,nullptr,FALSE);} return 0;
    case WM_LBUTTONUP: if(c&&c->drag){ c->drag=false; ReleaseCapture(); InvalidateRect(hWnd,nullptr,FALSE);} return 0;
    case WM_CAPTURECHANGED: if(c&&c->drag){ c->drag=false; InvalidateRect(hWnd,nullptr,FALSE);} return 0;
    case WM_MOUSEWHEEL: if(c) setv(c->v+(GET_WHEEL_DELTA_WPARAM(wp)>0?5:-5)); return 0;
    case WM_PAINT: {
        if(!c) break;
        WC_CtlPaint(hWnd,[&](Gdiplus::Graphics& g,float w,float h){
            float tw=w-TXT-X0*2,cy=h/2.f;
            float t=(c->mx>c->mn)?(float)(c->v-c->mn)/(float)(c->mx-c->mn):0.f;
            Gdiplus::SolidBrush tr(Gdiplus::Color(255,224,226,230));
            Gdiplus::GraphicsPath tp; WC_RoundPath(tp,X0,cy-2.f,tw,4.f,2.f); g.FillPath(&tr,&tp);
            if(t>0.f){ Gdiplus::SolidBrush fb(Gdiplus::Color(255,0,120,215));
                Gdiplus::GraphicsPath fp; WC_RoundPath(fp,X0,cy-2.f,std::max(4.f,tw*t),4.f,2.f); g.FillPath(&fb,&fp); }
            float tx=X0+tw*t,rad=(c->drag||c->hot)?8.f:7.f;
            Gdiplus::SolidBrush sh(Gdiplus::Color(40,0,0,0)); g.FillEllipse(&sh,tx-rad,cy-rad+1.f,rad*2,rad*2);
            Gdiplus::SolidBrush th(Gdiplus::Color(255,255,255,255)); g.FillEllipse(&th,tx-rad,cy-rad,rad*2,rad*2);
            Gdiplus::Pen tpn(Gdiplus::Color(255,0,120,215),2.f); g.DrawEllipse(&tpn,tx-rad+1.f,cy-rad+1.f,rad*2-2.f,rad*2-2.f);
            wchar_t s[16]; swprintf_s(s,16,L"%d",c->v);
            Gdiplus::FontFamily ff(L"Segoe UI"); Gdiplus::Font f(&ff,9.f,Gdiplus::FontStyleRegular,Gdiplus::UnitPoint);
            Gdiplus::SolidBrush tb(Gdiplus::Color(255,70,70,70)); Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentFar); sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(s,-1,&f,Gdiplus::RectF(w-TXT,0,TXT,h),&sf,&tb);
        });
        return 0; }
    }
    return DefWindowProcW(hWnd,msg,wp,lp);
}

static void WC_UpdateRangeEnabled(WCState* s)
{
    bool all=(SendMessageW(s->hChkAll,BM_GETCHECK,0,0)==BST_CHECKED);
    EnableWindow(s->hEditFrom,!all);
    EnableWindow(s->hEditTo,  !all);
}

static LRESULT CALLBACK WC_DlgProc(HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
    WCState* s=g_wc;
    switch(msg){
    case WM_CREATE:{
        s=g_wc; if(!s) break;
        s->hDlg=hWnd;
        HFONT hFont=CreateFontW(-MulDiv(9,GetDeviceCaps(GetDC(nullptr),LOGPIXELSY),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        auto mkLabel=[&](const wchar_t* t,int x,int y,int w2,int h)->HWND{
            HWND hw=CreateWindowW(L"STATIC",t,WS_CHILD|WS_VISIBLE,x,y,w2,h,hWnd,nullptr,nullptr,nullptr);
            SendMessageW(hw,WM_SETFONT,(WPARAM)hFont,TRUE); return hw; };
        s->hCanvas=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,WC_PAD,WC_TOP_H,WC_CLOUD_W,WC_CLOUD_H,hWnd,(HMENU)(INT_PTR)WC_ID_CANVAS,nullptr,nullptr);
        s->origCanvas=(WNDPROC)SetWindowLongPtrW(s->hCanvas,GWLP_WNDPROC,(LONG_PTR)WC_CanvasProc);
        int row1Y=WC_TOP_H+WC_CLOUD_H+8,cx=WC_PAD;
        s->hChkAll=CreateWindowW(L"BUTTON",LS(STR_WC_ALL_PAGES),WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,cx,row1Y,100,WC_CTL_H,hWnd,(HMENU)(INT_PTR)WC_ID_CHK_ALLPAGES,nullptr,nullptr);
        SendMessageW(s->hChkAll,WM_SETFONT,(WPARAM)hFont,TRUE); SendMessageW(s->hChkAll,BM_SETCHECK,BST_CHECKED,0); cx+=108;
        mkLabel(LS(STR_WC_FROM),cx,row1Y+4,30,18); cx+=32;
        s->hEditFrom=CreateWindowW(L"PlainWCStepper",L"",WS_CHILD|WS_VISIBLE,cx,row1Y,84,WC_CTL_H,hWnd,(HMENU)(INT_PTR)WC_ID_EDIT_FROM,nullptr,nullptr);
        SendMessageW(s->hEditFrom,WCM_SETRANGE,1,s->pageCount); SendMessageW(s->hEditFrom,WCM_SETVAL,1,0); cx+=92;
        mkLabel(LS(STR_WC_TO),cx,row1Y+4,24,18); cx+=26;
        s->hEditTo=CreateWindowW(L"PlainWCStepper",L"",WS_CHILD|WS_VISIBLE,cx,row1Y,84,WC_CTL_H,hWnd,(HMENU)(INT_PTR)WC_ID_EDIT_TO,nullptr,nullptr);
        SendMessageW(s->hEditTo,WCM_SETRANGE,1,s->pageCount); SendMessageW(s->hEditTo,WCM_SETVAL,s->pageCount,0); cx+=92; cx+=14;
        mkLabel(LS(STR_WC_PALETTE),cx,row1Y+4,36,18); cx+=38;
        s->hCombo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,cx,row1Y,120,120,hWnd,(HMENU)(INT_PTR)WC_ID_COMBO_PALETTE,nullptr,nullptr);
        SendMessageW(s->hCombo,WM_SETFONT,(WPARAM)hFont,TRUE);
        for(const auto& p:g_wcPalettes) SendMessageW(s->hCombo,CB_ADDSTRING,0,(LPARAM)p.name);
        SendMessageW(s->hCombo,CB_SETCURSEL,0,0); cx+=128; cx+=14;
        mkLabel(LS(STR_WC_MAX_WORDS),cx,row1Y+4,60,18); cx+=62;
        s->hSlider=CreateWindowW(L"PlainWCSlider",L"",WS_CHILD|WS_VISIBLE,cx,row1Y,170,WC_CTL_H,hWnd,(HMENU)(INT_PTR)WC_ID_SLIDER_MAXW,nullptr,nullptr);
        SendMessageW(s->hSlider,WCM_SETRANGE,10,100); SendMessageW(s->hSlider,WCM_SETVAL,50,0);
        int row2Y=row1Y+WC_CTL_H+8;
        s->hChkStop=CreateWindowW(L"BUTTON",LS(STR_WC_STOPWORD),WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,WC_PAD,row2Y,220,WC_CTL_H,hWnd,(HMENU)(INT_PTR)WC_ID_CHK_STOPWORD,nullptr,nullptr);
        SendMessageW(s->hChkStop,WM_SETFONT,(WPARAM)hFont,TRUE); SendMessageW(s->hChkStop,BM_SETCHECK,BST_CHECKED,0);
        s->hLblStat=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFTNOWORDWRAP,WC_PAD+228,row2Y,380,WC_CTL_H,hWnd,(HMENU)(INT_PTR)WC_ID_LBL_STAT,nullptr,nullptr);
        SendMessageW(s->hLblStat,WM_SETFONT,(WPARAM)hFont,TRUE);
        WC_UpdateRangeEnabled(s);
        break; }
    case WM_NCCALCSIZE:
        if(wParam) return WVR_REDRAW;
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm=(MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x=640; mm->ptMinTrackSize.y=WC_TOP_H+220+WC_BOT_H;
        return 0; }
    case WM_SIZE: {
        if(wParam==SIZE_MINIMIZED) break;
        int nw=LOWORD(lParam), nh=HIWORD(lParam);
        if(nw<=0||nh<=0) break;
        int dy=nh-g_wcH;
        g_wcW=nw; g_wcH=nh;
        if(s&&s->hCanvas){
            if(dy!=0){
                for(HWND c=GetWindow(hWnd,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT)){
                    if(c==s->hCanvas) continue;
                    RECT r; GetWindowRect(c,&r); POINT p={r.left,r.top}; ScreenToClient(hWnd,&p);
                    SetWindowPos(c,nullptr,p.x,p.y+dy,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
                }
            }
            SetWindowPos(s->hCanvas,nullptr,WC_PAD,WC_TOP_H,nw-WC_PAD*2,nh-WC_TOP_H-WC_BOT_H,SWP_NOZORDER|SWP_NOACTIVATE);
        }
        RedrawWindow(hWnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
        break; }
    case WM_NCHITTEST: {
        POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        RECT wr; GetWindowRect(hWnd,&wr);
        const int B=6;
        bool L=pt.x<wr.left+B, R=pt.x>=wr.right-B, T=pt.y<wr.top+B, Bt=pt.y>=wr.bottom-B;
        if(T&&L) return HTTOPLEFT;   if(T&&R) return HTTOPRIGHT;
        if(Bt&&L) return HTBOTTOMLEFT; if(Bt&&R) return HTBOTTOMRIGHT;
        if(L) return HTLEFT; if(R) return HTRIGHT; if(T) return HTTOP; if(Bt) return HTBOTTOM;
        ScreenToClient(hWnd,&pt);
        RECT xr=WC_CloseBtnRect();
        if(PtInRect(&xr,pt)) return HTCLIENT;
        if(pt.y<WC_TOP_H) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_PAINT: WC_DrawDialog(hWnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
        SetBkMode((HDC)wParam,TRANSPARENT); SetTextColor((HDC)wParam,RGB(40,40,40));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_COMMAND:
        if(LOWORD(wParam)==WC_ID_CHK_ALLPAGES&&s) WC_UpdateRangeEnabled(s);
        break;
    case WM_LBUTTONDOWN:
        if(s){ POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            RECT gr=WC_GenBtnRect(),sr2=WC_SaveBtnRect(),xr=WC_CloseBtnRect();
            if(PtInRect(&xr, pt)){DestroyWindow(hWnd);return 0;}
            if(PtInRect(&gr, pt)) WC_Build(s);
            if(PtInRect(&sr2,pt)) WC_Save(s); }
        break;
    case WM_MOUSEMOVE:
        if(s){ POINT pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            RECT gr=WC_GenBtnRect(),sr2=WC_SaveBtnRect(),xr=WC_CloseBtnRect();
            bool hg=!!PtInRect(&gr,pt),hs=!!PtInRect(&sr2,pt),hc=!!PtInRect(&xr,pt);
            if(hg!=s->hoverGenerate){ s->hoverGenerate=hg; InvalidateRect(hWnd,&gr,FALSE); }
            if(hs!=s->hoverSave)    { s->hoverSave=hs;     InvalidateRect(hWnd,&sr2,FALSE); }
            if(hc!=s->hoverClose)   { s->hoverClose=hc;    InvalidateRect(hWnd,&xr,FALSE); } }
        break;
    case WM_MOUSELEAVE:
        if(s){ RECT gr=WC_GenBtnRect(),sr2=WC_SaveBtnRect(),xr=WC_CloseBtnRect();
            if(s->hoverGenerate){ s->hoverGenerate=false; InvalidateRect(hWnd,&gr,FALSE); }
            if(s->hoverSave)    { s->hoverSave=false;     InvalidateRect(hWnd,&sr2,FALSE); }
            if(s->hoverClose)   { s->hoverClose=false;    InvalidateRect(hWnd,&xr,FALSE); } }
        break;
    case WM_KEYDOWN:
        if(wParam==VK_ESCAPE){DestroyWindow(hWnd);return 0;} break;
    case WM_DESTROY:
        if(s){ delete s->pBmp; s->pBmp=nullptr;
            ULONG_PTR tok=s->gdipToken;
            if(s->pDone)*s->pDone=true;
            delete s; g_wc=nullptr;
            Gdiplus::GdiplusShutdown(tok); }
        {
            HWND hPar = GetParent(hWnd);
            if (hPar) { SetForegroundWindow(hPar); SetFocus(hPar); }
        }
        break;
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}
void ShowWordCloudDialog(HWND hParent,FPDF_DOCUMENT doc,int pageCount)
{
    if(!doc){ CleanPopup::Show(hParent,LS(STR_WC_TITLE),LS(STR_WC_NO_DOC)); return; }
    if(g_wc&&g_wc->hDlg&&IsWindow(g_wc->hDlg)){SetForegroundWindow(g_wc->hDlg);return;}
    WCState* s=new WCState(); s->doc=doc; s->pageCount=pageCount;
    Gdiplus::GdiplusStartupInput gsi{};
    Gdiplus::GdiplusStartup(&s->gdipToken,&gsi,nullptr);
    g_wc=s;
    static bool registered=false;
    if(!registered){
        INITCOMMONCONTROLSEX icex{sizeof(icex),ICC_BAR_CLASSES}; InitCommonControlsEx(&icex);
        WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.style=0;
        wc.lpfnWndProc=WC_DlgProc; wc.hInstance=GetModuleHandleW(nullptr);
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        wc.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH);
        wc.lpszClassName=L"PlainWCDlg"; wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);
        RegisterClassExW(&wc);
        WNDCLASSEXW cc{}; cc.cbSize=sizeof(cc); cc.hInstance=wc.hInstance; cc.hCursor=LoadCursor(nullptr,IDC_HAND);
        cc.lpfnWndProc=WC_StepperProc; cc.lpszClassName=L"PlainWCStepper"; RegisterClassExW(&cc);
        cc.hCursor=LoadCursor(nullptr,IDC_ARROW); cc.lpfnWndProc=WC_SliderProc; cc.lpszClassName=L"PlainWCSlider"; RegisterClassExW(&cc);
        registered=true;
    }
    RECT pr; GetWindowRect(hParent,&pr);
    int cx2=pr.left+(pr.right-pr.left)/2, cy2=pr.top+(pr.bottom-pr.top)/2;
    g_wcW=WC_DLG_W; g_wcH=WC_DLG_H;
    HWND hWnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_LAYERED,L"PlainWCDlg",LS(STR_WC_TITLE),
        WS_POPUP|WS_THICKFRAME|WS_CLIPCHILDREN,
        cx2-WC_DLG_W/2,cy2-WC_DLG_H/2,WC_DLG_W,WC_DLG_H,
        hParent,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!hWnd){ Gdiplus::GdiplusShutdown(s->gdipToken); delete s; g_wc=nullptr; return; }
    DWORD cornerPref=2; DwmSetWindowAttribute(hWnd,33,&cornerPref,sizeof(cornerPref));
    MARGINS margins={1,1,1,1}; DwmExtendFrameIntoClientArea(hWnd,&margins);
    SetLayeredWindowAttributes(hWnd,0,255,LWA_ALPHA);
    ShowWindow(hWnd,SW_SHOW);
    AnimateWindow(hWnd,150,AW_BLEND);
    UpdateWindow(hWnd);
    bool done=false; s->pDone=&done;
    MSG msgLoop{};
    while(!done&&GetMessageW(&msgLoop,nullptr,0,0)>0){
        if(!IsWindow(hWnd)||!IsDialogMessageW(hWnd,&msgLoop)){
            TranslateMessage(&msgLoop); DispatchMessageW(&msgLoop);
        }
    }
}