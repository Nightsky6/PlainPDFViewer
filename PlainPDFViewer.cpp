// PlainPDFViewer.cpp : PDFium 기반 PDF 뷰어

#include "framework.h"
#include "PlainPDFViewer.h"
#include "LicenseDlg.h"
#include "ToastDlg.h"
#include "Preferences.h"
#include "CleanPreferences.h"
#include "SignaturePad.h"
#include "RecentFiles.h"
#include "TextViewer.h"
#include "GotoDialog.h"
#include "ConfirmDialog.h"
#include "KeywordDialog.h"
#include "WordFreqDialog.h"
#include "WordCloud.h"
#include "PrintDialog.h"
#include "PdfCompress.h"
#include "ExportImage.h"
#include "Toolbar.h"
#include "FindBar.h"
#include "WindowsOps.h"
#include "Lang.h"
#include <fpdfview.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <algorithm>
#include <uxtheme.h>
#include <fpdf_text.h>
#include <fpdf_doc.h>
#include <fpdf_annot.h>
#include <fpdf_save.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_flatten.h>
#include <fpdf_formfill.h>
#include <fpdf_fwlevent.h>
#include <vector>
#include <unordered_map>
#include <set>
#include <windowsx.h>
#include "CleanPopup.h"
#include "DocPropertiesDialog.h"
#include "GroupPanel.h"
#include "PageManager.h"
#include <objidl.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <deque>


#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Version.lib")

#define MAX_LOADSTRING 100
#define ZOOM_STEP      1.25f
#define ZOOM_MIN       0.05f
#define ZOOM_MAX       8.0f
#define SCROLL_LINE    40

// 스크롤
static float g_scrollVelY = 0.0f;
float g_scrollYf = 0.0f;

static float g_scrollTargetY  = 0.0f;
static bool  g_thumbTracking  = false;

// 영구 더블 버퍼 (크기 변경 시에만 재생성)
static HDC     g_backDC  = nullptr;
static HBITMAP g_backBmp = nullptr;
static HBITMAP g_backOld = nullptr;
static int     g_backW   = 0;
static int     g_backH   = 0;
static bool    g_isSizing = false;

// 페이지 베이스 캐시 DC — 페이지 비트맵 + 고정 오버레이(선택/찾기 제외)
// 텍스트 선택 드래그 중 RenderPage 전체 재실행을 건너뛰기 위해 사용
static HDC     g_pageBaseDC    = nullptr;
static HBITMAP g_pageBaseBmp   = nullptr;
static HBITMAP g_pageBaseOld   = nullptr;
static int     g_pageBaseW     = 0;
static int     g_pageBaseH     = 0;
static bool    g_pageBaseDirty = true;

#define SMOOTH_TIMER_ID   1001
#define SMOOTH_INTERVAL   8        // ms (≈120fps)

#define SCROLL_FRICTION   0.92f
#define SCROLL_MIN_VEL    0.3f
#define SCROLL_MAX_VEL    400.0f

// 자동 스크롤
bool         g_autoScrolling = false;
POINT        g_contextMenuPt = {};
#define AUTO_SCROLL_TIMER_ID  1002
#define AUTO_SCROLL_INTERVAL  16
#define AUTO_SCROLL_SPEED     1.5f

// 슬라이드쇼
bool         g_slideshowActive    = false;
bool         g_ssArrowHoverLeft   = false;
bool         g_ssArrowHoverRight  = false;
bool         g_ssCloseHover       = false;

#define SLIDESHOW_TIMER_ID    1003
#define SLIDESHOW_INTERVAL_MS 3000

// 로딩 스피너 애니메이션
#define LOADING_ANIM_TIMER_ID 1004
#define LOADING_ANIM_INTERVAL 60

// 전체화면
bool  g_fullscreen       = false;
RECT  g_windowedRect     = {};    // 전체화면 진입 전 창 위치/크기
DWORD g_windowedStyle    = 0;     // 전체화면 진입 전 창 스타일
HMENU g_hMainMenu        = nullptr; // 전체화면 진입 전 메뉴 핸들

#define REGISTRY_KEY     L"Software\\PlainPDFViewer"
#define REGISTRY_RECENT  L"Recent"
#define REGISTRY_WND_X   L"WindowX"
#define REGISTRY_WND_Y   L"WindowY"
#define REGISTRY_WND_W   L"WindowW"
#define REGISTRY_WND_H   L"WindowH"
#define REGISTRY_WND_MAX L"WindowMaximized"

// 주석(하이라이트/밑줄/취소선/물결) 색상 팔레트
#define ANNOT_COLOR_YELLOW   RGB(255, 255,   0)   // 노란색
#define ANNOT_COLOR_GREEN    RGB(  0, 200, 150)   // 민트 그린
#define ANNOT_COLOR_BLUE     RGB( 41, 182, 246)   // 스카이 블루
#define ANNOT_COLOR_ORANGE   RGB(255, 138, 101)   // 코랄
#define ANNOT_COLOR_PINK     RGB(236,  72, 153)   // 로즈 핑크
#define ANNOT_COLOR_LAVENDER RGB(167, 139, 250)   // 라벤더
#define ANNOT_COLOR_EMERALD  RGB( 52, 211, 153)   // 에메랄드
#define ANNOT_COLOR_SLATE    RGB(148, 163, 184)   // 슬레이트

// 툴바/상태바 높이 상수는 Toolbar.h 참조

// 전역 변수
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

// PDF 상태
FPDF_DOCUMENT g_pdfDoc = nullptr;
FPDF_PAGE     g_pdfPage = nullptr;
FPDF_FORMHANDLE g_formHandle = nullptr; // PDF 폼 핸들
bool            g_formFieldFocused = false; // 폼 필드 포커스 상태

// PDFium 폼 타이머: PDFium이 요청한 타이머를 Win32 타이머로 구현
struct FormTimerEntry { UINT_PTR winTimerID; TimerCallback callback; };
static std::unordered_map<int, FormTimerEntry> g_formTimers; // key = PDFium timer ID
static int  g_formTimerNextId  = 1;     // PDFium에 반환할 ID (1부터 증가)
static HWND g_formTimerHwnd    = nullptr;
static constexpr UINT FORM_TIMER_BASE = 2000; // Win32 타이머 ID 오프셋 (기존과 충돌 방지)
int                  g_currentPage = 0;
int                  g_pageCount = 0;
float                g_zoom = 1.0f;
int                  g_rotation = 0;   // 0=0°, 1=90°CW, 2=180°, 3=270°CW
bool                 g_darkMode = false;    // 야간 모드 (페이지 색상 반전)
bool                 g_screenLock = false;  // 화면 잠금
bool                 g_spotlightMode = false; // 스포트라이트 모드
bool                 g_isDrawingSpotlight = false; // 드래그 중
bool                 g_hasSpotlight = false;  // 확정된 영역 존재
POINT                g_spotlightStart = {};   // 드래그 시작점
RECT                 g_spotlightRect  = {};   // 최종 사각형 (화면 좌표)
bool                 g_laserMode = false;     // 레이저 포인터 모드
POINT                g_laserPos = { -1, -1 }; // 현재 레이저 위치
int           g_scrollX = 0;
int           g_scrollY = 0;
std::wstring         g_filePath;

// ─── 텍스트 하이라이트 ────────────────────────────────────────────────────────
// 페이지별 추가 회전(0~3). 전체 회전(g_rotation)에 더해져 적용된다.
static std::vector<int> g_pageRot;
int GetEffectiveRotation(int pageIndex)
{
    int r = (pageIndex >= 0 && pageIndex < (int)g_pageRot.size()) ? g_pageRot[pageIndex] : 0;
    return (g_rotation + r) % 4;
}

std::vector<HighlightItem> g_highlights;
static int g_selectedHighlight = -1;  // 선택된 하이라이트 인덱스 (-1 = 없음)

// ─── 텍스트 밑줄 ──────────────────────────────────────────────────────────────
struct UnderlineItem {
    int      pageIndex;
    int      charStart;
    int      charEnd;
    COLORREF color;
    std::vector<PdfRectCache> cachedRects;  // 병합된 PDF 좌표 rect 캐시
};
static std::vector<UnderlineItem> g_underlines;
static int g_selectedUnderline = -1;  // 선택된 밑줄 인덱스 (-1 = 없음)

// ─── 텍스트 취소선 ──────────────────────────────────────────────────────────
struct StrikeoutItem {
    int      pageIndex  = 0;
    int      charStart  = 0;
    int      charEnd    = 0;
    COLORREF color      = 0;
    std::vector<PdfRectCache> cachedRects;
};
static std::vector<StrikeoutItem> g_strikeouts;
static int g_selectedStrikeout = -1;

// ─── 텍스트 물결 밑줄 ──────────────────────────────────────────────────────
struct SquigglyItem {
    int      pageIndex = 0;
    int      charStart = 0;
    int      charEnd = 0;
    COLORREF color = 0;
    std::vector<PdfRectCache> cachedRects;
};
static std::vector<SquigglyItem> g_squigglies;
static int g_selectedSquiggly = -1;

// ─── 텍스트 메모(Note) ──────────────────────────────────────────────────────
struct NoteItem {
    int          pageIndex = 0;
    double       pdfX = 0.0;   // PDF 좌표
    double       pdfY = 0.0;
    std::wstring text;
};
static std::vector<NoteItem> g_notes;
static int g_selectedNote = -1;
static int g_hoverNote = -1;
static HWND g_hNoteTooltip = nullptr;
static HWND g_hToolbarTooltip = nullptr;
static HWND g_hTabTooltip = nullptr;   // 탭 파일명 툴팁
static int  g_tabTooltipIdx = -1;      // 현재 툴팁이 표시된 탭 인덱스
static std::wstring g_tabTipText;      // 탭 툴팁 텍스트
static std::wstring g_toolbarTipText;   // 현재 표시할 툴팁 텍스트
static int  g_toolbarPressedBtn  = -1;  // WM_LBUTTONDOWN에서 눌린 툴바 버튼 인덱스
static UINT g_toolbarPressedCmd  = 0;   // 눌린 고정 툴바 버튼 commandId
static int  g_toolbarTipTimer = 0;      // 딜레이 타이머 ID
static Gdiplus::Bitmap* g_pMemoBitmap = nullptr; // memo.png 캐시
static bool g_isDraggingNote  = false;
static int  g_draggingNoteIdx = -1;
static int  g_noteDragOffsetX = 0; // 클릭 지점과 아이콘 원점의 화면 좌표 차이
static int  g_noteDragOffsetY = 0;

// ─── 텍스트 박스 ──────────────────────────────────────────────────────────────
struct TextBoxItem {
    int          pageIndex = 0;
    double       pdfX = 0.0;    // PDF 좌표 (좌상단)
    double       pdfY = 0.0;
    double       pdfW = 160.0;  // PDF 좌표 기준 너비
    double       pdfH = 60.0;   // PDF 좌표 기준 높이
    std::wstring text;
};
static std::vector<TextBoxItem> g_textBoxes;
static int  g_selectedTextBox   = -1;   // 선택된 텍스트 박스 인덱스
static bool g_isDraggingTextBox = false;
static int  g_tbDragOffsetX     = 0;
static int  g_tbDragOffsetY     = 0;
static HWND g_hTextBoxEdit      = nullptr; // 인라인 편집 컨트롤
static int  g_editingTextBox    = -1;      // 현재 편집 중인 인덱스
HWND g_hMainWnd          = nullptr; // 메인 윈도우 핸들 (TextBox용)

// 텍스트 박스 크기 조절
static bool   g_isResizingTextBox  = false;
static int    g_resizeHandle       = -1;   // 0=TL,1=TC,2=TR,3=ML,4=MR,5=BL,6=BC,7=BR
static double g_tbResizeInitX      = 0.0;
static double g_tbResizeInitY      = 0.0;
static double g_tbResizeInitW      = 0.0;
static double g_tbResizeInitH      = 0.0;
static int    g_tbResizeInitMouseX = 0;
static int    g_tbResizeInitMouseY = 0;

bool g_dirty = false;
bool g_twoPageView = false; // 두쪽 보기 모드

// ─── 서명 ────────────────────────────────────────────────────────────────────
struct SignatureItem {
    int    pageIndex = 0;
    double pdfX = 0.0;     // PDF 좌표 박스 좌측 (하단원점)
    double pdfY = 0.0;     // PDF 좌표 박스 상단 (하단원점)
    double pdfW = 120.0;
    double pdfH = 40.0;
    bool   isImage = false; // 이미지 불러오기로 생성된 서명
    // 획 데이터: 각 획은 PDF 좌표 포인트 목록 (하단원점)
    std::vector<std::vector<SigStrokePt>> strokes;
    // 화면 렌더링용 비트맵 캐시 (재생성 가능)
    Gdiplus::Bitmap* bitmap = nullptr;
    int srcW = 1, srcH = 1;
};
static std::vector<SignatureItem> g_signatures;
static int  g_selectedSig       = -1;
static bool g_isDraggingSig      = false;
static int  g_sigDragOffsetX     = 0;
static int  g_sigDragOffsetY     = 0;
static bool g_isResizingSig      = false;
static int  g_sigResizeHandle    = -1;  // 0=TL,1=TC,2=TR,3=ML,4=MR,5=BL,6=BC,7=BR
static double g_sigResizeInitX   = 0.0;
static double g_sigResizeInitY   = 0.0;
static double g_sigResizeInitW   = 0.0;
static double g_sigResizeInitH   = 0.0;
static int    g_sigResizeMouseX  = 0;
static int    g_sigResizeMouseY  = 0;
// 리사이즈 시작 시점의 획 좌표 스냅샷 (매 프레임 원본 기준으로 스케일)
static std::vector<std::vector<SigStrokePt>> g_sigResizeInitStrokes;

// ─── 도형 주석 (Square / Circle) ─────────────────────────────────────────────
struct ShapeItem {
    int    pageIndex = 0;
    double pdfX = 0.0;   // 좌측 (PDF 하단원점)
    double pdfY = 0.0;   // 상단 (PDF 하단원점)
    double pdfW = 0.0;
    double pdfH = 0.0;
    bool   isCircle = false;
    COLORREF color = RGB(255, 0, 0);
    float  lineWidth = 2.0f;
};
static std::vector<ShapeItem> g_shapes;
static int  g_shapeMode        = 0;    // 0=off, 1=rect, 2=circle
static bool g_isDraggingShape  = false;
static int  g_shapeDragStartX  = 0, g_shapeDragStartY  = 0;
static int  g_shapeDragCurX    = 0, g_shapeDragCurY    = 0;
static int  g_selectedShape    = -1;
static COLORREF g_shapeColor   = RGB(255, 0, 0);
static float    g_shapeLineWidth = 1.0f;

// ─── Ink (자유 그리기) ────────────────────────────────────────────────────────
struct InkStrokeItem {
    int pageIndex = 0;
    std::vector<std::pair<double,double>> pdfPts; // PDF 좌표 스트로크 점들
    COLORREF color = RGB(255, 0, 0);
    float lineWidth = 2.0f;
};
bool    g_inkMode       = false;
COLORREF g_inkColor     = RGB(255, 0, 0);   // 기본 빨강
static bool g_isDrawingInk  = false;
static int  g_selectedInkStroke = -1;       // 선택된 스트로크 인덱스
static std::vector<POINT>         g_inkStroke;    // 진행 중인 스트로크 (화면 좌표)
static std::vector<InkStrokeItem> g_inkStrokes;   // 완료된 스트로크 오버레이

// ─── Redact ───────────────────────────────────────────────────────────────────
struct RedactItem {
    int    pageIndex = 0;
    double pdfX = 0.0; // 좌측 (하단원점)
    double pdfY = 0.0; // 상단 (하단원점)
    double pdfW = 0.0;
    double pdfH = 0.0;
};
static std::vector<RedactItem> g_redacts;
static bool g_redactMode     = false; // 영역 지정 모드 활성 여부
static bool g_isDraggingRedact = false;
static int  g_redactDragStartX = 0, g_redactDragStartY = 0; // 화면 좌표
static int  g_redactDragCurX   = 0, g_redactDragCurY   = 0;
static int  g_selectedRedact   = -1;

// ─── 하이라이트 모드 ───────────────────────────────────────────────────────────
bool     g_highlightMode  = false;
COLORREF g_highlightColor = RGB(255, 255,  0); // 기본 노란색 (수마트라 PDF 동일)

// ─── 텍스트 선택 상태 ──────────────────────────────────────────────────────────
FPDF_TEXTPAGE g_textPage = nullptr;
bool          g_isSelecting = false;
bool          g_hasSelection = false;
static int           g_selCharStart = -1;    // 선택 시작 문자 인덱스
static int           g_selCharEnd = -1;      // 선택 끝 문자 인덱스
int           g_selPage = -1;         // 선택이 속한 페이지
static bool          g_selCaptured = false;  // SetCapture 호출 여부
static POINT         g_selDragStart = {};    // 드래그 시작 화면 좌표
static POINT         g_selLastMouse = { -1, -1 }; // 마지막으로 처리한 마우스 좌표 (스로틀링용)

// ─── 텍스트 선택 rect 캐시 (DrawTextSelection 재계산 방지) ────────────────────
struct SelRectCache {
    int charStart = -1, charEnd = -1;
    float zoom = 0.0f;
    struct R { double left, top, right, bottom; };
    std::vector<R> rects;
    void invalidate() { charStart = charEnd = -1; }
} g_selRectCache;

// ─── 선택 하이라이트용 영구 AlphaBlend DC (매 페인트마다 생성/삭제 방지) ─────
static HDC     g_selBlendDC  = nullptr;
static HBITMAP g_selBlendBmp = nullptr;
static HBITMAP g_selBlendOld = nullptr;

// ─── 사용자 설정 ──────────────────────────────────────────────────────────────
// g_selectionColor는 g_prefs 초기화 전 DrawTextSelection에서도 참조되므로 별도 유지
static COLORREF g_selectionColor = RGB(51, 144, 255);

// 툴바 버튼 hover 상태
int         g_hoverButton = -1;
static bool g_trackingMouse = false;

// 패닝(손 도구) 상태
bool         g_handMode = false;   // 손 도구 활성화 여부
static bool  g_isPanning = false;   // 현재 드래그 중
static POINT g_panStart = {};      // 드래그 시작 마우스 좌표
static int   g_panStartX = 0;      // 드래그 시작 시 스크롤 X
static int   g_panStartY = 0;      // 드래그 시작 시 스크롤 Y
static HCURSOR g_curHand = nullptr; // 열린 손 커서
static HCURSOR g_curGrab = nullptr; // 잡은 손 커서

// ─── 연속 스크롤 (Continuous Scroll) ─────────────────────────────────────────
#define PAGE_GAP    16  // 페이지 간 간격 (px)
#define SIDE_MARGIN 0  // 뷰포트 양쪽 고정 여백 (px)

// PageLayoutInfo 구조체는 PlainPDFViewer.h 참조
std::vector<PageLayoutInfo> g_pageLayouts;
int g_totalDocHeight = 0;
int g_maxPageWidth = 0;

// ─── 페이지 비트맵 캐시 ───────────────────────────────────────────────────────
struct PageCacheEntry {
    HBITMAP hBmp = nullptr;
    int     width = 0;
    int     height = 0;
    float   zoom = 0.0f;
    bool    dark = false;
};
static std::vector<PageCacheEntry> g_pageCache;

// ─── 썸네일 비트맵 캐시 ──────────────────────────────────────────────────────
struct ThumbCacheEntry {
    HBITMAP hBmp   = nullptr;
    int     width  = 0;
    int     height = 0;
};
static std::vector<ThumbCacheEntry> g_thumbCache;

// 인접 페이지 프리페치 메시지 및 큐
// 인접 페이지 프리패치 메시지
#define WM_PREFETCH_PAGE    (WM_USER + 203)
#define WM_RENDER_DONE      (WM_USER + 207)

// ─── 백그라운드 렌더 워커 구조체 ──────────────────────────────────────────
// worker는 픽셀 바이트 버퍼만 생성. HBITMAP 생성/캐시 저장은 UI 스레드 전담.
struct RenderRequest {
    int   pageIndex = 0;
    int   renderW   = 0;
    int   renderH   = 0;
    float zoom      = 1.0f;
    int   rotation  = 0;
    HWND  hWnd      = nullptr;
    // 워커 전용 문서 핸들 — shared_ptr로 수명 보장 (탭 교체/해제 후에도 안전)
    std::shared_ptr<std::vector<char>> pdfBuf;
};

struct RenderResult {
    int               pageIndex;
    int               width;
    int               height;
    float             zoom;
    std::vector<BYTE> pixels;
    std::shared_ptr<std::vector<char>> pdfBuf;
    int               rotation = 0;
};

// ─── 워커 전역 상태
static std::deque<RenderRequest> g_renderQueue;
static std::mutex                g_renderMtx;
std::mutex                       g_pdfiumMtx;
static std::condition_variable   g_renderCV;
static bool                      g_pwPromptCancelled = false;   // 암호 입력 취소 여부 (오류 팝업 생략용)
static std::thread               g_renderThread;
static std::atomic<bool>         g_renderStop{ false };
static std::atomic<int>          g_renderFocusPage{ 0 };   //워커가 우선 처리할 페이지
// 화면에 실제로 걸쳐 보이는 페이지 범위(반씩 겹쳐 보이는 경우 둘 다 포함) — 이 범위 내부는 모두 최우선(거리 0)으로 처리
static std::atomic<int>          g_renderFocusFirst{ 0 };
static std::atomic<int>          g_renderFocusLast{ 0 };

static std::vector<bool> g_prefetchPending;
static std::vector<bool> g_annotPageLoaded;
static std::vector<bool> g_annotPagePosted;
static bool g_annotLoadReady = false;
// ─── 렌더 큐 flush (g_prefetchPending 초기화 + worker 큐 비우기) ─────────────
static void FlushRenderQueue()
{
    {
        std::lock_guard<std::mutex> lk(g_renderMtx);
        g_renderQueue.clear();
    }
    if (!g_prefetchPending.empty())
        std::fill(g_prefetchPending.begin(), g_prefetchPending.end(), false);
}

// ─── 이전 호환 래퍼 ───────────────────────────────────────────────────────────
static void ClearRenderQueue() { FlushRenderQueue(); }

static void ClearPreviewCache();

void ClearPageCache()
{
    FlushRenderQueue();
    for (auto& e : g_pageCache)
        if (e.hBmp) { DeleteObject(e.hBmp); e.hBmp = nullptr; }
    g_pageCache.clear();
    g_pageBaseDirty = true;
}

void InitPageCache(int pageCount)
{
    ClearPageCache();
    ClearPreviewCache();
    g_pageCache.resize(pageCount);
    g_prefetchPending.assign(pageCount, false);
    g_annotPageLoaded.assign(pageCount, false);
    g_annotPagePosted.assign(pageCount, false);
}

// ─── 백그라운드 렌더 워커 함수 ────────────────────────────────────────────────
// 워커는 전달받은 PDF 버퍼로 독립 FPDF_Document를 열어 렌더링하므로
// g_pdfDoc / UI 스레드와 어떤 공유 상태도 없음 → 락 불필요.
static std::string g_docPassword;   // 현재 문서의 열기 암호 (메모리 전용, 보호 문서 렌더링용)

static void RenderWorkerProc()
{
    // 워커 전용 영속 문서 (버퍼가 같으면 재사용)
    std::shared_ptr<std::vector<char>> cachedBuf;
    FPDF_DOCUMENT cachedDoc = nullptr;

    while (true)
    {
        RenderRequest req;
        {
            std::unique_lock<std::mutex> lk(g_renderMtx);
            g_renderCV.wait(lk, [] { return g_renderStop.load() || !g_renderQueue.empty(); });
            if (g_renderStop.load() && g_renderQueue.empty()) break;
            // 현재 화면(focus)에 가장 가까운 요청을 먼저 처리 (deque 랜덤 접근)
            // 위/아래 페이지가 반씩 겹쳐 보이는 경우 둘 다 화면에 걸쳐 있으므로
            // [focusFirst, focusLast] 범위 안이면 모두 거리 0(최우선)으로 취급한다.
            const int focus      = g_renderFocusPage.load(std::memory_order_relaxed);
            const int focusFirst = g_renderFocusFirst.load(std::memory_order_relaxed);
            const int focusLast  = g_renderFocusLast.load(std::memory_order_relaxed);
            auto distToFocus = [&](int pageIndex) {
                if (pageIndex >= focusFirst && pageIndex <= focusLast) return 0;
                return std::min(std::abs(pageIndex - focus),
                    std::min(std::abs(pageIndex - focusFirst), std::abs(pageIndex - focusLast)));
            };
            size_t best = 0;
            int    bestDist = distToFocus(g_renderQueue[0].pageIndex);
            for (size_t k = 1; k < g_renderQueue.size(); ++k)
            {
                int d = distToFocus(g_renderQueue[k].pageIndex);
                if (d < bestDist) { bestDist = d; best = k; }
            }
            req = g_renderQueue[best];
            g_renderQueue.erase(g_renderQueue.begin() + best);
        }
        if (!req.pdfBuf || req.pdfBuf->empty()) continue;

        FPDF_PAGE page = nullptr;
        std::vector<BYTE> pixels;
        {
            // PDFium은 스레드 안전하지 않으므로 UI 스레드와 동시에 호출되지 않도록 직렬화
            std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);

            // 버퍼가 바뀌었을 때만 문서를 다시 연다
            if (req.pdfBuf != cachedBuf)
            {
                if (cachedDoc) FPDF_CloseDocument(cachedDoc);
                cachedDoc = FPDF_LoadMemDocument(req.pdfBuf->data(),
                    (int)req.pdfBuf->size(),
                                        g_docPassword.empty() ? nullptr : g_docPassword.c_str());
                                    cachedBuf = req.pdfBuf;
            }
            if (!cachedDoc)
            {
                cachedBuf.reset();
            }
            else
            {
                page = FPDF_LoadPage(cachedDoc, req.pageIndex);
                if (page)
                {
                    FPDF_BITMAP fpdfBmp = FPDFBitmap_Create(req.renderW, req.renderH, 0);
                    if (fpdfBmp)
                    {
                        FPDFBitmap_FillRect(fpdfBmp, 0, 0, req.renderW, req.renderH, 0xFFFFFFFF);
                        FPDF_RenderPageBitmap(fpdfBmp, page, 0, 0, req.renderW, req.renderH,
                            req.rotation, FPDF_ANNOT | FPDF_LCD_TEXT);
                        const BYTE* src = (const BYTE*)FPDFBitmap_GetBuffer(fpdfBmp);
                        const int   stride = FPDFBitmap_GetStride(fpdfBmp);
                        const int   rowBytes = req.renderW * 4;
                        pixels.resize((size_t)req.renderH * rowBytes);
                        for (int y = 0; y < req.renderH; ++y)
                            memcpy(pixels.data() + (ptrdiff_t)y * rowBytes,
                                src + (ptrdiff_t)y * stride, rowBytes);
                        FPDFBitmap_Destroy(fpdfBmp);
                    }
                    FPDF_ClosePage(page);          // 페이지만 닫고, 문서는 유지
                }
            }
        }
        if (pixels.empty()) continue;

        RenderResult* res = new RenderResult{
            req.pageIndex, req.renderW, req.renderH, req.zoom, std::move(pixels), req.pdfBuf, req.rotation };
        PostMessageW(req.hWnd, WM_RENDER_DONE, 0, (LPARAM)res);
    }
    {
        std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);
        if (cachedDoc) FPDF_CloseDocument(cachedDoc);  // 종료 시 정리
    }
}

static void StartRenderWorker()
{
    g_renderStop.store(false);
    g_renderThread = std::thread(RenderWorkerProc);
}

static void StopRenderWorker()
{
    {
        std::lock_guard<std::mutex> lk(g_renderMtx);
        g_renderStop.store(true);
        g_renderQueue.clear();
    }
    g_renderCV.notify_all();
    if (g_renderThread.joinable()) g_renderThread.join();
}


static void ClearThumbCache()
{
    for (auto& e : g_thumbCache)
        if (e.hBmp) { DeleteObject(e.hBmp); e.hBmp = nullptr; }
    g_thumbCache.clear();
}

static void InitThumbCache(int pageCount)
{
    ClearThumbCache();
    g_thumbCache.resize(pageCount);
}

// ─── 저해상도 프리뷰 캐시 (placeholder 용) ──────────────────────────────────
// 줌 변경에도 유지하고, 회전/문서 변경 시에만 무효화한다.
struct PreviewEntry { HBITMAP hBmp = nullptr; int w = 0, h = 0; };
static std::vector<PreviewEntry> g_previewCache;

static void ClearPreviewCache()
{
    for (auto& e : g_previewCache)
        if (e.hBmp) { DeleteObject(e.hBmp); e.hBmp = nullptr; }
    g_previewCache.clear();
}

// ─── 캐시/프리뷰가 모두 없는 페이지용 빠른 저해상도 미리보기 즉시 생성 ─────────
// 전체 해상도 렌더링은 백그라운드 워커(단일 스레드)가 순차 처리하므로 빠르게
// 스크롤하면 아직 렌더링되지 않은 페이지가 흰 백지로 보일 수 있다.
// 이를 막기 위해 UI 스레드에서 아주 작은 크기로 즉시(동기) 렌더링해 프리뷰
// 캐시를 채운다 — DrawSidebarThumbnails의 캐시 미스 처리와 동일한 방식이며
// 결과물 크기가 작아 비용이 매우 낮다. 이후 백그라운드 렌더링이 끝나면
// WM_RENDER_DONE 처리에서 이 프리뷰를 고화질로 교체한다.
static void EnsureQuickPreview(int pageIndex, int fullW, int fullH)
{
    if (!g_pdfDoc || fullW <= 0 || fullH <= 0) return;
    if (pageIndex < 0 || pageIndex >= g_pageCount) return;

    if ((int)g_previewCache.size() != g_pageCount)
        g_previewCache.assign(g_pageCount, PreviewEntry{});
    if (g_previewCache[pageIndex].hBmp) return; // 이미 존재

    const int PREV_W = 160;
    const int prevW = std::min(PREV_W, fullW);
    const int prevH = std::max(1, (int)((double)fullH / fullW * prevW));

    // PDFium은 스레드 안전하지 않으므로 백그라운드 렌더 워커와 동시에 호출되지 않도록 직렬화
    std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);

    const bool useCurrentPage = (pageIndex == g_currentPage && g_pdfPage);
    FPDF_PAGE page = useCurrentPage ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, pageIndex);
    if (!page) return;

    FPDF_BITMAP fpdfBmp = FPDFBitmap_Create(prevW, prevH, 0);
    if (fpdfBmp)
    {
        FPDFBitmap_FillRect(fpdfBmp, 0, 0, prevW, prevH, 0xFFFFFFFF);
        FPDF_RenderPageBitmap(fpdfBmp, page, 0, 0, prevW, prevH, GetEffectiveRotation(pageIndex), FPDF_ANNOT);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = prevW;
        bmi.bmiHeader.biHeight      = -prevH;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dibBits = nullptr;
        HDC     tmpDC = CreateCompatibleDC(nullptr);
        HBITMAP hDib  = CreateDIBSection(tmpDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        DeleteDC(tmpDC);

        const BYTE* src = static_cast<const BYTE*>(FPDFBitmap_GetBuffer(fpdfBmp));
        if (hDib && dibBits && src)
        {
            const int stride = FPDFBitmap_GetStride(fpdfBmp);
            BYTE* dst = static_cast<BYTE*>(dibBits);
            for (int y = 0; y < prevH; ++y)
                memcpy(dst + (ptrdiff_t)y * prevW * 4, src + (ptrdiff_t)y * stride, (size_t)prevW * 4);

            g_previewCache[pageIndex].hBmp = hDib;
            g_previewCache[pageIndex].w    = prevW;
            g_previewCache[pageIndex].h    = prevH;
        }
        else if (hDib)
        {
            DeleteObject(hDib);
        }
        FPDFBitmap_Destroy(fpdfBmp);
    }

    if (!useCurrentPage)
        FPDF_ClosePage(page);
}

// ─── 찾기 상태 (FindBar.h / FindBar.cpp 참조) ─────────────────────────
HWND          g_hFindPanel = nullptr;
HWND          g_hFindEdit = nullptr;
HWND          g_hFindLabel = nullptr;
HWND          g_hFindPrevBtn = nullptr;
HWND          g_hFindNextBtn = nullptr;
HWND          g_hFindCloseBtn = nullptr;
HWND          g_hFindCaseChk = nullptr;
bool          g_findBarVisible    = false;
bool          g_quickJumpVisible  = false;
bool          g_findCaseSensitive = false;
WCHAR         g_findText[256] = {};

FPDF_SCHHANDLE g_findHandle = nullptr;
int            g_findPage = -1;
int            g_findCharIndex = -1;
int            g_findCharCount = 0;
int            g_findResultIdx = 0;
int            g_findResultTotal = 0;
std::vector<int> g_findPageCounts;

WNDPROC        g_origEditProc = nullptr;

// ─── 북마크/목차 사이드바 ─────────────────────────────────────────────────────
#define SIDEBAR_WIDTH    220
#define SIDEBAR_ITEM_H   28
#define SIDEBAR_INDENT   18
#define SIDEBAR_HEADER_H 36
#define SIDEBAR_THUMB_PAD 45   // 썸네일 좌우 여백
#define SIDEBAR_THUMB_GAP 8    // 썸네일 간 세로 간격
#define THUMB_SB_W        6    // 썸네일 사이드바 스크롤바 너비

// enum SidebarTab은 Toolbar.h에 정의됨

struct BookmarkItem {
    std::wstring title;
    int          pageIndex = 0;
    int          level = 0;
    bool         isOpen = false;
    bool         hasChildren = false;
    bool         isUserAdded = false; // 사용자가 직접 추가한 북마크
};
bool                            g_sidebarVisible = false;
static const UINT_PTR           SB_ANIM_TIMER = 0x5342;
static const ULONGLONG          SB_ANIM_MS = 160;
static bool                     s_sbAnimating = false;
static bool                     s_sbClosing = false;
static ULONGLONG                s_sbStart = 0;
static int                      s_sbOffset = 0;
SidebarTab                      g_sidebarTab = TAB_BOOKMARKS;
static std::vector<BookmarkItem> g_bookmarks;
static int                      g_sidebarScrollY = 0;
static int                      g_sidebarHoverIdx = -1;
static int                      g_thumbScrollY = 0;
static int                      g_thumbHoverIdx = -1;
// 썸네일 스크롤바 드래그 상태
static bool                     g_thumbSbDragging = false;
static int                      g_thumbSbDragStartY = 0;
static int                      g_thumbSbDragStartScrollY = 0;

// ─── 로딩 GIF ─────────────────────────────────────────────────────────────────
#define WM_PDF_LOADED       (WM_USER + 200)   // 백그라운드 로딩 완료 알림
#define WM_PDF_INIT         (WM_USER + 201)   // 로딩 완료 후 초기화 (메시지 루프 양보)
#define WM_PDF_INIT2        (WM_USER + 204)   // 초기화 2단계: 주석 로드 시작
#define WM_ANNOT_LOADED     (WM_USER + 205)   // 백그라운드 주석 로드 완료 알림
#define WM_ANNOT_PAGE       (WM_USER + 206)   // 페이지별 주석 로드 (wParam = 페이지 인덱스)
#define WM_SHOW_TEXT_VIEWER (WM_USER + 202)   // TextViewer 창 표시 (PostMessage 지연용)

// 어노테이션 로딩 결과 수집용 (동기 로드에서도 재사용)
struct AnnotLoadResult {
    std::vector<HighlightItem>  highlights;
    std::vector<UnderlineItem>  underlines;
    std::vector<StrikeoutItem>  strikeouts;
    std::vector<SquigglyItem>   squigglies;
    std::vector<NoteItem>       notes;
    std::vector<SignatureItem>  signatures;
    std::vector<TextBoxItem>    textBoxes;
    std::vector<ShapeItem>      shapes;
    std::vector<InkStrokeItem>  inkStrokes;
};

struct LoadingResult {
    FPDF_DOCUMENT    doc;
    std::wstring     path;
    bool             isReplace;
    AnnotLoadResult* annots = nullptr;     // 백그라운드에서 미리 로드한 주석 (없으면 nullptr)
    std::shared_ptr<std::vector<char>> pdfBuffer;  // FPDF_LoadMemDocument 수명 유지용 버퍼
};

static bool                      g_isLoading        = false;
static int                       g_loadingFrame     = 0;

static void StartLoadingGif(HWND hWnd)
{
    g_isLoading    = true;
    g_loadingFrame = 0;
    SetTimer(hWnd, LOADING_ANIM_TIMER_ID, LOADING_ANIM_INTERVAL, nullptr);
    InvalidateRect(hWnd, nullptr, FALSE);
    UpdateWindow(hWnd);
}

static void StopLoadingGif(HWND hWnd)
{
    g_isLoading = false;
    KillTimer(hWnd, LOADING_ANIM_TIMER_ID);
    InvalidateRect(hWnd, nullptr, FALSE);
}

static void DrawLoadingOverlay(HDC hdc, const RECT& clientRect)
{
    if (!g_isLoading) return;

    const int cw = clientRect.right  - clientRect.left;
    const int ch = clientRect.bottom - clientRect.top;

    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    // 배경: 밝은 단색
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 245, 246, 248));
    g.FillRectangle(&bgBrush, 0, 0, cw, ch);

    const float cx = cw / 2.0f;
    const float cy = ch / 2.0f;

    // PDF 문서 아이콘 (단순 사각형)
    const float iconW = 38.0f, iconH = 48.0f;
    const float iconX = cx - iconW / 2.0f;
    const float iconY = cy - 44.0f;
    const float fold  = 10.0f; // 접힌 모서리 크기

    // 아이콘 본체
    Gdiplus::GraphicsPath iconPath;
    iconPath.AddLine(iconX + fold, iconY, iconX + iconW, iconY);
    iconPath.AddLine(iconX + iconW, iconY, iconX + iconW, iconY + iconH);
    iconPath.AddLine(iconX + iconW, iconY + iconH, iconX, iconY + iconH);
    iconPath.AddLine(iconX, iconY + iconH, iconX, iconY + fold);
    iconPath.CloseFigure();
    Gdiplus::SolidBrush iconBg(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::Pen iconBorder(Gdiplus::Color(255, 180, 190, 205), 1.5f);
    g.FillPath(&iconBg, &iconPath);
    g.DrawPath(&iconBorder, &iconPath);

    // 접힌 모서리
    Gdiplus::GraphicsPath foldPath;
    foldPath.AddLine(iconX, iconY + fold, iconX + fold, iconY + fold);
    foldPath.AddLine(iconX + fold, iconY + fold, iconX + fold, iconY);
    Gdiplus::SolidBrush foldBrush(Gdiplus::Color(255, 210, 218, 228));
    g.FillPath(&foldBrush, &foldPath);
    Gdiplus::Pen foldPen(Gdiplus::Color(255, 180, 190, 205), 1.5f);
    g.DrawPath(&foldPen, &foldPath);

    // 아이콘 안 줄 (문서 느낌)
    Gdiplus::Pen linePen(Gdiplus::Color(255, 200, 210, 225), 1.5f);
    for (int li = 0; li < 3; ++li)
    {
        float ly = iconY + 18.0f + li * 8.0f;
        g.DrawLine(&linePen, iconX + 8.0f, ly, iconX + iconW - 8.0f, ly);
    }

    // 회전 스피너
    const float spinR     = 16.0f;  // 스피너 원 반지름
    const float dotR      = 3.2f;   // 각 점의 반지름
    const float spinCx    = cx;
    const float spinCy    = iconY + iconH + 30.0f;
    const int   totalDots = 8;
    const int   activeDot = g_loadingFrame % totalDots;

    for (int i = 0; i < totalDots; ++i)
    {
        int dist  = (activeDot - i + totalDots) % totalDots;
        int alpha = 255 - dist * 28;
        if (alpha < 40) alpha = 40;

        float angleDeg = i * (360.0f / totalDots) - 90.0f;
        float angleRad = angleDeg * 3.14159265f / 180.0f;
        float dx       = spinCx + spinR * std::cos(angleRad);
        float dy       = spinCy + spinR * std::sin(angleRad);

        Gdiplus::SolidBrush dotBrush(Gdiplus::Color((BYTE)alpha, 90, 130, 190));
        g.FillEllipse(&dotBrush, dx - dotR, dy - dotR, dotR * 2.0f, dotR * 2.0f);
    }

    // "불러오는 중..." 텍스트
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font       font(&ff, 13, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 90, 100, 115));
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF textRect(cx - 120.0f, spinCy + spinR + 10.0f, 240.0f, 24.0f);
    g.DrawString(L"불러오는 중...", -1, &font, textRect, &sf, &textBrush);
}


#define TAB_BAR_HEIGHT 30
#define TAB_WIDTH      180
#define TAB_CLOSE_SIZE 22

struct TabState {
    // PDF
    FPDF_DOCUMENT pdfDoc = nullptr;
    FPDF_PAGE     pdfPage = nullptr;
    FPDF_TEXTPAGE textPage = nullptr;
    int           currentPage = 0;
    int           pageCount = 0;
    float         zoom = 1.0f;
    int           rotation = 0;
    std::vector<int> pageRot;
    bool          darkMode = false;
    int           scrollX = 0;
    int           scrollY = 0;
    float         scrollYf = 0.0f;
    std::wstring  filePath;
    // 레이아웃
    std::vector<PageLayoutInfo> pageLayouts;
    int           totalDocHeight = 0;
    int           maxPageWidth = 0;
    // 캐시
    std::vector<PageCacheEntry> pageCache;
    // 선택
    bool          hasSelection = false;
    bool          isSelecting = false;
    int           selCharStart = -1;
    int           selCharEnd = -1;
    int           selPage = -1;
    // 하이라이트
    std::vector<HighlightItem> highlights;
    int           selectedHighlight = -1;
    // 밑줄
    std::vector<UnderlineItem> underlines;
    int           selectedUnderline = -1;
    // 취소선
    std::vector<StrikeoutItem> strikeouts;
    int           selectedStrikeout = -1;
    // 물결 밑줄
    std::vector<SquigglyItem> squigglies;
    int           selectedSquiggly = -1;
    // 메모
    std::vector<NoteItem> notes;
    int           selectedNote = -1;
    // 텍스트 박스
    std::vector<TextBoxItem> textBoxes;
    int           selectedTextBox = -1;
    // 서명
    std::vector<SignatureItem> signatures;
    int           selectedSig = -1;
    // 도형 (원/사각형)
    std::vector<ShapeItem> shapes;
    int           selectedShape = -1;
    // 잉크 스트로크 오버레이
    std::vector<InkStrokeItem> inkStrokes;
    int           selectedInkStroke = -1;
    // 변경 추적
    bool          dirty = false;
    // 사이드바
    bool          sidebarVisible = false;
    SidebarTab    sidebarTab = TAB_BOOKMARKS;
    std::vector<BookmarkItem> bookmarks;
    int           sidebarScrollY = 0;
    int           thumbScrollY = 0;
    std::vector<ThumbCacheEntry> thumbCache;
    // 찾기
    FPDF_SCHHANDLE findHandle = nullptr;
    int           findPage = -1;
    int           findCharIndex = -1;
    int           findCharCount = 0;
    int           findResultIdx = 0;
    int           findResultTotal = 0;
    WCHAR         findText[256] = {};
    // 지연 주석 로드 상태
    std::vector<bool> annotPageLoaded;
    std::vector<bool> annotPagePosted;
    bool annotLoadReady = false;
    // FPDF_LoadMemDocument 수명 유지용 버퍼 (파일 핸들 즉시 해제를 위해 메모리 로드)
    std::shared_ptr<std::vector<char>> pdfBuffer;  // FPDF_LoadMemDocument 수명 유지용 버퍼 (shared)
    // 폼 핸들
    FPDF_FORMHANDLE formHandle = nullptr;
};

static std::vector<TabState> g_tabs;
static int g_activeTab = -1;
static int g_tabHover = -1;
static int g_tabCloseHover = -1;  // 닫기 버튼 hover 중인 탭
static bool g_tabPlusHover = false; // '+' 버튼 hover

// ─── 프리패치 헬퍼 ──────────────────────────────────────────────────────────
// 단일 페이지를 워커 큐에 등록 (캐시가 이미 유효하거나 이미 등록됐으면 건너뜀)
static void PostPrefetch(HWND hWnd, int pageIndex)
{
    if (!g_pdfDoc) return;
    if (pageIndex < 0 || pageIndex >= g_pageCount) return;
    if (pageIndex >= (int)g_pageLayouts.size()) return;

    if (pageIndex < (int)g_pageCache.size()
        && g_pageCache[pageIndex].hBmp
        && g_pageCache[pageIndex].zoom == g_zoom)
        return;

    if (pageIndex < (int)g_prefetchPending.size() && g_prefetchPending[pageIndex])
        return;
    if (pageIndex < (int)g_prefetchPending.size())
        g_prefetchPending[pageIndex] = true;

    // 현재 탭의 shared_ptr 버퍼를 워커에게 공유
    std::shared_ptr<std::vector<char>> buf;
    if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
        buf = g_tabs[g_activeTab].pdfBuffer;
    if (!buf || buf->empty()) return;

    RenderRequest req{
        pageIndex,
        g_pageLayouts[pageIndex].renderW,
        g_pageLayouts[pageIndex].renderH,
        g_zoom,
        GetEffectiveRotation(pageIndex),
        hWnd,
        buf
    };
    {
        std::lock_guard<std::mutex> lk(g_renderMtx);
        // 큐에 동일 페이지/줌 요청이 이미 있으면 중복 추가 안 함
        for (const auto& q : g_renderQueue)
            if (q.pageIndex == pageIndex && q.zoom == g_zoom) return;
        g_renderQueue.push_back(req);
    }
    g_renderCV.notify_one();
}

// 뷰포트에 실제 보이는 페이지 범위 + 앞뒤 버퍼를 워커 큐에 등록
static void SchedulePrefetch(HWND hWnd)
{
    if (!g_pdfDoc || g_pageCount <= 0 || g_pageLayouts.empty()) return;

    g_renderFocusPage.store(g_currentPage, std::memory_order_relaxed);

    RECT cr; GetClientRect(hWnd, &cr);
    const RECT vp = GetViewportRect(cr);
    const int vpTop    = g_scrollY;
    const int vpBottom = g_scrollY + (vp.bottom - vp.top);

    // 뷰포트와 겹치는 첫/마지막 페이지 탐색
    int firstVisible = g_pageCount - 1;
    int lastVisible  = 0;
    for (int i = 0; i < g_pageCount; ++i)
    {
        const int pageTop = g_pageLayouts[i].yOffset;
        const int pageBot = pageTop + g_pageLayouts[i].renderH;
        if (pageBot > vpTop && pageTop < vpBottom)
        {
            firstVisible = std::min(firstVisible, i);
            lastVisible  = std::max(lastVisible,  i);
        }
    }

    // 위/아래 페이지가 반씩 겹쳐 보이는 등 실제로 화면에 걸친 모든 페이지를
    // 워커가 동일한 최우선 순위로 처리하도록 범위를 전달 (한쪽만 흐리게 남는 문제 방지)
    g_renderFocusFirst.store(firstVisible, std::memory_order_relaxed);
    g_renderFocusLast.store(lastVisible, std::memory_order_relaxed);

    // 보이는 페이지 + 앞뒤 2페이지 버퍼를 현재 페이지 우선 순으로 요청
    const int bufPages = 2;
    const int from = std::max(0, firstVisible - bufPages);
    const int to   = std::min(g_pageCount - 1, lastVisible + bufPages);

    // 현재 페이지를 가장 먼저, 이후 화면 순으로
    PostPrefetch(hWnd, g_currentPage);
    for (int i = firstVisible; i <= lastVisible; ++i)
        PostPrefetch(hWnd, i);
    for (int i = firstVisible - 1; i >= from; --i)
        PostPrefetch(hWnd, i);
    for (int i = lastVisible + 1; i <= to; ++i)
        PostPrefetch(hWnd, i);
}

// 현재 전역 상태를 TabState에 저장
static void SaveTabState(TabState& ts)
{
    ts.pdfDoc = g_pdfDoc;
    ts.pdfPage = g_pdfPage;
    ts.textPage = g_textPage;
    ts.currentPage = g_currentPage;
    ts.pageCount = g_pageCount;
    ts.zoom = g_zoom;
    ts.rotation = g_rotation;
    ts.pageRot = g_pageRot;
    ts.darkMode = g_darkMode;
    ts.scrollX = g_scrollX;
    ts.scrollY = g_scrollY;
    ts.scrollYf = g_scrollYf;
    ts.filePath = g_filePath;
    ts.pageLayouts = std::move(g_pageLayouts);
    ts.totalDocHeight = g_totalDocHeight;
    ts.maxPageWidth = g_maxPageWidth;
    ts.pageCache = std::move(g_pageCache);
    ts.hasSelection = g_hasSelection;
    ts.isSelecting = g_isSelecting;
    ts.selCharStart = g_selCharStart;
    ts.selCharEnd = g_selCharEnd;
    ts.selPage = g_selPage;
    ts.highlights = g_highlights;
    ts.selectedHighlight = g_selectedHighlight;
    ts.underlines = g_underlines;
    ts.selectedUnderline = g_selectedUnderline;
    ts.strikeouts = g_strikeouts;
    ts.selectedStrikeout = g_selectedStrikeout;
    ts.squigglies = g_squigglies;
    ts.selectedSquiggly = g_selectedSquiggly;
    ts.notes = g_notes;
    ts.selectedNote = g_selectedNote;
    ts.textBoxes = g_textBoxes;
    ts.selectedTextBox = g_selectedTextBox;
    ts.signatures = g_signatures;
    // 탭 상태에는 비트맵 포인터를 보관하지 않음 (이중 해제 방지)
    for (auto& s : ts.signatures) s.bitmap = nullptr;
    ts.selectedSig = g_selectedSig;
    ts.shapes = g_shapes;
    ts.inkStrokes = g_inkStrokes;
    ts.selectedInkStroke = g_selectedInkStroke;
    ts.selectedShape = g_selectedShape;
    ts.sidebarVisible = g_sidebarVisible;
    ts.sidebarTab = g_sidebarTab;
    ts.bookmarks = std::move(g_bookmarks);
    ts.sidebarScrollY = g_sidebarScrollY;
    ts.thumbScrollY = g_thumbScrollY;
    ts.thumbCache = std::move(g_thumbCache);
    ts.findHandle = g_findHandle;
    ts.findPage = g_findPage;
    ts.findCharIndex = g_findCharIndex;
    ts.findCharCount = g_findCharCount;
    ts.findResultIdx = g_findResultIdx;
    ts.findResultTotal = g_findResultTotal;
    memcpy(ts.findText, g_findText, sizeof(g_findText));
    ts.dirty = g_dirty;
    ts.annotPageLoaded = g_annotPageLoaded;
    ts.annotPagePosted = g_annotPagePosted;
    ts.annotLoadReady = g_annotLoadReady;
    ts.formHandle = g_formHandle;
}
static void RestoreTabState(TabState& ts)
{
    g_pdfDoc = ts.pdfDoc;
    g_pdfPage = ts.pdfPage;
    g_textPage = ts.textPage;
    g_currentPage = ts.currentPage;
    g_pageCount = ts.pageCount;
    g_zoom = ts.zoom;
    g_rotation = ts.rotation;
    g_pageRot = ts.pageRot;
    g_darkMode = ts.darkMode;
    g_scrollX = ts.scrollX;
    g_scrollY = ts.scrollY;
    g_scrollYf = ts.scrollYf;
    g_filePath = ts.filePath;
    g_pageLayouts = std::move(ts.pageLayouts);
    g_totalDocHeight = ts.totalDocHeight;
    g_maxPageWidth = ts.maxPageWidth;
    g_pageCache = std::move(ts.pageCache);
    g_hasSelection = ts.hasSelection;
    g_isSelecting = ts.isSelecting;
    g_selCharStart = ts.selCharStart;
    g_selCharEnd = ts.selCharEnd;
    g_selPage = ts.selPage;
    g_highlights = std::move(ts.highlights);
    g_selectedHighlight = ts.selectedHighlight;
    g_underlines = std::move(ts.underlines);
    g_selectedUnderline = ts.selectedUnderline;
    g_strikeouts = std::move(ts.strikeouts);
    g_selectedStrikeout = ts.selectedStrikeout;
    g_squigglies = std::move(ts.squigglies);
    g_selectedSquiggly = ts.selectedSquiggly;
    g_notes = std::move(ts.notes);
    g_selectedNote = ts.selectedNote;
    g_textBoxes = std::move(ts.textBoxes);
    g_selectedTextBox = ts.selectedTextBox;
    g_signatures = std::move(ts.signatures);
    g_selectedSig = ts.selectedSig;
    g_shapes = std::move(ts.shapes);
    g_inkStrokes = std::move(ts.inkStrokes);
    g_selectedInkStroke = ts.selectedInkStroke;
    g_selectedShape = ts.selectedShape;
    g_sidebarVisible = ts.sidebarVisible;
    g_sidebarTab = ts.sidebarTab;
    g_bookmarks = std::move(ts.bookmarks);
    g_sidebarScrollY = ts.sidebarScrollY;
    g_thumbScrollY = ts.thumbScrollY;
    g_thumbCache = std::move(ts.thumbCache);
    g_findHandle = ts.findHandle;
    g_findPage = ts.findPage;
    g_findCharIndex = ts.findCharIndex;
    g_findCharCount = ts.findCharCount;
    g_findResultIdx = ts.findResultIdx;
    g_findResultTotal = ts.findResultTotal;
    memcpy(g_findText, ts.findText, sizeof(g_findText));
    g_dirty = ts.dirty;
    g_annotPageLoaded = ts.annotPageLoaded;
    g_annotPagePosted = ts.annotPagePosted;
    g_annotLoadReady = ts.annotLoadReady;
    g_formHandle = ts.formHandle;
}
static void ClearGlobalsWithoutClose()
{
    // (timer removed)

    g_pdfDoc = nullptr;
    g_pdfPage = nullptr;
    g_textPage = nullptr;
    g_currentPage = 0;
    g_pageCount = 0;
    g_zoom = 1.0f;
    g_rotation = 0;
    g_pageRot.clear();
    g_darkMode = false;
    g_scrollX = 0;
    g_scrollY = 0;
    g_scrollYf = 0.0f;
    g_filePath.clear();
    g_pageLayouts.clear();
    g_totalDocHeight = 0;
    g_maxPageWidth = 0;
    g_pageCache.clear();
    g_hasSelection = false;
    g_isSelecting = false;
    g_selCaptured = false;
    g_selCharStart = g_selCharEnd = -1;
    g_selRectCache.invalidate();
    g_selPage = -1;
    g_highlights.clear();
    g_selectedHighlight = -1;
    g_underlines.clear();
    g_selectedUnderline = -1;
    g_strikeouts.clear();
    g_selectedStrikeout = -1;
    g_squigglies.clear();
    g_selectedSquiggly = -1;
    g_notes.clear();
    g_selectedNote = -1;
    g_hoverNote = -1;
    g_textBoxes.clear();
    g_selectedTextBox = -1;
    g_editingTextBox = -1;
    g_isDraggingTextBox = false;
    g_isResizingTextBox = false;
    g_resizeHandle = -1;
    for (auto& s : g_signatures) delete s.bitmap;
    g_signatures.clear();
    g_selectedSig    = -1;
    g_isDraggingSig  = false;
    g_isResizingSig  = false;
    g_sigResizeHandle = -1;
    g_shapes.clear();
    g_inkStrokes.clear();
    g_selectedShape       = -1;
    g_selectedInkStroke   = -1;
    g_sidebarVisible = false;
    g_sidebarTab = TAB_BOOKMARKS;
    g_bookmarks.clear();
    g_sidebarScrollY = 0;
    g_thumbScrollY = 0;
    ClearThumbCache();
    ClearPreviewCache();
    FlushRenderQueue();   // 워커 큐 비우기
    g_findHandle = nullptr;
    g_findPage = -1;
    g_findCharIndex = -1;
    g_findCharCount = 0;
    g_findResultIdx = 0;
    g_findResultTotal = 0;
    g_findText[0] = L'\0';
    g_annotPageLoaded.clear();
    g_annotPagePosted.clear();
    g_annotLoadReady = false;
    g_formHandle = nullptr;
    g_formFieldFocused = false;
}

// ─── PDF 폼(FormFill) 초기화/해제 ─────────────────────────────────────────────

static HWND g_formHWnd = nullptr; // FFI_Invalidate 콜백에서 재그리기 요청에 사용

static void FFI_Invalidate_CB(FPDF_FORMFILLINFO* /*info*/,
    FPDF_PAGE /*page*/, double /*left*/, double /*top*/,
    double /*right*/, double /*bottom*/)
{
    if (g_formHWnd)
    {
        InvalidateRect(g_formHWnd, nullptr, FALSE);
    }
}

// FFI_OnFocusChange: PDFium이 폼 필드 포커스 변경을 알릴 때 호출
static void FFI_OnFocusChange_CB(FPDF_FORMFILLINFO* /*info*/,
    FPDF_ANNOTATION annot, int /*page_index*/)
{
    // annot이 nullptr이면 포커스 해제, 아니면 폼 필드 포커스 획득
    g_formFieldFocused = (annot != nullptr);
}

// FFI_SetTimer: PDFium이 커서 깜빡임 등을 위해 타이머 설치를 요청
static int FFI_SetTimer_CB(FPDF_FORMFILLINFO* /*info*/, int uElapse, TimerCallback lpTimerFunc)
{
    if (!g_formTimerHwnd || !lpTimerFunc) return 0;
    int pdfId = g_formTimerNextId++;
    UINT_PTR winId = FORM_TIMER_BASE + (UINT_PTR)pdfId;
    SetTimer(g_formTimerHwnd, winId, (UINT)uElapse, nullptr);
    g_formTimers[pdfId] = { winId, lpTimerFunc };
    return pdfId;
}

// FFI_KillTimer: PDFium이 타이머 해제를 요청
static void FFI_KillTimer_CB(FPDF_FORMFILLINFO* /*info*/, int nTimerID)
{
    auto it = g_formTimers.find(nTimerID);
    if (it != g_formTimers.end())
    {
        if (g_formTimerHwnd)
            KillTimer(g_formTimerHwnd, it->second.winTimerID);
        g_formTimers.erase(it);
    }
}

// FFI_SetCursor: PDFium이 마우스 커서 모양 변경을 요청
static void FFI_SetCursor_CB(FPDF_FORMFILLINFO* /*info*/, int nCursorType)
{
    LPWSTR cursorId = IDC_ARROW;
    switch (nCursorType)
    {
    case FXCT_VBEAM:  cursorId = IDC_IBEAM;  break; // 텍스트 입력 I빔
    case FXCT_HBEAM:  cursorId = IDC_IBEAM;  break;
    case FXCT_HAND:   cursorId = IDC_HAND;   break;
    case FXCT_NESW:   cursorId = IDC_SIZENESW; break;
    case FXCT_NWSE:   cursorId = IDC_SIZENWSE; break;
    default:          cursorId = IDC_ARROW;  break;
    }
    SetCursor(LoadCursor(nullptr, cursorId));
}

// 현재 문서에 대해 폼 환경을 초기화한다.
static void InitFormFill(HWND hWnd)
{
    if (!g_pdfDoc) return;
    if (g_formHandle)
    {
        FPDFDOC_ExitFormFillEnvironment(g_formHandle);
        g_formHandle = nullptr;
    }

    // 폼 필드가 없는 문서는 초기화 생략 → 렌더링/입력 경로 전체 스킵
    if (FPDF_GetFormType(g_pdfDoc) == FORMTYPE_NONE)
        return;

    g_formHWnd = hWnd;
    g_formTimerHwnd = hWnd;

    // FPDF_FORMFILLINFO는 PDFium이 수명 동안 포인터를 참조하므로 static으로 유지
    static FPDF_FORMFILLINFO s_formInfo = {};
    s_formInfo.version = 2;
    s_formInfo.FFI_Invalidate     = FFI_Invalidate_CB;
    s_formInfo.FFI_OnFocusChange  = FFI_OnFocusChange_CB;
    s_formInfo.FFI_SetTimer       = FFI_SetTimer_CB;
    s_formInfo.FFI_KillTimer      = FFI_KillTimer_CB;
    s_formInfo.FFI_SetCursor      = FFI_SetCursor_CB;

    g_formHandle = FPDFDOC_InitFormFillEnvironment(g_pdfDoc, &s_formInfo);
    if (g_formHandle)
    {
        FPDF_SetFormFieldHighlightColor(g_formHandle, FPDF_FORMFIELD_UNKNOWN, 0x00E0EDFF);
        FPDF_SetFormFieldHighlightAlpha(g_formHandle, 0x80);
        FORM_DoDocumentAAction(g_formHandle, FPDFDOC_AACTION_DP);
    }
}

static void DestroyFormFill()
{
    // 남아있는 폼 타이머 모두 해제
    if (g_formTimerHwnd)
    {
        for (auto& kv : g_formTimers)
            KillTimer(g_formTimerHwnd, kv.second.winTimerID);
    }
    g_formTimers.clear();
    g_formTimerNextId = 1;

    if (g_formHandle)
    {
        FPDFDOC_ExitFormFillEnvironment(g_formHandle);
        g_formHandle = nullptr;
    }
}

// 화면 좌표(pageLeft/pageTop 기준 픽셀) → PDF 사용자 공간 좌표 변환
static void FormDeviceToPage(int pgX, int pgY, int renderW, int renderH,
                              double* outPdfX, double* outPdfY)
{
    if (g_pdfPage)
        FPDF_DeviceToPage(g_pdfPage, 0, 0, renderW, renderH,
                          GetEffectiveRotation(g_currentPage), pgX, pgY, outPdfX, outPdfY);
    else
    { *outPdfX = pgX; *outPdfY = pgY; }
}

static void SwitchToTab(HWND hWnd, int tabIndex);
static void CloseTab(HWND hWnd, int tabIndex);
static int  AddNewTab(HWND hWnd);
static void DrawTabBar(HDC hdc, const RECT& clientRect);
static int  HitTestTabBar(int x, int y, bool* closeBtn);
static std::wstring GetTabTitle(const TabState& ts);

// 탭 바 아래 시작 Y 위치
int GetBelowToolbarY()
{
    if (g_fullscreen) return 0;
    return TOOLBAR_HEIGHT + (!g_tabs.empty() ? TAB_BAR_HEIGHT : 0);
}

// 찾기 바 관련 전방 선언은 FindBar.h 참조

// 전방 선언
ATOM             MyRegisterClass(HINSTANCE hInstance);
BOOL             InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

// 전방 선언
ATOM             MyRegisterClass(HINSTANCE hInstance);
BOOL             InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

// ── QuickJump 드롭다운 팝업 ───────────────────────────────────────────────────

static HWND  g_hQJPopup    = nullptr; // 팝업 윈도우 핸들
static HWND  g_hQJEdit     = nullptr; // 검색어 입력 EditBox
static HWND  g_hQJLabelEdit= nullptr; // 이름 입력 EditBox
static HWND  g_hQJAddBtn   = nullptr; // 추가 버튼
static int   g_qjHover     = -1;      // hover 중인 항목 인덱스 (-1 없음)
static bool  g_qjAddHover  = false;   // 추가 버튼 hover 상태
static std::wstring g_qjActiveKeyword;  // 현재 활성(▶▶ 연속검색) 키워드
static int   g_qjBtnHover  = 0;  // 0=없음 1=▶ 2=▶▶ 3=✕
static bool  g_qjCloseHover = false; // 드래그 바 ✕ 닫기 버튼 hover 상태
static POINT g_qjPopupPos  = { -1, -1 }; // 마지막 팝업 위치 (이동 후 기억)

static LRESULT CALLBACK QJAddBtnSubclass(HWND hBtn, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (!g_qjAddHover)
        {
            g_qjAddHover = true;
            InvalidateRect(hBtn, nullptr, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hBtn, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        g_qjAddHover = false;
        InvalidateRect(hBtn, nullptr, FALSE);
        break;
    }
    return DefSubclassProc(hBtn, msg, wParam, lParam);
}

static const int QJ_ITEM_H  = 34;
static const int QJ_HDR_H   = 122; // 입력 영역 높이 (드래그바 20px + 2줄 입력 + 버튼)
static const int QJ_W       = 300;
static const int QJ_PADDING = 8;

static const UINT_PTR QJ_ANIM_TIMER = 0x514A;
static const ULONGLONG QJ_ANIM_MS   = 160;
static bool      s_qjAnimating = false;
static int       s_qjFromX = 0, s_qjToX = 0, s_qjToY = 0;
static ULONGLONG s_qjStart = 0;
static bool      s_qjClosing = false;
static HWND      s_qjClosingWnd = nullptr;

static void QJAnimStep(HWND hWnd)
{
    double t = (double)(GetTickCount64() - s_qjStart) / (double)QJ_ANIM_MS;
    if (t > 1.0) t = 1.0;
    double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    int x = s_qjFromX + (int)((s_qjToX - s_qjFromX) * e);

    RECT wr; GetWindowRect(hWnd, &wr);
    SetWindowPos(hWnd, nullptr, x, s_qjToY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (t >= 1.0)
    {
        KillTimer(hWnd, QJ_ANIM_TIMER);
        s_qjAnimating = false;
        if (s_qjClosing)
        {
            s_qjClosing = false;
            s_qjClosingWnd = nullptr;
            DestroyWindow(hWnd);
            return;
        }
        SetWindowRgn(hWnd, nullptr, TRUE);
        return;
    }
    // 메인 창 오른쪽 밖으로 삐져나오지 않도록 클리핑
    HWND hMain = GetParent(hWnd);
    RECT mr; GetWindowRect(hMain, &mr);
    int visW = (QJ_W < (int)(mr.right - x)) ? QJ_W : (int)(mr.right - x);
    if (visW < 1) visW = 1;
    SetWindowRgn(hWnd, CreateRectRgn(0, 0, visW, wr.bottom - wr.top), TRUE);
}
static void QJPopupClose()
{
    if (g_hQJPopup)
    {
        HWND hMain = GetParent(g_hQJPopup);
        HWND hClosing = g_hQJPopup;
        if (s_qjClosingWnd && s_qjClosingWnd != hClosing)
        {
            DestroyWindow(s_qjClosingWnd);
            s_qjClosingWnd = nullptr;
        }
        if (hMain && IsWindowVisible(hMain))
        {
            RECT cwr, mwr;
            GetWindowRect(hClosing, &cwr);
            GetWindowRect(hMain, &mwr);
            s_qjFromX = cwr.left;
            s_qjToX = (mwr.right > cwr.left) ? (int)mwr.right : cwr.left + QJ_W;
            s_qjToY = cwr.top;
            s_qjStart = GetTickCount64();
            s_qjClosing = true;
            s_qjAnimating = true;
            s_qjClosingWnd = hClosing;
            EnableWindow(hClosing, FALSE);
            SetTimer(hClosing, QJ_ANIM_TIMER, 15, nullptr);
        }
        else
        {
            s_qjAnimating = false;
            DestroyWindow(hClosing);
        }
        g_hQJPopup = nullptr;
        g_quickJumpVisible = false;
        g_qjActiveKeyword.clear();

        // 찾기 바가 열려 있지 않을 때만 검색 하이라이트·상태 초기화
        if (!g_findBarVisible)
        {
            FindClose();
            g_findText[0] = L'\0';
            if (g_hFindEdit) SetWindowTextW(g_hFindEdit, L"");
        }

        if (hMain)
        {
            InvalidateRect(hMain, nullptr, FALSE);
            // 팝업 닫힌 후 메인 창 포커스를 명시적으로 복원
            // (항목 클릭·외부 클릭 등으로 포커스가 어긋나는 문제 방지)
            SetForegroundWindow(hMain);
            SetFocus(hMain);
        }
    }
}

// 검색어로 첫 번째 결과로 이동
static void QJJumpTo(HWND hMainWnd, const std::wstring& keyword)
{
    if (keyword.empty() || !g_pdfDoc) return;
    g_qjActiveKeyword = keyword;
    wcsncpy_s(g_findText, keyword.c_str(), _TRUNCATE);
    if (g_hFindEdit) SetWindowTextW(g_hFindEdit, keyword.c_str());
    FindClose();
    FindNext(hMainWnd, true);
}

// FindClose 없이 다음 결과로 이동
static void QJJumpNext(HWND hMainWnd, const std::wstring& keyword)
{
    if (keyword.empty() || !g_pdfDoc) return;
    // 키워드가 바뀐 경우 처음부터 시작
    if (g_qjActiveKeyword != keyword)
    {
        QJJumpTo(hMainWnd, keyword);
        return;
    }
    wcsncpy_s(g_findText, keyword.c_str(), _TRUNCATE);
    if (g_hFindEdit) SetWindowTextW(g_hFindEdit, keyword.c_str());
    FindNext(hMainWnd, true);
}

static LRESULT CALLBACK QJPopupProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND hMain = GetParent(hWnd);

    switch (msg)
    {
    case WM_CREATE:
    {
        const int lblW  = 44;  // 레이블 너비
        const int editH = 22;
        const int addH  = 24;
        // ── 1줄: 이름 ──
        CreateWindowExW(0, L"STATIC", LS(STR_NAME_LABEL),
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            QJ_PADDING, 28, lblW, editH, hWnd, nullptr, hInst, nullptr);
        g_hQJLabelEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_CENTER,
            QJ_PADDING + lblW + 4, 28, QJ_W - QJ_PADDING * 2 - lblW - 4, editH,
            hWnd, nullptr, hInst, nullptr);
        // ── 2줄: 검색어 ──
        CreateWindowExW(0, L"STATIC", LS(STR_SEARCH_LABEL),
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            QJ_PADDING, 58, lblW, editH, hWnd, nullptr, hInst, nullptr);
        g_hQJEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_CENTER,
            QJ_PADDING + lblW + 4, 58, QJ_W - QJ_PADDING * 2 - lblW - 4, editH,
            hWnd, nullptr, hInst, nullptr);
        // ── 3줄: 추가 버튼 (가로 전체) ──
        g_hQJAddBtn = CreateWindowExW(0, L"BUTTON", LS(STR_QJ_ADD_BTN),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            QJ_PADDING, 88, QJ_W - QJ_PADDING * 2, addH,
            hWnd, (HMENU)1, hInst, nullptr);
        SetWindowSubclass(g_hQJAddBtn, QJAddBtnSubclass, 1, 0);
        // 헤더 컨트롤 폰트 축소
        HFONT hSmallFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HWND hControls[] = { g_hQJLabelEdit, g_hQJEdit, g_hQJAddBtn };
        for (HWND h : hControls)
            SendMessageW(h, WM_SETFONT, (WPARAM)hSmallFont, TRUE);
        // 레이블 STATIC 컨트롤도 동일 폰트 (GetWindow로 순회)
        for (HWND h = GetWindow(hWnd, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT))
        {
            WCHAR cls[16];
            GetClassNameW(h, cls, 16);
            if (_wcsicmp(cls, L"STATIC") == 0)
                SendMessageW(h, WM_SETFONT, (WPARAM)hSmallFont, TRUE);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 1)
        {
            WCHAR labelBuf[128] = {}, kwBuf[128] = {};
            GetWindowTextW(g_hQJLabelEdit, labelBuf, 128);
            GetWindowTextW(g_hQJEdit,      kwBuf,    128);
            if (wcslen(kwBuf) == 0) break;
            if ((int)g_quickJumps.size() >= QUICKJUMP_MAX) break;
            QuickJumpItem item;
            item.keyword = kwBuf;
            item.label   = wcslen(labelBuf) > 0 ? labelBuf : kwBuf;
            g_quickJumps.push_back(std::move(item));
            SaveQuickJumps();
            SetWindowTextW(g_hQJLabelEdit, L"");
            SetWindowTextW(g_hQJEdit,      L"");
            // 팝업 높이 재조정
            RECT cr; GetClientRect(hWnd, &cr);
            int newH = QJ_HDR_H + (int)g_quickJumps.size() * QJ_ITEM_H + QJ_PADDING;
            RECT wr; GetWindowRect(hWnd, &wr);
            SetWindowPos(hWnd, nullptr, 0, 0, QJ_W, newH, SWP_NOMOVE | SWP_NOZORDER);
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        break;
    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlID != 1) break;
        HDC dc = dis->hDC;
        RECT rc = dis->rcItem;
        bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        bool hov     = g_qjAddHover;
        COLORREF bgCol  = pressed ? (g_darkMode ? RGB(40,80,150)  : RGB(160,195,240))
                        : hov     ? (g_darkMode ? RGB(55,95,160)  : RGB(190,215,250))
                                  : (g_darkMode ? RGB(50,60,85)   : RGB(225,232,245));
        COLORREF brdCol = pressed ? (g_darkMode ? RGB(80,130,200) : RGB(100,150,210))
                        : hov     ? (g_darkMode ? RGB(90,140,210) : RGB(120,165,225))
                                  : (g_darkMode ? RGB(70,85,120)  : RGB(180,195,220));
        COLORREF txtCol = g_darkMode ? RGB(210,220,240) : RGB(30,50,100);
        HBRUSH hBr = CreateSolidBrush(bgCol);
        HPEN   hPn = CreatePen(PS_SOLID, 1, brdCol);
        HPEN   oPn = (HPEN)SelectObject(dc, hPn);
        HBRUSH oBr = (HBRUSH)SelectObject(dc, hBr);
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
        SelectObject(dc, oPn); SelectObject(dc, oBr);
        DeleteObject(hPn); DeleteObject(hBr);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, txtCol);
        HFONT hF = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT oF = (HFONT)SelectObject(dc, hF);
        DrawTextW(dc, LS(STR_QJ_ADD_BTN), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oF); DeleteObject(hF);
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1; // 더블버퍼링이 전담하므로 배경 지우기 차단
    case WM_LBUTTONDOWN:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        if (my < 20)
        {
            RECT cr2; GetClientRect(hWnd, &cr2);
            if (mx >= cr2.right - 22 && mx < cr2.right - 2)
            {
                if (!g_findBarVisible && g_findText[0] != L'\0')
                {
                    HWND hMain2 = GetParent(hWnd);
                    bool ok = CleanPopup::Show(hMain2,
                        LS(STR_QJ_CLOSE_TITLE),
                        LS(STR_QJ_CLOSE_MSG),
                        true);
                    if (!ok) return 0;
                }
                QJPopupClose();
                return 0;
            }
            // 드래그 바 영역: OS에 윈도우 이동 위임
            ReleaseCapture();
            SendMessageW(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, lParam);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        int prev     = g_qjHover;
        int prevBtn  = g_qjBtnHover;
        g_qjHover    = -1;
        g_qjBtnHover = 0;
        RECT cr2; GetClientRect(hWnd, &cr2);
        // 드래그 바 ✕ hover
        bool prevClose = g_qjCloseHover;
        g_qjCloseHover = (my < 20 && mx >= cr2.right - 22 && mx < cr2.right - 2);
        if (g_qjCloseHover != prevClose)
        {
            RECT hdrR = { 0, 0, cr2.right, 20 };
            InvalidateRect(hWnd, &hdrR, FALSE);
        }

        const int BTN_W = 20;
        for (int i = 0; i < (int)g_quickJumps.size(); ++i)
        {
            int iy = QJ_HDR_H + i * QJ_ITEM_H;
            if (my >= iy && my < iy + QJ_ITEM_H)
            {
                g_qjHover = i;
                int delX = cr2.right - QJ_PADDING - BTN_W;
                int nxtX = delX - BTN_W - 2;
                int goX  = nxtX - BTN_W - 2;
                if      (mx >= delX && mx < delX + BTN_W) g_qjBtnHover = 3;
                else if (mx >= nxtX && mx < nxtX + BTN_W) g_qjBtnHover = 2;
                else if (mx >= goX  && mx < goX  + BTN_W) g_qjBtnHover = 1;
                break;
            }
        }
        if (g_qjHover != prev || g_qjBtnHover != prevBtn)
        {
            auto invalidateItem = [&](int idx) {
                if (idx < 0) return;
                RECT r = { 0, QJ_HDR_H + idx * QJ_ITEM_H, cr2.right, QJ_HDR_H + (idx + 1) * QJ_ITEM_H };
                InvalidateRect(hWnd, &r, FALSE);
            };
            invalidateItem(prev);
            invalidateItem(g_qjHover);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
    {
        int prev = g_qjHover;
        g_qjHover    = -1;
        g_qjBtnHover = 0;
        bool wasClose = g_qjCloseHover;
        g_qjCloseHover = false;
        RECT cr2; GetClientRect(hWnd, &cr2);
        if (prev >= 0)
        {
            RECT r = { 0, QJ_HDR_H + prev * QJ_ITEM_H, cr2.right, QJ_HDR_H + (prev + 1) * QJ_ITEM_H };
            InvalidateRect(hWnd, &r, FALSE);
        }
        if (wasClose)
        {
            RECT hdrR = { 0, 0, cr2.right, 20 };
            InvalidateRect(hWnd, &hdrR, FALSE);
        }
        break;
    }
    case WM_LBUTTONUP:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        RECT cr2; GetClientRect(hWnd, &cr2);
        const int BTN_W = 20;
        for (int i = 0; i < (int)g_quickJumps.size(); ++i)
        {
            int iy   = QJ_HDR_H + i * QJ_ITEM_H;
            if (my < iy || my >= iy + QJ_ITEM_H) continue;

            int delX = cr2.right - QJ_PADDING - BTN_W;
            int nxtX = delX - BTN_W - 2;
            int goX  = nxtX - BTN_W - 2;

            // ✕ 삭제
            if (mx >= delX && mx < delX + BTN_W)
            {
                std::wstring label = g_quickJumps[i].label;
                wchar_t msgBuf[512];
                _snwprintf_s(msgBuf, _countof(msgBuf), _TRUNCATE,
                    LS(STR_QJ_DEL_MSG_FMT),
                    label.c_str());
                std::wstring msg = msgBuf;
                bool ok = CleanPopup::Show(GetParent(hWnd), LS(STR_QJ_DEL_TITLE), msg, true);
                if (!ok) return 0;
                g_quickJumps.erase(g_quickJumps.begin() + i);
                SaveQuickJumps();
                int newH = QJ_HDR_H + (int)g_quickJumps.size() * QJ_ITEM_H + QJ_PADDING;
                if (newH < QJ_HDR_H + QJ_PADDING) newH = QJ_HDR_H + QJ_PADDING;
                SetWindowPos(hWnd, nullptr, 0, 0, QJ_W, newH, SWP_NOMOVE | SWP_NOZORDER);
                InvalidateRect(hWnd, nullptr, TRUE);
                return 0;
            }
            // ▶▶ 다음 결과
            if (mx >= nxtX && mx < nxtX + BTN_W)
            {
                std::wstring kw = g_quickJumps[i].keyword;
                QJJumpNext(hMain, kw);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            // ▶ 첫 결과
            if (mx >= goX && mx < goX + BTN_W)
            {
                std::wstring kw = g_quickJumps[i].keyword;
                QJJumpTo(hMain, kw);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            // 텍스트 영역 클릭 → ▶ 와 동일
            if (mx >= QJ_PADDING && mx < goX)
            {
                std::wstring kw = g_quickJumps[i].keyword;
                QJJumpTo(hMain, kw);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
        }
        break;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT cr; GetClientRect(hWnd, &cr);

        // 더블버퍼링
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        // 배경
        COLORREF bgColor  = g_darkMode ? RGB(0x28, 0x28, 0x38) : RGB(0xFA, 0xFA, 0xFC);
        COLORREF brdColor = g_darkMode ? RGB(0x45, 0x45, 0x60) : RGB(0xCC, 0xCC, 0xD4);
        HBRUSH bgBr = CreateSolidBrush(bgColor);
        FillRect(memDC, &cr, bgBr);
        DeleteObject(bgBr);
        HPEN brdPen = CreatePen(PS_SOLID, 1, brdColor);
        HPEN oldPen = (HPEN)SelectObject(memDC, brdPen);
        Rectangle(memDC, cr.left, cr.top, cr.right - 1, cr.bottom - 1);
        SelectObject(memDC, oldPen); DeleteObject(brdPen);

        // 드래그 바 (상단 20px) — 약간 다른 배경색
        {
            COLORREF dragBg = g_darkMode ? RGB(0x32, 0x32, 0x46) : RGB(0xEE, 0xEF, 0xF5);
            HBRUSH dragBr = CreateSolidBrush(dragBg);
            RECT dragR = { cr.left, cr.top, cr.right, 20 };
            FillRect(memDC, &dragR, dragBr);
            DeleteObject(dragBr);
            // 드래그 바 하단 구분선
            HPEN dragPen = CreatePen(PS_SOLID, 1, brdColor);
            HPEN oPn2 = (HPEN)SelectObject(memDC, dragPen);
            MoveToEx(memDC, 0, 20, nullptr);
            LineTo(memDC, cr.right, 20);
            SelectObject(memDC, oPn2); DeleteObject(dragPen);
            // 타이틀 텍스트
            HFONT hTitleFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT oTF = (HFONT)SelectObject(memDC, hTitleFont);
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, g_darkMode ? RGB(160, 165, 190) : RGB(100, 105, 130));
            RECT titleR = { QJ_PADDING, 0, cr.right - 24, 20 };
            DrawTextW(memDC, LS(STR_QJ_TITLE), -1, &titleR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            // 오른쪽 ✕ 닫기 버튼
            {
                bool ch = g_qjCloseHover;
                COLORREF closeC = ch ? (g_darkMode ? RGB(255, 100, 100) : RGB(200, 50, 50))
                                     : (g_darkMode ? RGB(150, 150, 170) : RGB(150, 150, 165));
                SetTextColor(memDC, closeC);
                RECT closeR = { cr.right - 22, 0, cr.right - 2, 20 };
                DrawTextW(memDC, L"✕", -1, &closeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            SelectObject(memDC, oTF); DeleteObject(hTitleFont);
        }

        // 구분선 (헤더 아래)
        HPEN sepPen = CreatePen(PS_SOLID, 1, brdColor);
        oldPen = (HPEN)SelectObject(memDC, sepPen);
        MoveToEx(memDC, 0, QJ_HDR_H - 1, nullptr);
        LineTo(memDC, cr.right, QJ_HDR_H - 1);
        SelectObject(memDC, oldPen); DeleteObject(sepPen);

        HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(memDC, hFont);
        SetBkMode(memDC, TRANSPARENT);

        if (g_quickJumps.empty())
        {
            SetTextColor(memDC, g_darkMode ? RGB(140, 140, 160) : RGB(160, 160, 170));
            RECT emptyR = { QJ_PADDING, QJ_HDR_H, cr.right - QJ_PADDING, QJ_HDR_H + QJ_ITEM_H };
            DrawTextW(memDC, LS(STR_NO_QUICKJUMP), -1, &emptyR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        else
        {
            for (int i = 0; i < (int)g_quickJumps.size(); ++i)
            {
                int iy = QJ_HDR_H + i * QJ_ITEM_H;
                RECT itemR = { 0, iy, cr.right, iy + QJ_ITEM_H };

                bool isActive = (!g_qjActiveKeyword.empty() && g_qjActiveKeyword == g_quickJumps[i].keyword);

                // 활성 키워드 행 배경 강조
                if (isActive)
                {
                    HBRUSH actBr = CreateSolidBrush(g_darkMode ? RGB(35, 55, 90) : RGB(215, 230, 255));
                    FillRect(memDC, &itemR, actBr);
                    DeleteObject(actBr);
                }
                // hover 배경 (활성 행 위에 살짝 덧씌움)
                if (i == g_qjHover)
                {
                    HBRUSH hvrBr = CreateSolidBrush(g_darkMode ? RGB(50, 65, 100) : RGB(210, 225, 248));
                    FillRect(memDC, &itemR, hvrBr);
                    DeleteObject(hvrBr);
                }

                // 버튼 영역 계산 (오른쪽부터: ✕20 | ▶▶20 | ▶20)
                const int BTN_W = 20;
                int delX  = cr.right - QJ_PADDING - BTN_W;           // ✕
                int nxtX  = delX  - BTN_W - 2;                       // ▶▶
                int goX   = nxtX  - BTN_W - 2;                       // ▶

                // 이름 텍스트 (버튼 3개 왼쪽까지)
                SetTextColor(memDC, g_darkMode ? RGB(220, 220, 235) : RGB(30, 30, 40));
                RECT lblR = { QJ_PADDING + 4, iy, goX - 4, iy + QJ_ITEM_H };
                DrawTextW(memDC, g_quickJumps[i].label.c_str(), -1, &lblR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                bool rowHov = (i == g_qjHover);

                // ▶ 버튼
                {
                    bool h = rowHov && g_qjBtnHover == 1;
                    COLORREF c = h ? (g_darkMode ? RGB(100, 180, 100) : RGB(30, 140, 30))
                                   : (g_darkMode ? RGB(130, 200, 130) : RGB(80, 160, 80));
                    SetTextColor(memDC, rowHov ? c : (g_darkMode ? RGB(100,130,100) : RGB(160,190,160)));
                    RECT goR = { goX, iy, goX + BTN_W, iy + QJ_ITEM_H };
                    DrawTextW(memDC, L"▶", -1, &goR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }

                // ▶▶ 버튼 (활성 키워드와 동일한 경우 강조)
                {
                    bool h = rowHov && g_qjBtnHover == 2;
                    COLORREF c = h        ? (g_darkMode ? RGB(100, 160, 255) : RGB(20, 100, 220))
                               : isActive ? (g_darkMode ? RGB(130, 170, 255) : RGB(60, 130, 230))
                                          : (g_darkMode ? RGB(100, 110, 150) : RGB(160, 170, 200));
                    SetTextColor(memDC, rowHov ? c : (g_darkMode ? RGB(80,90,120) : RGB(180,185,210)));
                    RECT nxtR = { nxtX, iy, nxtX + BTN_W, iy + QJ_ITEM_H };
                    DrawTextW(memDC, L"▶▶", -1, &nxtR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }

                // ✕ 삭제 버튼
                {
                    bool h = rowHov && g_qjBtnHover == 3;
                    SetTextColor(memDC, h
                        ? (g_darkMode ? RGB(255, 100, 100) : RGB(200, 50, 50))
                        : (rowHov ? (g_darkMode ? RGB(180,100,100) : RGB(210,140,140))
                                  : (g_darkMode ? RGB(130,130,150) : RGB(180,180,190))));
                    RECT delR = { delX, iy, delX + BTN_W, iy + QJ_ITEM_H };
                    DrawTextW(memDC, L"✕", -1, &delR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }

                // 항목 구분선
                if (i < (int)g_quickJumps.size() - 1)
                {
                    HPEN linePen = CreatePen(PS_SOLID, 1, g_darkMode ? RGB(50, 50, 68) : RGB(220, 220, 228));
                    oldPen = (HPEN)SelectObject(memDC, linePen);
                    MoveToEx(memDC, QJ_PADDING, iy + QJ_ITEM_H - 1, nullptr);
                    LineTo(memDC, cr.right - QJ_PADDING, iy + QJ_ITEM_H - 1);
                    SelectObject(memDC, oldPen); DeleteObject(linePen);
                }
            }
        }

        SelectObject(memDC, oldFont);
        DeleteObject(hFont);

        // rcPaint 영역만 화면에 전송
        BitBlt(hdc,
            ps.rcPaint.left, ps.rcPaint.top,
            ps.rcPaint.right  - ps.rcPaint.left,
            ps.rcPaint.bottom - ps.rcPaint.top,
            memDC,
            ps.rcPaint.left, ps.rcPaint.top,
            SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wParam == QJ_ANIM_TIMER) { QJAnimStep(hWnd); return 0; }
        break;
    case WM_MOVE:
    {
        if (s_qjAnimating) break;
        RECT wr; GetWindowRect(hWnd, &wr);
        g_qjPopupPos = { wr.left, wr.top };
        break;
    }
    case WM_SETCURSOR:
    {
        // 팝업 전체 영역에서 커서를 명시적으로 설정해 EDIT 자식 컨트롤의
        // IDC_IBEAM이 팝업 배경까지 번지는 것을 방지
        POINT pt; GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        RECT cr2; GetClientRect(hWnd, &cr2);

        LPWSTR cursor = IDC_ARROW;
        if (pt.y < 20)
        {
            // 드래그 바: ✕ 버튼은 기본 화살표, 나머지는 이동 커서
            if (pt.x >= cr2.right - 22 && pt.x < cr2.right - 2)
                cursor = IDC_ARROW;
            else
                cursor = IDC_SIZEALL;
        }
        else if (pt.y >= QJ_HDR_H)
        {
            // 항목 행: ▶ / ▶▶ / ✕ 버튼 위는 화살표, 텍스트 영역도 화살표
            cursor = IDC_ARROW;
        }
        SetCursor(LoadCursorW(nullptr, cursor));
        return TRUE;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowQuickJumpPopup(HWND hMainWnd)
{
    if (g_hQJPopup) { QJPopupClose(); return; }

    static bool s_registered = false;
    if (!s_registered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = QJPopupProc;
        wc.hInstance     = hInst;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"QJPopupClass";
        wc.style         = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        s_registered = true;
    }

    RECT cr; GetClientRect(hMainWnd, &cr);
    RECT btnR = GetQuickJumpToolbarBtnRect(cr);
    POINT pt = { btnR.left, btnR.bottom + 2 };
    ClientToScreen(hMainWnd, &pt);

    // 이전에 사용자가 이동한 위치가 있으면 그 위치 사용
    if (g_qjPopupPos.x != -1 && g_qjPopupPos.y != -1)
    {
        pt = g_qjPopupPos;
    }

    int itemCount = (int)g_quickJumps.size();
    int popupH = QJ_HDR_H + (itemCount > 0 ? itemCount * QJ_ITEM_H : QJ_ITEM_H) + QJ_PADDING;

    g_quickJumpVisible = true;
    if (s_qjClosingWnd)
    {
        DestroyWindow(s_qjClosingWnd);
        s_qjClosingWnd = nullptr;
        s_qjClosing = false;
    }
    RECT mainWr; GetWindowRect(hMainWnd, &mainWr);
    s_qjToX = pt.x;
    s_qjToY = pt.y;
    s_qjFromX = ((int)mainWr.right > pt.x) ? (int)mainWr.right : pt.x;
    s_qjAnimating = true;
    s_qjStart = GetTickCount64();
    g_hQJPopup = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"QJPopupClass", L"",
        WS_POPUP,
        s_qjFromX, pt.y, QJ_W, popupH,
        hMainWnd, nullptr, hInst, nullptr);

    if (g_hQJPopup)
    {
        QJAnimStep(g_hQJPopup);
        ShowWindow(g_hQJPopup, SW_SHOWNOACTIVATE);
        SetTimer(g_hQJPopup, QJ_ANIM_TIMER, 15, nullptr);
    }
    else
        s_qjAnimating = false;

    // 팝업 생성 후 메인 창 포커스 유지 (NOACTIVATE가 무시되는 환경 대비)
    if (g_hQJPopup)
    {
        SetForegroundWindow(hMainWnd);
        SetFocus(hMainWnd);
    }
}



void  ClosePDF();
static void  PrintDocument(HWND hWnd);
static void  SaveDocument(HWND hWnd, bool saveAs);
void  OpenFileFromPath(HWND hWnd, const std::wstring& path, bool showSpinner);
static void  OpenPDFFile(HWND hWnd);
void  LoadPage(HWND hWnd, int pageIndex);
static void  RenderPage(HWND hWnd, HDC hdc, const RECT& clientRect);
static RECT  GetSSArrowRect(const RECT& cr, bool left);
static RECT  GetSSCloseRect(const RECT& cr);
void  UpdateTitle(HWND hWnd);
void  UpdateScrollBars(HWND hWnd);
void  ZoomTo(HWND hWnd, float newZoom);
void  FitToWidth(HWND hWnd);
void  FitToPage(HWND hWnd);
void  NavigatePage(HWND hWnd, int delta);
void  HandleScroll(HWND hWnd, int bar, WPARAM wParam);
void  EnterFullscreen(HWND hWnd);
void  ExitFullscreen(HWND hWnd);
void  ToggleSlideshow(HWND hWnd);
static void  ShowContextMenu(HWND hWnd, POINT ptScreen);

// Toolbar 함수들은 Toolbar.h 참조
RECT  GetViewportRect(const RECT& clientRect);
void  RecalcPageLayouts();
int   GetMostVisiblePage(const RECT& viewport);
void  GetPageScreenPos(int pageIndex, const RECT& viewport, int* outLeft, int* outTop);
void  UpdateActivePageFromScroll(HWND hWnd);
void  ScrollToPage(HWND hWnd, int pageIndex);
static int   HitTestPageAtPoint(int screenX, int screenY, const RECT& vp, double* outPdfX, double* outPdfY);

// 사이드바
static void  RunAnnotLoad(HWND hWnd);
static void  RunAnnotLoadBatch(HWND hWnd, int startPage, int endPage); // [startPage, endPage)
static void  GetVisiblePageRange(HWND hWnd, int& first, int& last);   // 현재 화면에 보이는 페이지 범위
static void  TriggerAnnotLoadForVisiblePages(HWND hWnd);               // 미로드 가시 페이지 Post
static void  LoadHighlightAnnotations(AnnotLoadResult* out = nullptr, FPDF_DOCUMENT doc = nullptr);
static void  ExtractBookmarks();
static void  LoadUserBookmarks();
static void  SaveUserBookmarks();
static void  AddUserBookmark(HWND hWnd);
static void  DeleteUserBookmark(HWND hWnd, int idx);
static void  DrawSidebar(HDC hdc, const RECT& clientRect);
static int   SidebarHitTest(int x, int y, const RECT& clientRect);
static bool  IsBookmarkVisible(int idx);
static void  DrawSidebarThumbnails(HDC hdc, const RECT& clientRect);
static int   ThumbnailHitTest(int x, int y, const RECT& clientRect);

// 메모
static void  DrawNotes(HDC hdc, const RECT& clientRect);
static int   HitTestNote(int screenX, int screenY, const RECT& vp);
static void  AddNoteAtPdfPos(HWND hWnd, int pageIndex, double pdfX, double pdfY);
static void  LoadNoteAnnotations(AnnotLoadResult* out = nullptr, FPDF_DOCUMENT doc = nullptr);

// 텍스트 박스
static void  DrawTextBoxes(HDC hdc, const RECT& clientRect);
static int   HitTestTextBox(int screenX, int screenY, const RECT& vp);
static int   HitTestTextBoxHandle(const RECT& boxScreenRect, int screenX, int screenY);
static void  AddTextBoxAtPdfPos(HWND hWnd, int pageIndex, double pdfX, double pdfY);
static void  CommitTextBoxEdit();
static void  StartTextBoxEdit(HWND hWnd, int tbIdx);

// 서명
static void  DrawSignatures(HDC hdc, const RECT& clientRect);
static RECT  SigScreenRect(const SignatureItem& sig);
static int   HitTestSigHandle(const RECT& sr, int sx, int sy);
static int   HitTestSignature(int screenX, int screenY, const RECT& vp);
static void  LoadTextBoxAnnotations(AnnotLoadResult* out = nullptr, FPDF_DOCUMENT doc = nullptr);
static void  LoadSignatureAnnotations(AnnotLoadResult* out = nullptr, FPDF_DOCUMENT doc = nullptr);
static void  LoadShapeAnnotations(AnnotLoadResult* out = nullptr, FPDF_DOCUMENT doc = nullptr);
static void  LoadInkAnnotations(AnnotLoadResult* out = nullptr, FPDF_DOCUMENT doc = nullptr);
static int   GetThumbnailTotalHeight(const RECT& clientRect);
static void  GetThumbnailGeometry(int pageIdx, const RECT& clientRect, int scrollY, int* outX, int* outY, int* outW, int* outH);

// PdfToScreen 인라인 함수는 PlainPDFViewer.h 참조

static inline void ScreenToPdf(int screenX, int screenY, double pdfW, double pdfH,
    int pgLeft, int pgTop, float zoom, int rotation,
    double* outPdfX, double* outPdfY)
{
    double sx = (screenX - pgLeft) / (double)zoom;
    double sy = (screenY - pgTop)  / (double)zoom;
    switch (rotation) {
    default:
    case 0: *outPdfX = sx;         *outPdfY = pdfH - sy;   break;
    case 1: *outPdfX = sy;         *outPdfY = sx;           break;
    case 2: *outPdfX = pdfW - sx;  *outPdfY = sy;           break;
    case 3: *outPdfX = pdfH - sy;  *outPdfY = pdfW - sx;   break;
    }
}

bool         g_gotoModalDone = false;

// ToolbarButton struct/g_toolbarButtons/g_toolbarButtonCount 는 Toolbar.h / Toolbar.cpp 참조

// ─── PDF 헬퍼 ─────────────────────────────────────────────────────────────────

void ClosePDF()
{
    FindClose();
    if (g_textPage) { FPDFText_ClosePage(g_textPage); g_textPage = nullptr; }
    g_hasSelection = g_isSelecting = false;
    g_selCharStart = g_selCharEnd = -1;
    g_selRectCache.invalidate();
    g_selPage = -1;
    g_selectedHighlight = -1;
    g_underlines.clear();
    g_selectedUnderline = -1;
    g_strikeouts.clear();
    g_selectedStrikeout = -1;
    g_squigglies.clear();
    g_selectedSquiggly = -1;
    g_notes.clear();
    g_selectedNote = -1;
    g_hoverNote = -1;
    g_shapes.clear();
    g_inkStrokes.clear();
    g_selectedShape     = -1;
    g_selectedInkStroke = -1;

    if (g_pdfPage) { FPDF_ClosePage(g_pdfPage); g_pdfPage = nullptr; }
    if (g_pdfDoc)  { FPDF_CloseDocument(g_pdfDoc); g_pdfDoc = nullptr; }
    g_currentPage = 0;
    g_pageCount = 0;
    g_scrollX = 0;
    g_scrollY = 0;
    g_zoom = 1.0f;
    g_rotation = 0;
    g_pageRot.clear();
    g_darkMode = false;
    g_autoScrolling = false;
    g_filePath.clear();
    g_pageLayouts.clear();
    g_totalDocHeight = 0;
    g_maxPageWidth = 0;
    ClearPageCache();
    ClearThumbCache();
    InvalidateToolbarBaseCache();
    InvalidateStatusBarCache();
}

static std::wstring GetTabTitle(const TabState& ts)
{
    if (ts.filePath.empty()) return LS(STR_NEW_TAB);
    const size_t pos = ts.filePath.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? ts.filePath.substr(pos + 1) : ts.filePath;
}

static void SwitchToTab(HWND hWnd, int tabIndex)
{
    if (tabIndex < 0 || tabIndex >= (int)g_tabs.size() || tabIndex == g_activeTab)
        return;

    // 현재 탭 저장
    if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
        SaveTabState(g_tabs[g_activeTab]);

    // 전역 비우기 (리소스 해제 않고)
    ClearGlobalsWithoutClose();

    // 새 탭 복원
    g_activeTab = tabIndex;
    RestoreTabState(g_tabs[g_activeTab]);

    // 찾기 에디트 텍스트 동기화
    if (g_hFindEdit)
        SetWindowTextW(g_hFindEdit, g_findText);
    UpdateFindLabel();

    UpdateTitle(hWnd);
    UpdateScrollBars(hWnd);
    InvalidateRect(hWnd, nullptr, TRUE);
}

static void CloseTab(HWND hWnd, int tabIndex)
{
    if (tabIndex < 0 || tabIndex >= (int)g_tabs.size()) return;

    // 닫으려는 탭이 활성 탭이면 상태 저장 후 정리
    if (tabIndex == g_activeTab)
    {
        // 현재 전역에 있는 리소스 해제
        FindClose();
        if (g_textPage) { FPDFText_ClosePage(g_textPage); g_textPage = nullptr; }
        if (g_pdfPage)
        {
            if (g_formHandle)
            {
                FORM_DoPageAAction(g_pdfPage, g_formHandle, FPDFPAGE_AACTION_CLOSE);
                FORM_OnBeforeClosePage(g_pdfPage, g_formHandle);
            }
            FPDF_ClosePage(g_pdfPage); g_pdfPage = nullptr;
        }
        DestroyFormFill();
        if (g_pdfDoc) { FPDF_CloseDocument(g_pdfDoc); g_pdfDoc = nullptr; }
        ClearPageCache();
        ClearThumbCache();
    }
    else
    {
        // 비활성 탭의 리소스 해제
        auto& ts = g_tabs[tabIndex];
        if (ts.findHandle) { FPDFText_FindClose(ts.findHandle); ts.findHandle = nullptr; }
        if (ts.textPage) { FPDFText_ClosePage(ts.textPage); ts.textPage = nullptr; }
        if (ts.pdfPage)
        {
            if (ts.formHandle)
            {
                FORM_DoPageAAction(ts.pdfPage, ts.formHandle, FPDFPAGE_AACTION_CLOSE);
                FORM_OnBeforeClosePage(ts.pdfPage, ts.formHandle);
            }
            FPDF_ClosePage(ts.pdfPage); ts.pdfPage = nullptr;
        }
        if (ts.formHandle) { FPDFDOC_ExitFormFillEnvironment(ts.formHandle); ts.formHandle = nullptr; }
        if (ts.pdfDoc) { FPDF_CloseDocument(ts.pdfDoc); ts.pdfDoc = nullptr; }
        for (auto& e : ts.pageCache)
            if (e.hBmp) { DeleteObject(e.hBmp); e.hBmp = nullptr; }
    }

    g_tabs.erase(g_tabs.begin() + tabIndex);

    if (g_tabs.empty())
    {
        // 모든 탭 닫힘 → 빈 상태 유지 (새 탭 생성 안 함)
        ClearGlobalsWithoutClose();
        g_activeTab = -1;
    }
    else if (tabIndex == g_activeTab)
    {
        // 활성 탭 닫힘 → 옆 탭으로 이동
        g_activeTab = std::min(tabIndex, (int)g_tabs.size() - 1);
        ClearGlobalsWithoutClose();
        RestoreTabState(g_tabs[g_activeTab]);
    }
    else if (tabIndex < g_activeTab)
    {
        g_activeTab--;
    }

    // 찾기 에디트 텍스트 동기화
    if (g_hFindEdit)
        SetWindowTextW(g_hFindEdit, g_findText);
    UpdateFindLabel();

    UpdateTitle(hWnd);
    UpdateScrollBars(hWnd);
    InvalidateRect(hWnd, nullptr, TRUE);
}

static int AddNewTab(HWND hWnd)
{
    // 현재 탭 저장
    if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
        SaveTabState(g_tabs[g_activeTab]);

    // 찾기 패널 숨기기
    if (g_findBarVisible) HideFindBar(hWnd);

    ClearGlobalsWithoutClose();
    g_tabs.push_back(TabState{});
    g_activeTab = (int)g_tabs.size() - 1;

    UpdateTitle(hWnd);
    UpdateScrollBars(hWnd);
    InvalidateRect(hWnd, nullptr, TRUE);
    return g_activeTab;
}

static RECT GetTabRect(int tabIndex, const RECT& clientRect)
{
    int y = TOOLBAR_HEIGHT;
    int x = tabIndex * TAB_WIDTH;
    // '+' 버튼은 마지막 탭 뒤에
    return { x, y, x + TAB_WIDTH, y + TAB_BAR_HEIGHT };
}

static int HitTestTabBar(int x, int y, bool* closeBtn)
{
    if (closeBtn) *closeBtn = false;
    if (g_tabs.empty()) return -1;
    if (y < TOOLBAR_HEIGHT || y >= TOOLBAR_HEIGHT + TAB_BAR_HEIGHT) return -1;

    // '+' 새 탭 버튼
    int plusX = (int)g_tabs.size() * TAB_WIDTH;
    if (x >= plusX && x < plusX + 30)
        return -2;  // 새 탭 버튼

    int tabIdx = x / TAB_WIDTH;
    if (tabIdx < 0 || tabIdx >= (int)g_tabs.size()) return -1;

    // 닫기 버튼 영역 (탭 오른쪽 끝)
    int closeLeft = (tabIdx + 1) * TAB_WIDTH - TAB_CLOSE_SIZE - 6;
    int closeTop = TOOLBAR_HEIGHT + (TAB_BAR_HEIGHT - TAB_CLOSE_SIZE) / 2;
    if (x >= closeLeft && x < closeLeft + TAB_CLOSE_SIZE &&
        y >= closeTop && y < closeTop + TAB_CLOSE_SIZE)
    {
        if (closeBtn) *closeBtn = true;
    }

    return tabIdx;
}

static void DrawTabBar(HDC hdc, const RECT& clientRect)
{
    if (g_tabs.empty()) return;

    int y = TOOLBAR_HEIGHT;
    RECT barRect = { 0, y, clientRect.right, y + TAB_BAR_HEIGHT };

    // 배경
    HBRUSH bgBr = CreateSolidBrush(RGB(229, 230, 238));
    FillRect(hdc, &barRect, bgBr);
    DeleteObject(bgBr);

    // 하단 구분선
    HPEN linePen = CreatePen(PS_SOLID, 1, RGB(218, 219, 228));
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, linePen));
    MoveToEx(hdc, 0, y + TAB_BAR_HEIGHT - 1, nullptr);
    LineTo(hdc, clientRect.right, y + TAB_BAR_HEIGHT - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(linePen);

    HFONT hFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFont));
    SetBkMode(hdc, TRANSPARENT);

    for (int i = 0; i < (int)g_tabs.size(); i++)
    {
        int tx = i * TAB_WIDTH;
        RECT tr = { tx, y, tx + TAB_WIDTH, y + TAB_BAR_HEIGHT };
        bool active = (i == g_activeTab);
        bool hover = (i == g_tabHover);

        // 탭 배경
        COLORREF tabBg = active ? RGB(246, 246, 252)
                       : hover  ? RGB(234, 234, 242)
                                : RGB(229, 230, 238);
        HBRUSH tabBr = CreateSolidBrush(tabBg);
        FillRect(hdc, &tr, tabBr);
        DeleteObject(tabBr);

        // 활성 탭 상단 액센트
        if (active)
        {
            RECT accent = { tx + 1, y, tx + TAB_WIDTH - 1, y + 2 };
            HBRUSH acBr = CreateSolidBrush(RGB(0, 103, 192));
            FillRect(hdc, &accent, acBr);
            DeleteObject(acBr);
        }

        // 탭 사이 구분선
        if (!active)
        {
            HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(210, 211, 222));
            HPEN prevPen = static_cast<HPEN>(SelectObject(hdc, sepPen));
            MoveToEx(hdc, tx + TAB_WIDTH - 1, y + 4, nullptr);
            LineTo(hdc, tx + TAB_WIDTH - 1, y + TAB_BAR_HEIGHT - 4);
            SelectObject(hdc, prevPen);
            DeleteObject(sepPen);
        }

        // 제목 텍스트 (닫기 버튼 영역 제외)
        std::wstring title = GetTabTitle(g_tabs[i]);
        if (title.length() > 18) title = title.substr(0, 16) + L"..";
        RECT textRc = { tx + 8, y, tx + TAB_WIDTH - TAB_CLOSE_SIZE - 10, y + TAB_BAR_HEIGHT };
        SetTextColor(hdc, active ? RGB(20, 20, 30) : RGB(90, 92, 108));
        DrawTextW(hdc, title.c_str(), -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // 닫기 버튼 (×) - GDI+로 둥근 호버 효과
        int closeLeft = tx + TAB_WIDTH - TAB_CLOSE_SIZE - 6;
        int closeTop = y + (TAB_BAR_HEIGHT - TAB_CLOSE_SIZE) / 2;
        RECT closeBtnRc = { closeLeft, closeTop, closeLeft + TAB_CLOSE_SIZE, closeTop + TAB_CLOSE_SIZE };

        bool closeHover = (i == g_tabCloseHover);
        bool showClose  = closeHover || active || hover;

        if (closeHover) {
            Gdiplus::Graphics gx(hdc);
            gx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            float cx = (float)closeLeft, cy2 = (float)closeTop;
            float sz = (float)TAB_CLOSE_SIZE;
            Gdiplus::SolidBrush hBr(Gdiplus::Color(210, 220, 60, 40));
            gx.FillEllipse(&hBr, cx + 1.f, cy2 + 1.f, sz - 2.f, sz - 2.f);
        } else if (showClose) {
            Gdiplus::Graphics gx(hdc);
            gx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            float cx = (float)closeLeft, cy2 = (float)closeTop;
            float sz = (float)TAB_CLOSE_SIZE;
            Gdiplus::SolidBrush hBr(Gdiplus::Color(45, 30, 30, 60));
            gx.FillEllipse(&hBr, cx + 1.f, cy2 + 1.f, sz - 2.f, sz - 2.f);
        }

        if (showClose) {
            // 대각선 두 줄로 X를 직접 그려 픽셀 단위 완벽 중앙 정렬
            Gdiplus::Graphics gx(hdc);
            gx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Color clr = closeHover ? Gdiplus::Color(255, 255, 255, 255)
                                            : Gdiplus::Color(255, 100, 105, 125);
            Gdiplus::Pen pen(clr, 1.5f);
            pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
            float cx = (float)(closeBtnRc.left + closeBtnRc.right)  * 0.5f;
            float cy = (float)(closeBtnRc.top  + closeBtnRc.bottom) * 0.5f;
            const float hs = 4.0f;
            gx.DrawLine(&pen, cx - hs, cy - hs, cx + hs, cy + hs);
            gx.DrawLine(&pen, cx + hs, cy - hs, cx - hs, cy + hs);
        }
    }

    // '+' 새 탭 버튼 — 정방형 rect로 원형 배경, DrawLine으로 정확히 중앙 정렬
    int plusX = (int)g_tabs.size() * TAB_WIDTH;
    const int plusSz = TAB_BAR_HEIGHT - 8;  // 22px (정방형)
    int plusTop  = y + (TAB_BAR_HEIGHT - plusSz) / 2;
    RECT plusRc = { plusX + 4, plusTop, plusX + 4 + plusSz, plusTop + plusSz };
    {
        Gdiplus::Graphics gx(hdc);
        gx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        float pw = (float)plusSz;
        if (g_tabPlusHover)
        {
            Gdiplus::SolidBrush hBr(Gdiplus::Color(55, 30, 30, 80));
            gx.FillEllipse(&hBr, (float)plusRc.left, (float)plusTop, pw, pw);
        }
        // + 기호를 대각선 없이 수직/수평 두 선으로 직접 그리기
        Gdiplus::Pen pen(Gdiplus::Color(255, 90, 95, 115), 1.5f);
        pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        float cx = (float)(plusRc.left + plusRc.right)  * 0.5f;
        float cy = (float)(plusRc.top  + plusRc.bottom) * 0.5f;
        const float hs = 4.0f;
        gx.DrawLine(&pen, cx - hs, cy,        cx + hs, cy);        // 수평
        gx.DrawLine(&pen, cx,      cy - hs,   cx,      cy + hs);   // 수직
    }

    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
}

// ─── 텍스트 추출 및 뷰어는 TextViewer.h / TextViewer.cpp 참조 ─────────────────

// ─── 인쇄 ─────────────────────────────────────────────────────────────────────

// ─── 인쇄 ─────────────────────────────────────────────────────────────────────

// 워커 스레드에 전달할 인쇄 작업 데이터
struct PrintJobPage
{
    std::vector<BYTE> pixels; // BGRx 픽셀 데이터
    int renderW = 0;
    int renderH = 0;
    int offsetX = 0;
    int offsetY = 0;
};

struct PrintJobData
{
    HDC        hPrintDC = nullptr;
    HGLOBAL    hDevMode = nullptr;
    HGLOBAL    hDevNames = nullptr;
    std::wstring jobName;
    int        printW = 0;
    int        printH = 0;
    int        copies = 1;
    std::vector<PrintJobPage> pages;
};

static DWORD WINAPI PrintWorkerThread(LPVOID param)
{
    PrintJobData* job = reinterpret_cast<PrintJobData*>(param);

    DOCINFOW di = {};
    di.cbSize    = sizeof(di);
    di.lpszDocName = job->jobName.c_str();

    bool ok = (StartDocW(job->hPrintDC, &di) > 0);
    if (ok)
    {
        int copies = (job->copies > 0) ? job->copies : 1;
        for (int c = 0; c < copies && ok; ++c)
        {
        for (const auto& pg : job->pages)
        {
            if (StartPage(job->hPrintDC) <= 0) { ok = false; break; }

            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth       = pg.renderW;
            bmi.bmiHeader.biHeight      = -pg.renderH;
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            // SetDIBitsToDevice는 1:1 복사라 프린터 해상도에서 너무 작게 출력됨
            // StretchDIBits로 출력 영역(offsetX,offsetY,renderW,renderH)에 맞게 늘려 인쇄
            StretchDIBits(job->hPrintDC,
                pg.offsetX, pg.offsetY,
                pg.renderW, pg.renderH,
                0, 0,
                pg.renderW, pg.renderH,
                pg.pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);

            EndPage(job->hPrintDC);
        }
        } // copies
        if (ok)
            EndDoc(job->hPrintDC);
        else
            AbortDoc(job->hPrintDC);
    }

    if (!ok)
    {
        MessageBoxW(nullptr,
            L"\xC778\xC1C4 \xC791\xC5C5\xC744 \xC2DC\xC791\xD558\xC9C0 \xBABB\xD588\xC2B5\xB2C8\xB2E4. \xD504\xB9B0\xD130 \xC0C1\xD0DC\xB97C \xD655\xC778\xD574 \xC8FC\xC138\xC694.",
            L"\xC778\xC1C4 \xC624\xB958", MB_OK | MB_ICONERROR);
    }

    DeleteDC(job->hPrintDC);
    if (job->hDevMode)  GlobalFree(job->hDevMode);
    if (job->hDevNames) GlobalFree(job->hDevNames);
    delete job;
    return 0;
}

static void PrintDocument(HWND hWnd)
{
    if (!g_pdfDoc || g_pageCount <= 0) return;

    PrintDialogResult pdr;
    if (!ShowPrintDialog(hWnd, g_pdfDoc, g_pageCount, g_currentPage, g_rotation, g_pdfiumMtx, pdr) || !pdr.hDC)
    {
        if (pdr.hDC) DeleteDC(pdr.hDC);
        return;
    }

    HDC hPrintDC = pdr.hDC;

    int fromPage = std::clamp(pdr.fromPage, 0, g_pageCount - 1);
    int toPage   = std::clamp(pdr.toPage,   0, g_pageCount - 1);
    int copies   = std::max(1, pdr.copies);

    int dpiX   = GetDeviceCaps(hPrintDC, LOGPIXELSX);
    int dpiY   = GetDeviceCaps(hPrintDC, LOGPIXELSY);
    int printW = GetDeviceCaps(hPrintDC, HORZRES);
    int printH = GetDeviceCaps(hPrintDC, VERTRES);

    // UI 스레드에서 PDFium으로 모든 페이지를 미리 렌더링
    PrintJobData* job = new PrintJobData();
    job->hPrintDC  = hPrintDC;
    job->hDevMode  = nullptr;
    job->hDevNames = nullptr;
    job->printW    = printW;
    job->printH    = printH;
    job->copies    = copies;

    const size_t pos = g_filePath.find_last_of(L"\\/");
    job->jobName = (pos != std::wstring::npos) ? g_filePath.substr(pos + 1) : g_filePath;

    for (int i = fromPage; i <= toPage; ++i)
    {
        // PDFium은 스레드 안전하지 않으므로 백그라운드 렌더 워커와 동시에 호출되지 않도록 직렬화
        std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);

        FPDF_PAGE page = FPDF_LoadPage(g_pdfDoc, i);
        if (!page) continue;

        double pdfW = FPDF_GetPageWidth(page);
        double pdfH = FPDF_GetPageHeight(page);
        bool swapped = (g_rotation == 1 || g_rotation == 3);
        if (swapped) std::swap(pdfW, pdfH);

        if (pdfW <= 0.0 || pdfH <= 0.0 || printW <= 0 || printH <= 0
            || dpiX <= 0 || dpiY <= 0)
        {
            FPDF_ClosePage(page);
            continue;
        }

        double scaleX = (double)printW / (pdfW * dpiX / 72.0);
        double scaleY = (double)printH / (pdfH * dpiY / 72.0);
        double scale  = pdr.fitToPage ? std::min(scaleX, scaleY) : 1.0;

        int renderW = (int)(pdfW * dpiX / 72.0 * scale);
        int renderH = (int)(pdfH * dpiY / 72.0 * scale);

        if (renderW <= 0 || renderH <= 0)
        {
            FPDF_ClosePage(page);
            continue;
        }

        FPDF_BITMAP fpdfBmp = FPDFBitmap_Create(renderW, renderH, 0);
        if (fpdfBmp)
        {
            FPDFBitmap_FillRect(fpdfBmp, 0, 0, renderW, renderH, 0xFFFFFFFF);
            FPDF_RenderPageBitmap(fpdfBmp, page, 0, 0, renderW, renderH, g_rotation,
                FPDF_ANNOT | FPDF_LCD_TEXT | FPDF_PRINTING);
        }
        if (fpdfBmp)
        {
            const BYTE* src = reinterpret_cast<const BYTE*>(FPDFBitmap_GetBuffer(fpdfBmp));
            const size_t stride = (size_t)FPDFBitmap_GetStride(fpdfBmp);

            PrintJobPage pg;
            pg.renderW = renderW;
            pg.renderH = renderH;
            pg.offsetX = (printW - renderW) / 2;
            pg.offsetY = (printH - renderH) / 2;
            pg.pixels.resize((size_t)renderW * renderH * 4);
            for (int row = 0; row < renderH; ++row)
                memcpy(pg.pixels.data() + (size_t)row * renderW * 4,
                       src + row * stride, (size_t)renderW * 4);

            job->pages.push_back(std::move(pg));
            FPDFBitmap_Destroy(fpdfBmp);
        }
        FPDF_ClosePage(page);
    }

    // GDI 인쇄 전송만 워커 스레드에서 수행 (UI 블로킹 없음)
    if (job->pages.empty())
    {
        // 모든 페이지 렌더링이 실패한 경우 — 인쇄 작업을 보내도 빈 페이지만 출력되므로 사전에 차단
        MessageBoxW(hWnd, L"\xC778\xC1C4\xD560 \xD398\xC774\xC9C0\xB97C \xB80C\xB354\xB9C1\xD558\xC9C0 \xBABB\xD588\xC2B5\xB2C8\xB2E4.",
            L"\xC778\xC1C4 \xC624\xB958", MB_OK | MB_ICONERROR);
        DeleteDC(job->hPrintDC);
        if (job->hDevMode)  GlobalFree(job->hDevMode);
        if (job->hDevNames) GlobalFree(job->hDevNames);
        delete job;
        return;
    }

    HANDLE hThread = CreateThread(nullptr, 0, PrintWorkerThread, job, 0, nullptr);
    if (hThread)
        CloseHandle(hThread);
    else
    {
        // 스레드 생성 실패 시 현재 스레드에서 직접 실행
        PrintWorkerThread(job);
    }
}

// 전방 선언 — 실제 정의는 파일 저장 코드 근처에 있음
static FPDF_DOCUMENT LoadPDFFromFile(const std::wstring& wpath,
                                     std::vector<char>* pFileData = nullptr,
                                     const char* password = nullptr);
static FPDF_DOCUMENT LoadPDFWithPasswordPrompt(HWND hWnd, const std::wstring& path,
                                               std::vector<char>* pFileData);

void OpenFileFromPath(HWND hWnd, const std::wstring& path, bool showSpinner)
{
    FlushRenderQueue();   // 기존 문서의 렌더 요청 모두 취소

    // 탭 준비 (메인 스레드에서 미리 처리)
    if (g_activeTab >= 0 && g_pdfDoc != nullptr)
    {
        SaveTabState(g_tabs[g_activeTab]);
        ClearGlobalsWithoutClose();
        g_tabs.push_back(TabState{});
        g_activeTab = (int)g_tabs.size() - 1;
    }
    else if (g_activeTab < 0)
    {
        g_tabs.push_back(TabState{});
        g_activeTab = 0;
    }
    if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
        g_tabs[g_activeTab].filePath = path;

    if (showSpinner)
    {
        // 스피너 표시 + 백그라운드 스레드 로딩
        StartLoadingGif(hWnd);

        struct Param { HWND hWnd; std::wstring path; };
        Param* p = new Param{ hWnd, path };

        HANDLE hThread = CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
            Param* param = static_cast<Param*>(lp);

            // 파일을 메모리 버퍼로 읽어 로드 — 파일 핸들이 즉시 해제되어
            // 저장 시 덮어쓰기가 가능해진다.
#ifdef _DEBUG
            Sleep(2000); // 스피너 애니메이션 확인용 딜레이 (디버그 빌드 전용)
#endif
            std::vector<char> buf;
            FPDF_DOCUMENT doc = LoadPDFFromFile(param->path, &buf);

            // 주석 로드는 PDFium 스레드 안전 문제로 UI 스레드(WM_PDF_INIT2)에서 처리
            auto sharedBuf = std::make_shared<std::vector<char>>(std::move(buf));
            LoadingResult* res = new LoadingResult{ doc, param->path, false, nullptr, sharedBuf };
            PostMessageW(param->hWnd, WM_PDF_LOADED, 0, reinterpret_cast<LPARAM>(res));

            delete param;
            return 0;
        }, p, 0, nullptr);

        if (hThread) CloseHandle(hThread);
    }
    else
    {
        // 스피너 없이 동기 로딩 (저장 후 재열기 등 내부 용도)
        std::vector<char> buf;
        FPDF_DOCUMENT doc = LoadPDFWithPasswordPrompt(hWnd, path, &buf);
        if (!doc)
        {
            if (g_pwPromptCancelled) { g_pwPromptCancelled = false; CloseTab(hWnd, g_activeTab); return; }
            WCHAR msg[256];
            swprintf_s(msg, 256, LS(STR_PDF_OPEN_FAIL_FMT), FPDF_GetLastError());
            CleanPopup::Show(hWnd, LS(STR_ERROR), msg);
            return;
        }

        g_pdfDoc    = doc;
        g_filePath  = path;
        g_pageCount = FPDF_GetPageCount(g_pdfDoc);

        if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
        {
            g_tabs[g_activeTab].filePath = path;
            g_tabs[g_activeTab].pdfBuffer = std::make_shared<std::vector<char>>(std::move(buf));
        }

        RecalcPageLayouts();
        InitPageCache(g_pageCount);
        InitThumbCache(g_pageCount);
        ExtractBookmarks();
        LoadUserBookmarks();
        AddToRecentFiles(path);
        UpdateRecentFilesMenu(hWnd);

        g_currentPage = 0;
        g_scrollX = 0;
        g_scrollY = 0;
        g_scrollYf = 0.0f;
        FitToWidth(hWnd);
        InitFormFill(hWnd);
        LoadPage(hWnd, 0);
        UpdateScrollBars(hWnd);
        UpdateTitle(hWnd);
        RunAnnotLoad(hWnd);
        InvalidateToolbarBaseCache();
        InvalidateRect(hWnd, nullptr, TRUE);
        UpdateWindow(hWnd);
    }
}

void OpenOrSwitchToTab(HWND hWnd, const std::wstring& path, bool showSpinner)
{
    // 이미 열린 탭이 있으면 해당 탭으로 전환
    for (int i = 0; i < (int)g_tabs.size(); ++i)
    {
        if (_wcsicmp(g_tabs[i].filePath.c_str(), path.c_str()) == 0)
        {
            SwitchToTab(hWnd, i);
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }
    }
    // 없으면 새 탭으로 열기
    OpenFileFromPath(hWnd, path, showSpinner);
}

static void OpenPDFFile(HWND hWnd)
{
    // 기존 렌더 요청 취소 (워커는 계속 실행)
    FlushRenderQueue();

    WCHAR szFile[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"PDF 파일 (*.pdf)\0*.pdf\0모든 파일 (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"PDF 파일 열기";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    ofn.lpstrDefExt = L"pdf";

    if (GetOpenFileNameW(&ofn))
        OpenFileFromPath(hWnd, szFile, true);
}

void LoadPage(HWND hWnd, int pageIndex)
{
    if (!g_pdfDoc || pageIndex < 0 || pageIndex >= g_pageCount)
        return;
    if (pageIndex == g_currentPage && g_pdfPage)
        return;  // 이미 로드됨

    // PDFium은 스레드 안전하지 않으므로 백그라운드 렌더 워커와 동시에 호출되지 않도록 직렬화
    std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);

    // g_findHandle은 g_textPage를 참조하므로 textPage 닫기 전에 먼저 해제
    if (g_findHandle) { FPDFText_FindClose(g_findHandle); g_findHandle = nullptr; }
    if (g_textPage) { FPDFText_ClosePage(g_textPage); g_textPage = nullptr; }
    if (g_pdfPage)
    {
        if (g_formHandle)
        {
            FORM_DoPageAAction(g_pdfPage, g_formHandle, FPDFPAGE_AACTION_CLOSE);
            FORM_OnBeforeClosePage(g_pdfPage, g_formHandle);
        }
        FPDF_ClosePage(g_pdfPage);
        g_pdfPage = nullptr;
    }

    g_pdfPage = FPDF_LoadPage(g_pdfDoc, pageIndex);
    if (!g_pdfPage) return;
    g_textPage = FPDFText_LoadPage(g_pdfPage);
    g_currentPage = pageIndex;
    if (g_formHandle)
    {
        FORM_OnAfterLoadPage(g_pdfPage, g_formHandle);
        FORM_DoPageAAction(g_pdfPage, g_formHandle, FPDFPAGE_AACTION_OPEN);
    }
}

// ─── 연속 스크롤 헬퍼 함수들 ──────────────────────────────────────────────────

void RecalcPageLayouts()
{
    g_pageLayouts.clear();
    g_totalDocHeight = 0;
    g_maxPageWidth = 0;
    if (!g_pdfDoc || g_pageCount <= 0) return;

    g_pageLayouts.resize(g_pageCount);

    if (g_twoPageView)
    {
        // 두쪽 보기: 짝수 인덱스(0,2,4...)가 왼쪽, 홀수가 오른쪽
        // 한 행의 높이 = 두 페이지 중 더 큰 높이
        // 한 행의 너비 = 두 페이지 너비 합 + 가운데 간격
        constexpr int CENTER_GAP = 4;
        int y = 0;
        for (int i = 0; i < g_pageCount; i += 2)
        {
            const int er0 = GetEffectiveRotation(i);
            const int er1 = GetEffectiveRotation(i + 1);
            bool swapped = (er0 == 1 || er0 == 3);

            double w0, h0;
            FPDF_GetPageSizeByIndex(g_pdfDoc, i, &w0, &h0);
            g_pageLayouts[i].pdfW = w0;
            g_pageLayouts[i].pdfH = h0;
            double dispW0 = swapped ? h0 : w0;
            double dispH0 = swapped ? w0 : h0;
            g_pageLayouts[i].renderW = (int)(dispW0 * g_zoom);
            g_pageLayouts[i].renderH = (int)(dispH0 * g_zoom);

            int rowH = g_pageLayouts[i].renderH;
            int rowW = g_pageLayouts[i].renderW;

            if (i + 1 < g_pageCount)
            {
                double w1, h1;
                FPDF_GetPageSizeByIndex(g_pdfDoc, i + 1, &w1, &h1);
                g_pageLayouts[i+1].pdfW = w1;
                g_pageLayouts[i+1].pdfH = h1;
                const bool swapped1 = (er1 == 1 || er1 == 3);
                double dispW1 = swapped1 ? h1 : w1;
                double dispH1 = swapped1 ? w1 : h1;
                g_pageLayouts[i+1].renderW = (int)(dispW1 * g_zoom);
                g_pageLayouts[i+1].renderH = (int)(dispH1 * g_zoom);
                rowH = std::max(rowH, g_pageLayouts[i+1].renderH);
                rowW = (int)((long long)g_pageLayouts[i].renderW + CENTER_GAP + g_pageLayouts[i+1].renderW);
            }

            // yOffset에 행 Y를 저장; X 오프셋은 GetPageScreenPos에서 계산
            g_pageLayouts[i].yOffset = y;
            if (i + 1 < g_pageCount)
                g_pageLayouts[i+1].yOffset = y;

            if (rowW > g_maxPageWidth) g_maxPageWidth = rowW;
            y += rowH + PAGE_GAP;
        }
        g_totalDocHeight = y - PAGE_GAP;
    }
    else
    {
        int y = 0;
        for (int i = 0; i < g_pageCount; i++)
        {
            double w, h;
            FPDF_GetPageSizeByIndex(g_pdfDoc, i, &w, &h);
            g_pageLayouts[i].pdfW = w;
            g_pageLayouts[i].pdfH = h;
            const int erI = GetEffectiveRotation(i);
            bool swapped = (erI == 1 || erI == 3);
            double dispW = swapped ? h : w;
            double dispH = swapped ? w : h;
            g_pageLayouts[i].renderW = (int)(dispW * g_zoom);
            g_pageLayouts[i].renderH = (int)(dispH * g_zoom);
            g_pageLayouts[i].yOffset = y;
            y += g_pageLayouts[i].renderH + PAGE_GAP;
            if (g_pageLayouts[i].renderW > g_maxPageWidth)
                g_maxPageWidth = g_pageLayouts[i].renderW;
        }
        g_totalDocHeight = y - PAGE_GAP;
    }
}

static void RotateAllKeepPosition(HWND hWnd, int delta)
{
    int anchor = 0; double frac = 0.0;
    if (!g_pageLayouts.empty())
    {
        for (int i = 0; i < (int)g_pageLayouts.size(); ++i)
        {
            if (g_pageLayouts[i].yOffset <= g_scrollY) anchor = i; else break;
        }
        if (g_pageLayouts[anchor].renderH > 0)
            frac = (double)(g_scrollY - g_pageLayouts[anchor].yOffset) / g_pageLayouts[anchor].renderH;
    }
    g_rotation = (g_rotation + delta) % 4;
    ClearPageCache();
    ClearPreviewCache();
    g_pageCache.resize(g_pageCount);
    g_prefetchPending.assign(g_pageCount, false);
    ClearThumbCache();
    InitThumbCache(g_pageCount);
    RecalcPageLayouts();
    if (anchor < (int)g_pageLayouts.size())
        g_scrollY = g_pageLayouts[anchor].yOffset + (int)(frac * g_pageLayouts[anchor].renderH);
    for (auto& h : g_highlights)  h.cachedRects.clear();
    for (auto& u : g_underlines)  u.cachedRects.clear();
    for (auto& s : g_strikeouts)  s.cachedRects.clear();
    for (auto& q : g_squigglies)  q.cachedRects.clear();
    UpdateScrollBars(hWnd);
    InvalidateRect(hWnd, nullptr, TRUE);
}

static void RotateSinglePage(HWND hWnd, int pageIdx, int delta)
{
    if (!g_pdfDoc || pageIdx < 0 || pageIdx >= g_pageCount) return;
    if ((int)g_pageRot.size() != g_pageCount) g_pageRot.resize(g_pageCount, 0);
    g_pageRot[pageIdx] = (g_pageRot[pageIdx] + delta + 4) % 4;

    FlushRenderQueue();
    if (pageIdx < (int)g_pageCache.size() && g_pageCache[pageIdx].hBmp)
    {
        DeleteObject(g_pageCache[pageIdx].hBmp);
        g_pageCache[pageIdx] = PageCacheEntry{};
    }
    if (pageIdx < (int)g_previewCache.size() && g_previewCache[pageIdx].hBmp)
    {
        DeleteObject(g_previewCache[pageIdx].hBmp);
        g_previewCache[pageIdx] = PreviewEntry{};
    }
    if (pageIdx < (int)g_thumbCache.size() && g_thumbCache[pageIdx].hBmp)
    {
        DeleteObject(g_thumbCache[pageIdx].hBmp);
        g_thumbCache[pageIdx] = ThumbCacheEntry{};
    }
    g_pageBaseDirty = true;
    RecalcPageLayouts();
    for (auto& h : g_highlights)  h.cachedRects.clear();
    for (auto& u : g_underlines)  u.cachedRects.clear();
    for (auto& s : g_strikeouts)  s.cachedRects.clear();
    for (auto& q : g_squigglies)  q.cachedRects.clear();
    g_dirty = true;
    UpdateScrollBars(hWnd);
    UpdateTitle(hWnd);
    InvalidateRect(hWnd, nullptr, TRUE);
}

void GetPageScreenPos(int pageIndex, const RECT& vp, int* outLeft, int* outTop)
{
    // 수직 스크롤바가 표시될 때 실제 사용 가능한 너비를 일관되게 맞춤
    const int sbW = GetSystemMetrics(SM_CXVSCROLL);
    const int vpW = (vp.right - vp.left) - sbW;
    int renderW = g_pageLayouts[pageIndex].renderW;
    *outTop = vp.top + g_pageLayouts[pageIndex].yOffset - g_scrollY;

    if (g_twoPageView)
    {
        constexpr int CENTER_GAP = 4;
        bool isRight = (pageIndex % 2 == 1);
        int leftIdx  = isRight ? pageIndex - 1 : pageIndex;
        int rightIdx = leftIdx + 1;

        int leftW  = g_pageLayouts[leftIdx].renderW;
        int rightW = (rightIdx < g_pageCount) ? g_pageLayouts[rightIdx].renderW : 0;
        int rowW   = leftW + (rightIdx < g_pageCount ? CENTER_GAP + rightW : 0);

        int rowLeft = (g_maxPageWidth <= vpW - SIDE_MARGIN * 2)
            ? vp.left + SIDE_MARGIN + (vpW - SIDE_MARGIN * 2 - rowW) / 2
            : vp.left + SIDE_MARGIN + (g_maxPageWidth - rowW) / 2 - g_scrollX;

        if (isRight)
            *outLeft = rowLeft + leftW + CENTER_GAP;
        else
            *outLeft = rowLeft;
    }
    else
    {
        if (g_maxPageWidth <= vpW - SIDE_MARGIN * 2)
            *outLeft = vp.left + SIDE_MARGIN + (vpW - SIDE_MARGIN * 2 - renderW) / 2;
        else
            *outLeft = vp.left + SIDE_MARGIN + (g_maxPageWidth - renderW) / 2 - g_scrollX;
    }
}

int GetMostVisiblePage(const RECT& vp)
{
    int vpH = vp.bottom - vp.top;
    int vpCenter = g_scrollY + vpH / 2;
    for (int i = 0; i < g_pageCount; i++)
    {
        int pageBot = g_pageLayouts[i].yOffset + g_pageLayouts[i].renderH;
        if (vpCenter < pageBot)
            return i;
    }
    return g_pageCount - 1;
}

void UpdateActivePageFromScroll(HWND hWnd)
{
    if (!g_pdfDoc || g_pageCount <= 0 || g_pageLayouts.empty()) return;
    RECT cr; GetClientRect(hWnd, &cr);
    const RECT vp = GetViewportRect(cr);
    int mostVisible = GetMostVisiblePage(vp);
    if (mostVisible != g_currentPage)
    {
        LoadPage(hWnd, mostVisible);
        UpdateTitle(hWnd);
    }
    // 새로 보이는 페이지의 주석이 아직 로드되지 않았으면 트리거
    TriggerAnnotLoadForVisiblePages(hWnd);
}

void ScrollToPage(HWND hWnd, int pageIndex)
{
    if (pageIndex < 0 || pageIndex >= g_pageCount || g_pageLayouts.empty()) return;

    g_scrollY = g_pageLayouts[pageIndex].yOffset;
    g_scrollYf = (float)g_scrollY;
    g_scrollTargetY = g_scrollYf;
    g_scrollX = 0;
    LoadPage(hWnd, pageIndex);
    UpdateTitle(hWnd);
    UpdateScrollBars(hWnd);
    InvalidateRect(hWnd, nullptr, TRUE);
    TriggerAnnotLoadForVisiblePages(hWnd);
}

static int HitTestPageAtPoint(int screenX, int screenY, const RECT& vp, double* outPdfX, double* outPdfY)
{
    for (int i = 0; i < g_pageCount; i++)
    {
        int pgLeft, pgTop;
        GetPageScreenPos(i, vp, &pgLeft, &pgTop);
        int renderW = g_pageLayouts[i].renderW;
        int renderH = g_pageLayouts[i].renderH;
        if (screenY >= pgTop && screenY < pgTop + renderH &&
            screenX >= pgLeft && screenX < pgLeft + renderW)
        {
            // 화면 좌표 → PDF 좌표 (회전 고려)
            double sx = (screenX - pgLeft) / (double)g_zoom;
            double sy = (screenY - pgTop) / (double)g_zoom;
            double pw = g_pageLayouts[i].pdfW;
            double ph = g_pageLayouts[i].pdfH;
            double pdfX = 0, pdfY = 0;
            switch (GetEffectiveRotation(i)) {
            case 0: pdfX = sx;       pdfY = ph - sy;  break;
            case 1: pdfX = sy;       pdfY = sx;       break;
            case 2: pdfX = pw - sx;  pdfY = sy;       break;
            case 3: pdfX = ph - sy;  pdfY = pw - sx;  break;
            }
            if (outPdfX) *outPdfX = pdfX;
            if (outPdfY) *outPdfY = pdfY;
            return i;
        }
    }
    return -1;
}

static void SaveWindowPlacement(HWND hWnd)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;

    WINDOWPLACEMENT wp = { sizeof(wp) };
    GetWindowPlacement(hWnd, &wp);

    // 최대화 상태이면 복원 위치를 저장
    const RECT& rc = wp.rcNormalPosition;
    DWORD x = rc.left, y = rc.top;
    DWORD w = rc.right - rc.left, h = rc.bottom - rc.top;
    DWORD maximized = (wp.showCmd == SW_SHOWMAXIMIZED) ? 1 : 0;

    RegSetValueExW(hKey, REGISTRY_WND_X, 0, REG_DWORD, (const BYTE*)&x, sizeof(DWORD));
    RegSetValueExW(hKey, REGISTRY_WND_Y, 0, REG_DWORD, (const BYTE*)&y, sizeof(DWORD));
    RegSetValueExW(hKey, REGISTRY_WND_W, 0, REG_DWORD, (const BYTE*)&w, sizeof(DWORD));
    RegSetValueExW(hKey, REGISTRY_WND_H, 0, REG_DWORD, (const BYTE*)&h, sizeof(DWORD));
    RegSetValueExW(hKey, REGISTRY_WND_MAX, 0, REG_DWORD, (const BYTE*)&maximized, sizeof(DWORD));
    RegCloseKey(hKey);
}

static bool LoadWindowPlacement(int* outX, int* outY, int* outW, int* outH, bool* outMaximized)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    DWORD x = 0, y = 0, w = 0, h = 0, maximized = 0;
    DWORD sz = sizeof(DWORD);
    bool ok = (RegQueryValueExW(hKey, REGISTRY_WND_X, nullptr, nullptr, (LPBYTE)&x, &sz) == ERROR_SUCCESS);
    sz = sizeof(DWORD);
    ok = ok && (RegQueryValueExW(hKey, REGISTRY_WND_Y, nullptr, nullptr, (LPBYTE)&y, &sz) == ERROR_SUCCESS);
    sz = sizeof(DWORD);
    ok = ok && (RegQueryValueExW(hKey, REGISTRY_WND_W, nullptr, nullptr, (LPBYTE)&w, &sz) == ERROR_SUCCESS);
    sz = sizeof(DWORD);
    ok = ok && (RegQueryValueExW(hKey, REGISTRY_WND_H, nullptr, nullptr, (LPBYTE)&h, &sz) == ERROR_SUCCESS);
    sz = sizeof(DWORD);
    RegQueryValueExW(hKey, REGISTRY_WND_MAX, nullptr, nullptr, (LPBYTE)&maximized, &sz);
    RegCloseKey(hKey);

    if (ok && w > 0 && h > 0)
    {
        *outX = (int)x; *outY = (int)y;
        *outW = (int)w; *outH = (int)h;
        *outMaximized = (maximized != 0);
        return true;
    }
    return false;
}

// ─── 페이지 이동 다이얼로그는 GotoDialog.h / GotoDialog.cpp 참조 ───────────────

// ─── 뷰포트 영역 (툴바/상태바 제외) ──────────────────────────────────────────

RECT GetViewportRect(const RECT& clientRect)
{
    RECT vp = clientRect;
    if (!g_fullscreen)
    {
        vp.top += TOOLBAR_HEIGHT;
        if (g_tabs.size() >= 1)
            vp.top += TAB_BAR_HEIGHT;
        vp.bottom -= STATUSBAR_HEIGHT;
    }
    if (g_sidebarVisible)
        vp.left += SIDEBAR_WIDTH;
    if (GroupPanel::IsVisible())
        vp.right -= GROUP_PANEL_WIDTH;
    if (vp.bottom < vp.top) vp.bottom = vp.top;
    return vp;
}

// ─── 찾기 기능은 FindBar.h / FindBar.cpp 참조 ────────────────────────────────


// ─── 그라데이션 배경 / 툴바 / 상태바 / DrawAlphaLine 은 Toolbar.h / Toolbar.cpp 참조 ──

// ─── annotation rect 계산 헬퍼 ─────────────────────────────────────────────────
// FPDFText 로부터 병합된 PDF 좌표 rect 목록을 반환한다.
// 결과는 annotation 아이템의 cachedRects 에 저장 후 재사용한다.
static void BuildAnnotRects(FPDF_TEXTPAGE textPage,
    int charFirst, int charLast,
    std::vector<PdfRectCache>& out)
{
    out.clear();
    const int count = charLast - charFirst + 1;
    if (count <= 0) return;

    const int nRects = FPDFText_CountRects(textPage, charFirst, count);
    if (nRects <= 0) return;

    out.resize(nRects);
    for (int ri = 0; ri < nRects; ++ri)
        FPDFText_GetRect(textPage, ri, &out[ri].left, &out[ri].top,
            &out[ri].right, &out[ri].bottom);

    // 같은 줄 높이 통일
    for (size_t i = 0; i < out.size(); ++i)
    {
        double ci = (out[i].top + out[i].bottom) * 0.5;
        double hi = out[i].top - out[i].bottom;
        if (hi <= 0) continue;
        for (size_t j = i + 1; j < out.size(); ++j)
        {
            double hj = out[j].top - out[j].bottom;
            if (hj <= 0) continue;
            double cj = (out[j].top + out[j].bottom) * 0.5;
            if (std::abs(ci - cj) < std::max(hi, hj) * 0.7)
            {
                double ut = std::max(out[i].top,    out[j].top);
                double ub = std::min(out[i].bottom, out[j].bottom);
                out[i].top = out[j].top = ut;
                out[i].bottom = out[j].bottom = ub;
            }
        }
    }

    // 같은 줄 수평 병합
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (out[i].left >= out[i].right) continue;
        double lineH_i = out[i].top - out[i].bottom;
        bool merged = true;
        while (merged)
        {
            merged = false;
            for (size_t j = i + 1; j < out.size(); ++j)
            {
                if (out[j].left >= out[j].right) continue;
                double lineH_j = out[j].top - out[j].bottom;
                double maxH = std::max(lineH_i, lineH_j);
                double ci2 = (out[i].top + out[i].bottom) * 0.5;
                double cj2 = (out[j].top + out[j].bottom) * 0.5;
                if (maxH <= 0 || std::abs(ci2 - cj2) >= maxH * 0.7) continue;
                out[i].left   = std::min(out[i].left,   out[j].left);
                out[i].right  = std::max(out[i].right,  out[j].right);
                out[i].top    = std::max(out[i].top,    out[j].top);
                out[i].bottom = std::min(out[i].bottom, out[j].bottom);
                lineH_i = out[i].top - out[i].bottom;
                out[j].right = out[j].left; // 병합됨 → 비어 있음
                merged = true;
            }
        }
    }
}

// ─── 하이라이트 그리기

static void DrawHighlights(HDC hdc, const RECT& clientRect)
{
    if (g_highlights.empty() || !g_pdfDoc || g_pageLayouts.empty()) return;

    // PDFium은 스레드 안전하지 않으므로 백그라운드 렌더 워커와 동시에 호출되지 않도록 직렬화
    std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);

    const RECT vp = GetViewportRect(clientRect);

    // 페이지별 캐시: 같은 페이지의 어노테이션이 여러 개일 때 중복 로드 방지
    std::unordered_map<int, std::pair<FPDF_PAGE, FPDF_TEXTPAGE>> pageCache;

    // 공유 1x1 AlphaBlend DC: 매 rect마다 DC/비트맵 생성 오버헤드 제거
    HDC      hlMemDC    = CreateCompatibleDC(hdc);
    HBITMAP  hlBmp      = CreateCompatibleBitmap(hdc, 1, 1);
    HBITMAP  hlOldBmp   = static_cast<HBITMAP>(SelectObject(hlMemDC, hlBmp));
    COLORREF hlLastColor = CLR_INVALID;
    BLENDFUNCTION hlBf   = { AC_SRC_OVER, 0, 100, 0 }; // 수마트라 PDF와 동일한 투명도(~40%)

    for (int hlIdx = 0; hlIdx < static_cast<int>(g_highlights.size()); ++hlIdx)
    {
        auto& hl = g_highlights[hlIdx];
        bool isSelected = (hlIdx == g_selectedHighlight);
        if (hl.pageIndex < 0 || hl.pageIndex >= g_pageCount) continue;

        // 뷰포트에 보이지 않는 페이지는 건너뜀
        int pgTop = vp.top + g_pageLayouts[hl.pageIndex].yOffset - g_scrollY;
        int pgBot = pgTop + g_pageLayouts[hl.pageIndex].renderH;
        if (pgBot < vp.top || pgTop > vp.bottom) continue;

        // rect 캐시가 없으면 한 번만 계산
        if (hl.cachedRects.empty())
        {
            FPDF_PAGE page; FPDF_TEXTPAGE textPage;
            if (hl.pageIndex == g_currentPage && g_pdfPage) {
                page = g_pdfPage; textPage = g_textPage;
            } else {
                auto it = pageCache.find(hl.pageIndex);
                if (it != pageCache.end()) {
                    page = it->second.first; textPage = it->second.second;
                } else {
                    page = FPDF_LoadPage(g_pdfDoc, hl.pageIndex);
                    textPage = page ? FPDFText_LoadPage(page) : nullptr;
                    pageCache[hl.pageIndex] = { page, textPage };
                }
            }
            if (!page || !textPage) continue;
            const int firstIdx = std::min(hl.charStart, hl.charEnd);
            const int lastIdx  = std::max(hl.charStart, hl.charEnd);
            BuildAnnotRects(textPage, firstIdx, lastIdx, hl.cachedRects);
            if (hl.cachedRects.empty()) continue;
        }

        {
            const double pageW = g_pageLayouts[hl.pageIndex].pdfW;
            const double pageH = g_pageLayouts[hl.pageIndex].pdfH;
            int pgLeft, pgTopPos;
            GetPageScreenPos(hl.pageIndex, vp, &pgLeft, &pgTopPos);

            for (const auto& r : hl.cachedRects)
            {
                if (r.left >= r.right) continue;

                int sx1, sy1, sx2, sy2;
                PdfToScreen(r.left, r.top, pageW, pageH,
                    pgLeft, pgTopPos, g_zoom, GetEffectiveRotation(hl.pageIndex), &sx1, &sy1);
                PdfToScreen(r.right, r.bottom, pageW, pageH,
                    pgLeft, pgTopPos, g_zoom, GetEffectiveRotation(hl.pageIndex), &sx2, &sy2);
                if (sx1 > sx2) std::swap(sx1, sx2);
                if (sy1 > sy2) std::swap(sy1, sy2);

                int cx1 = std::max(sx1, (int)vp.left);
                int cy1 = std::max(sy1, (int)vp.top);
                int cx2 = std::min(sx2, (int)vp.right);
                int cy2 = std::min(sy2, (int)vp.bottom);
                if (cx2 <= cx1 || cy2 <= cy1) continue;

                const int bw = cx2 - cx1, bh = cy2 - cy1;
                if (hl.color != hlLastColor) {
                    HBRUSH hBr = CreateSolidBrush(hl.color);
                    RECT fr = { 0, 0, 1, 1 };
                    FillRect(hlMemDC, &fr, hBr);
                    DeleteObject(hBr);
                    hlLastColor = hl.color;
                }
                AlphaBlend(hdc, cx1, cy1, bw, bh, hlMemDC, 0, 0, 1, 1, hlBf);

                if (isSelected)
                {
                    HPEN selPen = CreatePen(PS_DASH, 1, RGB(40, 120, 255));
                    HPEN oPen = static_cast<HPEN>(SelectObject(hdc, selPen));
                    HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
                    Rectangle(hdc, cx1 - 1, cy1 - 1, cx2 + 1, cy2 + 1);
                    SelectObject(hdc, oBr);
                    SelectObject(hdc, oPen);
                    DeleteObject(selPen);
                }
            }
        }
    }

    SelectObject(hlMemDC, hlOldBmp);
    DeleteObject(hlBmp);
    DeleteDC(hlMemDC);

    for (auto& kv : pageCache) {
        if (kv.second.second) FPDFText_ClosePage(kv.second.second);
        if (kv.second.first)  FPDF_ClosePage(kv.second.first);
    }
}

// 선택 영역으로 하이라이트 추가
static void AddHighlightFromSelection(COLORREF color)
{
    if (!g_hasSelection || g_selCharStart < 0 || g_selCharEnd < 0) return;
    if (g_selCharStart == g_selCharEnd) return;

    HighlightItem hl{};
    hl.pageIndex = g_currentPage;
    hl.charStart = std::min(g_selCharStart, g_selCharEnd);
    hl.charEnd   = std::max(g_selCharStart, g_selCharEnd);
    hl.color     = color;
    g_highlights.push_back(hl);
    g_dirty = true;
    // 선택 해제
    g_hasSelection = false;
    g_selCharStart = g_selCharEnd = -1;
    g_selPage = -1;
}

// 특정 위치의 하이라이트 찾기
static int HitTestHighlight(int pageIndex, int charIdx)
{
    for (int i = static_cast<int>(g_highlights.size()) - 1; i >= 0; --i)
    {
        const auto& hl = g_highlights[i];
        if (hl.pageIndex == pageIndex)
        {
            int lo = std::min(hl.charStart, hl.charEnd);
            int hi = std::max(hl.charStart, hl.charEnd);
            if (charIdx >= lo && charIdx <= hi)
                return i;
        }
    }
    return -1;
}

// ─── 밑줄 그리기 ──────────────────────────────────────────────────────────────

static void DrawUnderlines(HDC hdc, const RECT& clientRect)
{
    if (g_underlines.empty() || !g_pdfDoc || g_pageLayouts.empty()) return;

    // PDFium은 스레드 안전하지 않으므로 백그라운드 렌더 워커와 동시에 호출되지 않도록 직렬화
    std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);

    const RECT vp = GetViewportRect(clientRect);

    std::unordered_map<int, std::pair<FPDF_PAGE, FPDF_TEXTPAGE>> pageCache;

    for (int ulIdx = 0; ulIdx < static_cast<int>(g_underlines.size()); ++ulIdx)
    {
        auto& ul = g_underlines[ulIdx];
        bool isSelected = (ulIdx == g_selectedUnderline);
        if (ul.pageIndex < 0 || ul.pageIndex >= g_pageCount) continue;

        int pgTopY = vp.top + g_pageLayouts[ul.pageIndex].yOffset - g_scrollY;
        int pgBot  = pgTopY + g_pageLayouts[ul.pageIndex].renderH;
        if (pgBot < vp.top || pgTopY > vp.bottom) continue;

        if (ul.cachedRects.empty())
        {
            FPDF_PAGE page; FPDF_TEXTPAGE textPage;
            if (ul.pageIndex == g_currentPage && g_pdfPage) {
                page = g_pdfPage; textPage = g_textPage;
            } else {
                auto it = pageCache.find(ul.pageIndex);
                if (it != pageCache.end()) {
                    page = it->second.first; textPage = it->second.second;
                } else {
                    page = FPDF_LoadPage(g_pdfDoc, ul.pageIndex);
                    textPage = page ? FPDFText_LoadPage(page) : nullptr;
                    pageCache[ul.pageIndex] = { page, textPage };
                }
            }
            if (!page || !textPage) continue;
            const int firstIdx = std::min(ul.charStart, ul.charEnd);
            const int lastIdx  = std::max(ul.charStart, ul.charEnd);
            BuildAnnotRects(textPage, firstIdx, lastIdx, ul.cachedRects);
            if (ul.cachedRects.empty()) continue;
        }

        {
            const double pageW = g_pageLayouts[ul.pageIndex].pdfW;
            const double pageH = g_pageLayouts[ul.pageIndex].pdfH;
            int pgLeft, pgTopPos;
            GetPageScreenPos(ul.pageIndex, vp, &pgLeft, &pgTopPos);

            for (const auto& r : ul.cachedRects)
            {
                if (r.left >= r.right) continue;

                int sx1, sy1, sx2, sy2;
                PdfToScreen(r.left, r.top, pageW, pageH,
                    pgLeft, pgTopPos, g_zoom, GetEffectiveRotation(ul.pageIndex), &sx1, &sy1);
                PdfToScreen(r.right, r.bottom, pageW, pageH,
                    pgLeft, pgTopPos, g_zoom, GetEffectiveRotation(ul.pageIndex), &sx2, &sy2);
                if (sx1 > sx2) std::swap(sx1, sx2);
                if (sy1 > sy2) std::swap(sy1, sy2);

                int gap       = 2;
                int thickness = std::max(1, std::min(2, (int)(1.0f * g_zoom)));
                const int ulRot = GetEffectiveRotation(ul.pageIndex);
                if (ulRot == 0 || ulRot == 2)
                {
                    int cx1   = std::max(sx1, (int)vp.left);
                    int cx2   = std::min(sx2, (int)vp.right);
                    int lineY = (ulRot == 0) ? std::min(sy2 + gap, (int)vp.bottom) : sy1 - gap;
                    if (cx2 <= cx1 || lineY < vp.top || lineY > vp.bottom) continue;
                    DrawAlphaLine(hdc, cx1, lineY, cx2, lineY, ul.color, thickness, 200);
                }
                else
                {
                    int cy1   = std::max(sy1, (int)vp.top);
                    int cy2   = std::min(sy2, (int)vp.bottom);
                    int lineX = (ulRot == 1) ? sx1 - gap : sx2 + gap;
                    if (cy2 <= cy1 || lineX < vp.left || lineX > vp.right) continue;
                    {
                        const int w = thickness, h = cy2 - cy1;
                        HDC     memDC = CreateCompatibleDC(hdc);
                        HBITMAP hBmp  = CreateCompatibleBitmap(hdc, w, h);
                        HBITMAP oBmp  = static_cast<HBITMAP>(SelectObject(memDC, hBmp));
                        HBRUSH  br    = CreateSolidBrush(ul.color);
                        RECT    fr    = { 0, 0, w, h };
                        FillRect(memDC, &fr, br);
                        DeleteObject(br);
                        BLENDFUNCTION bf = {};
                        bf.BlendOp             = AC_SRC_OVER;
                        bf.SourceConstantAlpha = 200;
                        AlphaBlend(hdc, lineX - w / 2, cy1, w, h, memDC, 0, 0, w, h, bf);
                        SelectObject(memDC, oBmp);
                        DeleteObject(hBmp);
                        DeleteDC(memDC);
                    }
                }

                if (isSelected)
                {
                    HPEN selPen = CreatePen(PS_DASH, 1, RGB(40, 120, 255));
                    HPEN oPen2 = static_cast<HPEN>(SelectObject(hdc, selPen));
                    HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
                    Rectangle(hdc, sx1 - 1, sy1 - 1, sx2 + 1, sy2 + 1);
                    SelectObject(hdc, oBr);
                    SelectObject(hdc, oPen2);
                    DeleteObject(selPen);
                }
            }
        }
    }

    for (auto& kv : pageCache) {
        if (kv.second.second) FPDFText_ClosePage(kv.second.second);
        if (kv.second.first)  FPDF_ClosePage(kv.second.first);
    }
}

        // 선택 영역으로 밑줄 추가
        static void AddUnderlineFromSelection(COLORREF color)
        {
            if (!g_hasSelection || g_selCharStart < 0 || g_selCharEnd < 0) return;
            if (g_selCharStart == g_selCharEnd) return;

            UnderlineItem ul{};
            ul.pageIndex = g_currentPage;
            ul.charStart = std::min(g_selCharStart, g_selCharEnd);
            ul.charEnd   = std::max(g_selCharStart, g_selCharEnd);
            ul.color     = color;
            g_underlines.push_back(ul);
            g_dirty = true;
            g_hasSelection = false;
            g_selCharStart = g_selCharEnd = -1;
            g_selPage = -1;
        }

        // 특정 위치의 밑줄 찾기
static int HitTestUnderline(int pageIndex, int charIdx)
{
    for (int i = static_cast<int>(g_underlines.size()) - 1; i >= 0; --i)
    {
        const auto& ul = g_underlines[i];
        if (ul.pageIndex == pageIndex)
        {
            int lo = std::min(ul.charStart, ul.charEnd);
            int hi = std::max(ul.charStart, ul.charEnd);
            if (charIdx >= lo && charIdx <= hi)
                return i;
        }
    }
    return -1;
}

// ─── 취소선 그리기 ──────────────────────────────────────────────────────────

static void DrawStrikeouts(HDC hdc, const RECT& clientRect)
{
    if (g_strikeouts.empty() || !g_pdfDoc || g_pageLayouts.empty()) return;

    // PDFium은 스레드 안전하지 않으므로 백그라운드 렌더 워커와 동시에 호출되지 않도록 직렬화
    std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);

    const RECT vp = GetViewportRect(clientRect);

    std::unordered_map<int, std::pair<FPDF_PAGE, FPDF_TEXTPAGE>> pageCache;

    for (int soIdx = 0; soIdx < static_cast<int>(g_strikeouts.size()); ++soIdx)
    {
        auto& so = g_strikeouts[soIdx];
        bool isSelected = (soIdx == g_selectedStrikeout);
        if (so.pageIndex < 0 || so.pageIndex >= g_pageCount) continue;

        int pgTopY = vp.top + g_pageLayouts[so.pageIndex].yOffset - g_scrollY;
        int pgBot  = pgTopY + g_pageLayouts[so.pageIndex].renderH;
        if (pgBot < vp.top || pgTopY > vp.bottom) continue;

        if (so.cachedRects.empty())
        {
            FPDF_PAGE page; FPDF_TEXTPAGE textPage;
            if (so.pageIndex == g_currentPage && g_pdfPage) {
                page = g_pdfPage; textPage = g_textPage;
            } else {
                auto it = pageCache.find(so.pageIndex);
                if (it != pageCache.end()) {
                    page = it->second.first; textPage = it->second.second;
                } else {
                    page = FPDF_LoadPage(g_pdfDoc, so.pageIndex);
                    textPage = page ? FPDFText_LoadPage(page) : nullptr;
                    pageCache[so.pageIndex] = { page, textPage };
                }
            }
            if (!page || !textPage) continue;
            const int firstIdx = std::min(so.charStart, so.charEnd);
            const int lastIdx  = std::max(so.charStart, so.charEnd);
            BuildAnnotRects(textPage, firstIdx, lastIdx, so.cachedRects);
            if (so.cachedRects.empty()) continue;
        }

        {
            const double pageW = g_pageLayouts[so.pageIndex].pdfW;
            const double pageH = g_pageLayouts[so.pageIndex].pdfH;
            int pgLeft, pgTopPos;
            GetPageScreenPos(so.pageIndex, vp, &pgLeft, &pgTopPos);

            for (const auto& r : so.cachedRects)
            {
                if (r.left >= r.right) continue;

                int sx1, sy1, sx2, sy2;
                PdfToScreen(r.left, r.top, pageW, pageH,
                    pgLeft, pgTopPos, g_zoom, GetEffectiveRotation(so.pageIndex), &sx1, &sy1);
                PdfToScreen(r.right, r.bottom, pageW, pageH,
                    pgLeft, pgTopPos, g_zoom, GetEffectiveRotation(so.pageIndex), &sx2, &sy2);
                if (sx1 > sx2) std::swap(sx1, sx2);
                if (sy1 > sy2) std::swap(sy1, sy2);

                const int soRot = GetEffectiveRotation(so.pageIndex);
                int thickness = std::max(1, (int)(1.5f * g_zoom));
                if (soRot == 0 || soRot == 2)
                {
                    int cx1  = std::max(sx1, (int)vp.left);
                    int cx2  = std::min(sx2, (int)vp.right);
                    int lineY = (sy1 + sy2) / 2;
                    if (cx2 <= cx1 || lineY < vp.top || lineY > vp.bottom) continue;
                    DrawAlphaLine(hdc, cx1, lineY, cx2, lineY, so.color, thickness, 200);
                }
                else
                {
                    int cy1  = std::max(sy1, (int)vp.top);
                    int cy2  = std::min(sy2, (int)vp.bottom);
                    int lineX = (sx1 + sx2) / 2;
                    if (cy2 <= cy1 || lineX < vp.left || lineX > vp.right) continue;
                    const int w = thickness, h = cy2 - cy1;
                    HDC     memDC = CreateCompatibleDC(hdc);
                    HBITMAP hBmp  = CreateCompatibleBitmap(hdc, w, h);
                    HBITMAP oBmp  = static_cast<HBITMAP>(SelectObject(memDC, hBmp));
                    HBRUSH  br    = CreateSolidBrush(so.color);
                    RECT    fr    = { 0, 0, w, h };
                    FillRect(memDC, &fr, br);
                    DeleteObject(br);
                    BLENDFUNCTION bf = {};
                    bf.BlendOp             = AC_SRC_OVER;
                    bf.SourceConstantAlpha = 200;
                    AlphaBlend(hdc, lineX - w / 2, cy1, w, h, memDC, 0, 0, w, h, bf);
                    SelectObject(memDC, oBmp);
                    DeleteObject(hBmp);
                    DeleteDC(memDC);
                }

                if (isSelected)
                {
                    HPEN selPen = CreatePen(PS_DASH, 1, RGB(40, 120, 255));
                    HPEN oPen2 = static_cast<HPEN>(SelectObject(hdc, selPen));
                    HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
                    Rectangle(hdc, sx1 - 1, sy1 - 1, sx2 + 1, sy2 + 1);
                    SelectObject(hdc, oBr);
                    SelectObject(hdc, oPen2);
                    DeleteObject(selPen);
                }
            }
        }
    }

    for (auto& kv : pageCache) {
        if (kv.second.second) FPDFText_ClosePage(kv.second.second);
        if (kv.second.first)  FPDF_ClosePage(kv.second.first);
    }
}

// 선택 영역으로 취소선 추가
static void AddStrikeoutFromSelection(COLORREF color)
{
    if (!g_hasSelection || g_selCharStart < 0 || g_selCharEnd < 0) return;
    if (g_selCharStart == g_selCharEnd) return;

    StrikeoutItem so;
    so.pageIndex = g_currentPage;
    so.charStart = std::min(g_selCharStart, g_selCharEnd);
    so.charEnd   = std::max(g_selCharStart, g_selCharEnd);
    so.color     = color;
    g_strikeouts.push_back(so);
    g_dirty = true;
    g_hasSelection = false;
    g_selCharStart = g_selCharEnd = -1;
    g_selPage = -1;
}

// 특정 위치의 취소선 찾기
static int HitTestStrikeout(int pageIndex, int charIdx)
{
    for (int i = static_cast<int>(g_strikeouts.size()) - 1; i >= 0; --i)
    {
        const auto& so = g_strikeouts[i];
        if (so.pageIndex == pageIndex)
        {
            int lo = std::min(so.charStart, so.charEnd);
            int hi = std::max(so.charStart, so.charEnd);
            if (charIdx >= lo && charIdx <= hi)
                return i;
        }
    }
    return -1;
}

// ─── 물결 밑줄 그리기 ──────────────────────────────────────────────────────────

static void DrawSquigglies(HDC hdc, const RECT& clientRect)
{
    if (g_squigglies.empty() || !g_pdfDoc || g_pageLayouts.empty()) return;

    // PDFium은 스레드 안전하지 않으므로 백그라운드 렌더 워커와 동시에 호출되지 않도록 직렬화
    std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);

    const RECT vp = GetViewportRect(clientRect);

    std::unordered_map<int, std::pair<FPDF_PAGE, FPDF_TEXTPAGE>> pageCache;

    // 물결선 오프스크린 버퍼 (모든 rect 공유, 필요 시 재할당)
    HDC     sqDC    = CreateCompatibleDC(hdc);
    HBITMAP sqBmp   = nullptr;
    HBITMAP sqOld   = nullptr;
    int     sqBmpW  = 0, sqBmpH = 0;
    BLENDFUNCTION sqBf = { AC_SRC_OVER, 0, 200, 0 };

    for (int sqIdx = 0; sqIdx < static_cast<int>(g_squigglies.size()); ++sqIdx)
    {
        auto& sq = g_squigglies[sqIdx];
        bool isSelected = (sqIdx == g_selectedSquiggly);
        if (sq.pageIndex < 0 || sq.pageIndex >= g_pageCount) continue;

        int pgTopY = vp.top + g_pageLayouts[sq.pageIndex].yOffset - g_scrollY;
        int pgBot  = pgTopY + g_pageLayouts[sq.pageIndex].renderH;
        if (pgBot < vp.top || pgTopY > vp.bottom) continue;

        if (sq.cachedRects.empty())
        {
            FPDF_PAGE page; FPDF_TEXTPAGE textPage;
            if (sq.pageIndex == g_currentPage && g_pdfPage) {
                page = g_pdfPage; textPage = g_textPage;
            } else {
                auto it = pageCache.find(sq.pageIndex);
                if (it != pageCache.end()) {
                    page = it->second.first; textPage = it->second.second;
                } else {
                    page = FPDF_LoadPage(g_pdfDoc, sq.pageIndex);
                    textPage = page ? FPDFText_LoadPage(page) : nullptr;
                    pageCache[sq.pageIndex] = { page, textPage };
                }
            }
            if (!page || !textPage) continue;
            const int firstIdx = std::min(sq.charStart, sq.charEnd);
            const int lastIdx  = std::max(sq.charStart, sq.charEnd);
            BuildAnnotRects(textPage, firstIdx, lastIdx, sq.cachedRects);
            if (sq.cachedRects.empty()) continue;
        }

        {
            const double pageW = g_pageLayouts[sq.pageIndex].pdfW;
            const double pageH = g_pageLayouts[sq.pageIndex].pdfH;
            int pgLeft, pgTopPos;
            GetPageScreenPos(sq.pageIndex, vp, &pgLeft, &pgTopPos);

            int amplitude = std::max(2, (int)(2.0f * g_zoom));
            int waveLen   = std::max(4, (int)(4.0f * g_zoom));
            int thickness = std::max(1, (int)(1.0f * g_zoom));
            int h2        = amplitude * 2 + thickness + 2;

            for (const auto& r : sq.cachedRects)
            {
                if (r.left >= r.right) continue;

                int sx1, sy1, sx2, sy2;
                PdfToScreen(r.left, r.top, pageW, pageH,
                    pgLeft, pgTopPos, g_zoom, GetEffectiveRotation(sq.pageIndex), &sx1, &sy1);
                PdfToScreen(r.right, r.bottom, pageW, pageH,
                    pgLeft, pgTopPos, g_zoom, GetEffectiveRotation(sq.pageIndex), &sx2, &sy2);
                if (sx1 > sx2) std::swap(sx1, sx2);
                if (sy1 > sy2) std::swap(sy1, sy2);

                const int sqRot = GetEffectiveRotation(sq.pageIndex);
                const bool vert = (sqRot == 1 || sqRot == 3);
                // 글자 밑부분(디센더)을 가리지 않도록 물결선을 텍스트 하단보다 더 아래로 내려서 그림
                int gap  = std::max(1, (int)(1.0f * g_zoom));
                int c1, c2, baseY = 0, baseX = 0;
                if (!vert)
                {
                    c1 = std::max(sx1, (int)vp.left);
                    c2 = std::min(sx2, (int)vp.right);
                    baseY = (sqRot == 0) ? std::min(sy2 + amplitude + gap, (int)vp.bottom) : sy1 - amplitude - gap;
                    if (c2 <= c1 || baseY < vp.top || baseY > vp.bottom) continue;
                }
                else
                {
                    c1 = std::max(sy1, (int)vp.top);
                    c2 = std::min(sy2, (int)vp.bottom);
                    baseX = (sqRot == 1) ? sx1 - amplitude - gap : sx2 + amplitude + gap;
                    if (c2 <= c1 || baseX < vp.left || baseX > vp.right) continue;
                }

                int w2 = c2 - c1 + 1;
                const int bufW = vert ? h2 : w2;
                const int bufH = vert ? w2 : h2;

                // 버퍼 재사용: 크기가 충분하지 않을 때만 재할당
                if (bufW > sqBmpW || bufH > sqBmpH)
                {
                    if (sqBmp) { SelectObject(sqDC, sqOld); DeleteObject(sqBmp); }
                    sqBmp  = CreateCompatibleBitmap(hdc, bufW, bufH);
                    sqOld  = static_cast<HBITMAP>(SelectObject(sqDC, sqBmp));
                    sqBmpW = bufW; sqBmpH = bufH;
                }

                RECT fr2 = { 0, 0, bufW, bufH };
                FillRect(sqDC, &fr2, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                HPEN hPen2 = CreatePen(PS_SOLID, thickness, sq.color);
                HPEN oPen2 = static_cast<HPEN>(SelectObject(sqDC, hPen2));
                std::vector<POINT> pts2;
                pts2.reserve(w2);
                for (int px = 0; px < w2; ++px)
                {
                    double phase = (double)px / waveLen * 2.0 * 3.14159265;
                    int py = amplitude + (int)(amplitude * std::sin(phase));
                    if (vert) pts2.push_back({ py, px });
                    else      pts2.push_back({ px, py });
                }
                if (pts2.size() >= 2)
                    Polyline(sqDC, pts2.data(), (int)pts2.size());
                SelectObject(sqDC, oPen2);
                DeleteObject(hPen2);
                if (vert)
                    AlphaBlend(hdc, baseX - amplitude - thickness, c1, bufW, bufH,
                        sqDC, 0, 0, bufW, bufH, sqBf);
                else
                    AlphaBlend(hdc, c1, baseY - amplitude - thickness, bufW, bufH,
                        sqDC, 0, 0, bufW, bufH, sqBf);

                if (isSelected)
                {
                    HPEN selPen = CreatePen(PS_DASH, 1, RGB(40, 120, 255));
                    HPEN oSel = static_cast<HPEN>(SelectObject(hdc, selPen));
                    HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
                    Rectangle(hdc, sx1 - 1, sy1 - 1, sx2 + 1, sy2 + 1);
                    SelectObject(hdc, oBr);
                    SelectObject(hdc, oSel);
                    DeleteObject(selPen);
                }
            }
        }
    }

    if (sqBmp) { SelectObject(sqDC, sqOld); DeleteObject(sqBmp); }
    DeleteDC(sqDC);

    for (auto& kv : pageCache) {
        if (kv.second.second) FPDFText_ClosePage(kv.second.second);
        if (kv.second.first)  FPDF_ClosePage(kv.second.first);
    }
}

// 선택 영역으로 물결 밑줄 추가
static void AddSquigglyFromSelection(COLORREF color)
{
    if (!g_hasSelection || g_selCharStart < 0 || g_selCharEnd < 0) return;
    if (g_selCharStart == g_selCharEnd) return;

    SquigglyItem sq;
    sq.pageIndex = g_currentPage;
    sq.charStart = std::min(g_selCharStart, g_selCharEnd);
    sq.charEnd   = std::max(g_selCharStart, g_selCharEnd);
    sq.color     = color;
    g_squigglies.push_back(sq);
    g_dirty = true;
    g_hasSelection = false;
    g_selCharStart = g_selCharEnd = -1;
    g_selPage = -1;
}

// 특정 위치의 물결 밑줄 찾기
static int HitTestSquiggly(int pageIndex, int charIdx)
{
    for (int i = static_cast<int>(g_squigglies.size()) - 1; i >= 0; --i)
    {
        const auto& sq = g_squigglies[i];
        if (sq.pageIndex == pageIndex)
        {
            int lo = std::min(sq.charStart, sq.charEnd);
            int hi = std::max(sq.charStart, sq.charEnd);
            if (charIdx >= lo && charIdx <= hi)
                return i;
        }
    }
    return -1;
}

static void LoadHighlightAnnotations(AnnotLoadResult* out, FPDF_DOCUMENT doc)
{
    if (!doc) doc = g_pdfDoc;
    if (!doc) return;
    const int pageCount = FPDF_GetPageCount(doc);
    if (pageCount <= 0) return;

    // out이 있으면 로컬 벡터에 수집 (백그라운드 스레드), 없으면 전역 직접 쓰기 (동기)
    std::vector<HighlightItem>& highlights = out ? out->highlights : g_highlights;
    std::vector<UnderlineItem>& underlines = out ? out->underlines : g_underlines;
    std::vector<StrikeoutItem>& strikeouts = out ? out->strikeouts : g_strikeouts;
    std::vector<SquigglyItem>&  squigglies = out ? out->squigglies : g_squigglies;

    if (!out)
    {
        highlights.clear();
        g_selectedHighlight = -1;
        underlines.clear();
        g_selectedUnderline = -1;
        strikeouts.clear();
        g_selectedStrikeout = -1;
        squigglies.clear();
        g_selectedSquiggly = -1;
    }

    // 모든 페이지의 주석을 순회하여 로드
    for (int pi = 0; pi < pageCount; ++pi)
    {
        // 백그라운드 스레드(out != nullptr)에서는 전용 doc만 사용, g_pdfPage 절대 공유 안 함
        bool isCurrentPage = !out && (pi == g_currentPage && g_pdfPage);
        FPDF_PAGE page = isCurrentPage ? g_pdfPage : FPDF_LoadPage(doc, pi);
        if (!page) continue;
        FPDF_TEXTPAGE textPage = isCurrentPage ? g_textPage : FPDFText_LoadPage(page);
        if (!textPage)
        {
            if (!isCurrentPage) FPDF_ClosePage(page);
            continue;
        }

        const int annotCount = FPDFPage_GetAnnotCount(page);
        if (annotCount <= 0)
        {
            if (!isCurrentPage) { FPDFText_ClosePage(textPage); FPDF_ClosePage(page); }
            continue;
        }

        // 메모리로 읽은 뒤 제거할 어노테이션 인덱스 (역순 제거를 위해 수집)
        std::vector<int> annotsToRemove;

        for (int ai = 0; ai < annotCount; ++ai)
        {
            FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, ai);
            if (!annot) continue;

            FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
            if (subtype != FPDF_ANNOT_HIGHLIGHT && subtype != FPDF_ANNOT_UNDERLINE && subtype != FPDF_ANNOT_STRIKEOUT && subtype != FPDF_ANNOT_SQUIGGLY)
            {
                FPDFPage_CloseAnnot(annot);
                continue;
            }

            // 색상 추출
            unsigned int R = 255, G = 255, B = 80, A = 150;
            FPDFAnnot_GetColor(annot, FPDFANNOT_COLORTYPE_Color, &R, &G, &B, &A);
            COLORREF color = RGB(R, G, B);

            bool isUnderline = (subtype == FPDF_ANNOT_UNDERLINE);
            bool isStrikeout = (subtype == FPDF_ANNOT_STRIKEOUT);
            bool isSquiggly  = (subtype == FPDF_ANNOT_SQUIGGLY);

            // QuadPoints에서 문자 인덱스 범위 계산
            size_t qpCount = FPDFAnnot_CountAttachmentPoints(annot);
            if (qpCount == 0)
            {
                FPDFPage_CloseAnnot(annot);
                continue;
            }

            // 각 QuadPoint를 개별 처리하여 문자 인덱스 수집
            int foundStart = INT_MAX, foundEnd = INT_MIN;
            for (size_t qi = 0; qi < qpCount; ++qi)
            {
                FS_QUADPOINTSF qp;
                if (!FPDFAnnot_GetAttachmentPoints(annot, qi, &qp)) continue;

                // 쿼드의 좌측·우측·중앙 좌표 계산
                double qMinX = std::min({ (double)qp.x1, (double)qp.x2, (double)qp.x3, (double)qp.x4 });
                double qMaxX = std::max({ (double)qp.x1, (double)qp.x2, (double)qp.x3, (double)qp.x4 });
                double qMinY = std::min({ (double)qp.y1, (double)qp.y2, (double)qp.y3, (double)qp.y4 });
                double qMaxY = std::max({ (double)qp.y1, (double)qp.y2, (double)qp.y3, (double)qp.y4 });
                double cx = (qMinX + qMaxX) * 0.5;
                double cy = (qMinY + qMaxY) * 0.5;

                // 여러 위치(중앙, 좌상, 우하, 좌하, 우상)에서 문자 검색
                struct { double x, y; } probes[] = {
                    { cx, cy },             // 중앙
                    { qMinX, qMaxY },       // 좌상
                    { qMaxX, qMinY },       // 우하
                    { qMinX, qMinY },       // 좌하
                    { qMaxX, qMaxY },       // 우상
                };

                for (auto& p : probes)
                {
                    for (double tol : { 5.0, 15.0, 30.0 })
                    {
                        int ci = FPDFText_GetCharIndexAtPos(textPage, p.x, p.y, tol, tol);
                        if (ci >= 0)
                        {
                            if (ci < foundStart) foundStart = ci;
                            if (ci > foundEnd)   foundEnd = ci;
                            break;
                        }
                    }
                }
            }

            if (foundStart <= foundEnd && foundStart != INT_MAX)
            {
                if (isStrikeout)
                {
                    StrikeoutItem so{};
                    so.pageIndex = pi;
                    so.charStart = std::min(foundStart, foundEnd);
                    so.charEnd   = std::max(foundStart, foundEnd);
                    so.color     = color;
                    strikeouts.push_back(so);
                }
                else if (isSquiggly)
                {
                    SquigglyItem sq;
                    sq.pageIndex = pi;
                    sq.charStart = std::min(foundStart, foundEnd);
                    sq.charEnd   = std::max(foundStart, foundEnd);
                    sq.color     = color;
                    squigglies.push_back(sq);
                }
                else if (isUnderline)
                {
                    UnderlineItem ul{};
                    ul.pageIndex = pi;
                    ul.charStart = std::min(foundStart, foundEnd);
                    ul.charEnd   = std::max(foundStart, foundEnd);
                    ul.color     = color;
                    underlines.push_back(ul);
                }
                else
                {
                    HighlightItem hl;
                    hl.pageIndex = pi;
                    hl.charStart = std::min(foundStart, foundEnd);
                    hl.charEnd   = std::max(foundStart, foundEnd);
                    hl.color     = color;
                    highlights.push_back(hl);
                }
                annotsToRemove.push_back(ai);
            }

            FPDFPage_CloseAnnot(annot);
        }

        // 역순으로 PDF 어노테이션 제거 (인덱스 밀림 방지)
        for (int i = static_cast<int>(annotsToRemove.size()) - 1; i >= 0; --i)
            FPDFPage_RemoveAnnot(page, annotsToRemove[i]);

        if (!annotsToRemove.empty())
            FPDFPage_GenerateContent(page);

        // 임시 로드한 페이지 정리
        if (!isCurrentPage)
        {
            FPDFText_ClosePage(textPage);
            FPDF_ClosePage(page);
        }
    }
}

// ─── 메모(Note) 기능 ──────────────────────────────────────────────────────────

#define NOTE_ICON_SIZE 32  // 화면에 표시되는 노트 아이콘 크기 (px)

// 노트 아이콘을 화면에 그리기
static void DrawNotes(HDC hdc, const RECT& clientRect)
{
    if (g_notes.empty() || !g_pdfDoc || g_pageLayouts.empty()) return;
    const RECT vp = GetViewportRect(clientRect);

    for (int ni = 0; ni < static_cast<int>(g_notes.size()); ++ni)
    {
        const auto& note = g_notes[ni];
        if (note.pageIndex < 0 || note.pageIndex >= g_pageCount) continue;

        int pgLeft, pgTop;
        GetPageScreenPos(note.pageIndex, vp, &pgLeft, &pgTop);

        const double pageW = g_pageLayouts[note.pageIndex].pdfW;
        const double pageH = g_pageLayouts[note.pageIndex].pdfH;
        int sx, sy;
        PdfToScreen(note.pdfX, note.pdfY, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(note.pageIndex), &sx, &sy);

        // 화면 밖이면 스킵
        if (sx < vp.left - NOTE_ICON_SIZE || sx > vp.right + NOTE_ICON_SIZE) continue;
        if (sy < vp.top - NOTE_ICON_SIZE  || sy > vp.bottom + NOTE_ICON_SIZE) continue;

        bool selected = (ni == g_selectedNote);
        bool hover    = (ni == g_hoverNote);
        BYTE alpha = selected ? 230 : (hover ? 220 : 180);

        RECT iconRc = { sx, sy - NOTE_ICON_SIZE, sx + NOTE_ICON_SIZE, sy };
        int iw = NOTE_ICON_SIZE, ih = NOTE_ICON_SIZE;

        // 오프스크린 DC에 아이콘을 그린 후 AlphaBlend로 합성
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, iw, ih);
        HBITMAP oBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

        // 배경을 현재 화면 내용으로 채워 AlphaBlend 기준색 설정
        BitBlt(memDC, 0, 0, iw, ih, hdc, iconRc.left, iconRc.top, SRCCOPY);

        // 아이콘 이미지 또는 이모지 폴백
        if (g_pMemoBitmap && g_pMemoBitmap->GetLastStatus() == Gdiplus::Ok)
        {
            // PNG만 그림 — 배경/테두리 없음
            Gdiplus::Graphics gfx(memDC);
            gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            gfx.DrawImage(g_pMemoBitmap, 0, 0, iw, ih);
        }
        else
        {
            // memo.png 없을 때 폴백: 노란 배경 + 📝 이모지
            COLORREF bgColor = selected ? RGB(255, 220, 60) : (hover ? RGB(255, 245, 140) : RGB(255, 235, 80));
            COLORREF bdColor = selected ? RGB(180, 130, 0) : RGB(200, 160, 20);
            HPEN hPen = CreatePen(PS_SOLID, selected ? 2 : 1, bdColor);
            HBRUSH hBr = CreateSolidBrush(bgColor);
            HPEN oPen = static_cast<HPEN>(SelectObject(memDC, hPen));
            HBRUSH oBr = static_cast<HBRUSH>(SelectObject(memDC, hBr));
            RoundRect(memDC, 0, 0, iw, ih, 5, 5);
            SelectObject(memDC, oPen); SelectObject(memDC, oBr);
            DeleteObject(hPen); DeleteObject(hBr);

            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(80, 60, 0));
            HFONT hF = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT oF = static_cast<HFONT>(SelectObject(memDC, hF));
            RECT localRc = { 0, 0, iw, ih };
            DrawTextW(memDC, L"\U0001F4DD", -1, &localRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(memDC, oF); DeleteObject(hF);
        }

        BLENDFUNCTION bf = {};
        bf.BlendOp             = AC_SRC_OVER;
        bf.SourceConstantAlpha = alpha;
        bf.AlphaFormat         = 0;
        AlphaBlend(hdc, iconRc.left, iconRc.top, iw, ih, memDC, 0, 0, iw, ih, bf);

        SelectObject(memDC, oBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
    }
    
    // hover 중인 노트 내용을 말풍선으로 표시
    if (g_hoverNote >= 0 && g_hoverNote < static_cast<int>(g_notes.size()))
    {
        const auto& hn = g_notes[g_hoverNote];
        if (hn.pageIndex >= 0 && hn.pageIndex < g_pageCount)
        {
            int pgLeft, pgTop;
            GetPageScreenPos(hn.pageIndex, vp, &pgLeft, &pgTop);
            const double pageW = g_pageLayouts[hn.pageIndex].pdfW;
            const double pageH = g_pageLayouts[hn.pageIndex].pdfH;
            int sx, sy;
            PdfToScreen(hn.pdfX, hn.pdfY, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(hn.pageIndex), &sx, &sy);

            const std::wstring& txt = hn.text.empty() ? std::wstring(L"(내용 없음)") : hn.text;

            HFONT hTF = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT oTF = static_cast<HFONT>(SelectObject(hdc, hTF));

            // 텍스트 크기 측정
            RECT measureRc = { 0, 0, 280, 0 };
            DrawTextW(hdc, txt.c_str(), -1, &measureRc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            const int pad = 8;
            int bw = measureRc.right - measureRc.left + pad * 2;
            int bh = measureRc.bottom - measureRc.top + pad * 2;

            // 말풍선 위치: 아이콘 오른쪽, 화면 벗어나면 왼쪽
            int bx = sx + NOTE_ICON_SIZE + 4;
            int by = sy - NOTE_ICON_SIZE;
            if (bx + bw > clientRect.right - 4)  bx = sx - bw - 4;
            if (by + bh > clientRect.bottom - 4)  by = clientRect.bottom - bh - 4;
            if (by < clientRect.top + 4)           by = clientRect.top + 4;

            RECT boxRc = { bx, by, bx + bw, by + bh };

            // 그림자
            RECT shadowRc = { boxRc.left + 3, boxRc.top + 3, boxRc.right + 3, boxRc.bottom + 3 };
            HBRUSH hShadow = CreateSolidBrush(RGB(180, 180, 180));
            HPEN hNoPen = CreatePen(PS_NULL, 0, 0);
            HPEN oSP = static_cast<HPEN>(SelectObject(hdc, hNoPen));
            HBRUSH oSB = static_cast<HBRUSH>(SelectObject(hdc, hShadow));
            RoundRect(hdc, shadowRc.left, shadowRc.top, shadowRc.right, shadowRc.bottom, 6, 6);
            SelectObject(hdc, oSP); SelectObject(hdc, oSB);
            DeleteObject(hShadow); DeleteObject(hNoPen);

            // 배경 + 테두리
            HBRUSH hBgBr = CreateSolidBrush(RGB(255, 252, 220));
            HPEN   hBdPen = CreatePen(PS_SOLID, 1, RGB(180, 150, 0));
            HPEN   oBP = static_cast<HPEN>(SelectObject(hdc, hBdPen));
            HBRUSH oBB = static_cast<HBRUSH>(SelectObject(hdc, hBgBr));
            RoundRect(hdc, boxRc.left, boxRc.top, boxRc.right, boxRc.bottom, 6, 6);
            SelectObject(hdc, oBP); SelectObject(hdc, oBB);
            DeleteObject(hBgBr); DeleteObject(hBdPen);

            // 텍스트
            RECT textRc = { boxRc.left + pad, boxRc.top + pad, boxRc.right - pad, boxRc.bottom - pad };
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(40, 30, 0));
            DrawTextW(hdc, txt.c_str(), -1, &textRc, DT_WORDBREAK | DT_NOPREFIX);

            SelectObject(hdc, oTF);
            DeleteObject(hTF);
        }
    }
}

// 화면 좌표에서 노트 인덱스 찾기
static int HitTestNote(int screenX, int screenY, const RECT& vp)
{
    if (g_notes.empty() || !g_pdfDoc || g_pageLayouts.empty()) return -1;
    for (int ni = static_cast<int>(g_notes.size()) - 1; ni >= 0; --ni)
    {
        const auto& note = g_notes[ni];
        if (note.pageIndex < 0 || note.pageIndex >= g_pageCount) continue;

        int pgLeft, pgTop;
        GetPageScreenPos(note.pageIndex, vp, &pgLeft, &pgTop);
        const double pageW = g_pageLayouts[note.pageIndex].pdfW;
        const double pageH = g_pageLayouts[note.pageIndex].pdfH;
        int sx, sy;
        PdfToScreen(note.pdfX, note.pdfY, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(note.pageIndex), &sx, &sy);

        RECT hitRc = { sx - 2, sy - NOTE_ICON_SIZE - 2, sx + NOTE_ICON_SIZE + 2, sy + 2 };
        POINT pt = { screenX, screenY };
        if (PtInRect(&hitRc, pt)) return ni;
    }
    return -1;
}

// ─── 텍스트 박스 ─────────────────────────────────────────────────────────────

static RECT TextBoxScreenRect(const TextBoxItem& tb)
{
    if (tb.pageIndex < 0 || tb.pageIndex >= g_pageCount || g_pageLayouts.empty())
        return {};
    RECT cr; GetClientRect(g_hMainWnd, &cr);
    const RECT vp = GetViewportRect(cr);
    int pgLeft, pgTop;
    GetPageScreenPos(tb.pageIndex, vp, &pgLeft, &pgTop);
    double pageW = g_pageLayouts[tb.pageIndex].pdfW, pageH = g_pageLayouts[tb.pageIndex].pdfH;
    int sx, sy, sx2, sy2;
    PdfToScreen(tb.pdfX,           tb.pdfY,           pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(tb.pageIndex), &sx,  &sy);
    PdfToScreen(tb.pdfX + tb.pdfW, tb.pdfY - tb.pdfH, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(tb.pageIndex), &sx2, &sy2);
    RECT r = { std::min(sx,sx2), std::min(sy,sy2), std::max(sx,sx2), std::max(sy,sy2) };
    return r;
}

// ─── 텍스트 박스 크기 조절 핸들 ──────────────────────────────────────────────
// 핸들 번호: 0=TL, 1=TC, 2=TR, 3=ML, 4=MR, 5=BL, 6=BC, 7=BR
static const int HANDLE_SZ = 7;

static RECT TextBoxHandleRect(const RECT& r, int h)
{
    const int hs  = HANDLE_SZ;
    const int cx  = (r.left + r.right)  / 2 - hs / 2;
    const int cy  = (r.top  + r.bottom) / 2 - hs / 2;
    switch (h)
    {
    case 0: return { r.left  - hs/2,     r.top    - hs/2,     r.left  + hs/2 + 1, r.top    + hs/2 + 1 };
    case 1: return { cx,                  r.top    - hs/2,     cx + hs,             r.top    + hs/2 + 1 };
    case 2: return { r.right - hs/2,     r.top    - hs/2,     r.right + hs/2 + 1, r.top    + hs/2 + 1 };
    case 3: return { r.left  - hs/2,     cy,                  r.left  + hs/2 + 1, cy + hs             };
    case 4: return { r.right - hs/2,     cy,                  r.right + hs/2 + 1, cy + hs             };
    case 5: return { r.left  - hs/2,     r.bottom - hs/2,     r.left  + hs/2 + 1, r.bottom + hs/2 + 1};
    case 6: return { cx,                  r.bottom - hs/2,     cx + hs,             r.bottom + hs/2 + 1};
    case 7: return { r.right - hs/2,     r.bottom - hs/2,     r.right + hs/2 + 1, r.bottom + hs/2 + 1};
    }
    return {};
}

static int HitTestTextBoxHandle(const RECT& r, int screenX, int screenY)
{
    POINT pt = { screenX, screenY };
    for (int h = 0; h < 8; ++h)
    {
        RECT hr = TextBoxHandleRect(r, h);
        InflateRect(&hr, 2, 2);
        if (PtInRect(&hr, pt)) return h;
    }
    return -1;
}

static LPCTSTR HandleCursor(int h)
{
    switch (h)
    {
    case 0: case 7: return IDC_SIZENWSE;
    case 2: case 5: return IDC_SIZENESW;
    case 1: case 6: return IDC_SIZENS;
    case 3: case 4: return IDC_SIZEWE;
    }
    return IDC_SIZEALL;
}

static void DrawTextBoxes(HDC hdc, const RECT& clientRect)
{
    if (g_textBoxes.empty() || !g_pdfDoc || g_pageLayouts.empty()) return;

    for (int ti = 0; ti < (int)g_textBoxes.size(); ++ti)
    {
        const auto& tb = g_textBoxes[ti];
        RECT r = TextBoxScreenRect(tb);
        if (r.right <= r.left || r.bottom <= r.top) continue;

        bool selected = (ti == g_selectedTextBox);
        bool editing  = (ti == g_editingTextBox);

        int bw = r.right - r.left;
        int bh = r.bottom - r.top;

        // 반투명 배경 — 편집 중에도 항상 그려서 부모 BitBlt 시 노랑이 유지됨
        {
            BITMAPINFOHEADER bmi = {};
            bmi.biSize        = sizeof(bmi);
            bmi.biWidth       = bw;
            bmi.biHeight      = -bh; // top-down
            bmi.biPlanes      = 1;
            bmi.biBitCount    = 32;
            bmi.biCompression = BI_RGB;
            void* pBits = nullptr;
            HBITMAP hDib = CreateDIBSection(nullptr, (BITMAPINFO*)&bmi,
                                            DIB_RGB_COLORS, &pBits, nullptr, 0);
            if (hDib && pBits)
            {
                const BYTE A = 220;
                const BYTE R = (BYTE)(255 * A / 255);
                const BYTE G = (BYTE)(255 * A / 255);
                const BYTE B = (BYTE)(160 * A / 255);
                BYTE* p = (BYTE*)pBits;
                for (int i = 0; i < bw * bh; i++)
                {
                    p[i * 4 + 0] = B; // Blue
                    p[i * 4 + 1] = G; // Green
                    p[i * 4 + 2] = R; // Red
                    p[i * 4 + 3] = A; // Alpha
                }
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hDib);
                BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                AlphaBlend(hdc, r.left, r.top, bw, bh,
                           hdcMem, 0, 0, bw, bh, bf);
                SelectObject(hdcMem, hOld);
                DeleteDC(hdcMem);
                DeleteObject(hDib);
            }
        }

        // 텍스트 — 편집 중이 아닐 때만 직접 렌더링 (편집 중에는 Edit 컨트롤이 담당)
        if (!editing && !tb.text.empty())
        {
            HFONT hFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT oFont = (HFONT)SelectObject(hdc, hFont);
            SetTextColor(hdc, RGB(30, 30, 30));
            SetBkMode(hdc, TRANSPARENT);
            RECT textRc = { r.left + 4, r.top + 4, r.right - 4, r.bottom - 4 };
            DrawTextW(hdc, tb.text.c_str(), -1, &textRc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
            SelectObject(hdc, oFont);
            DeleteObject(hFont);
        }

        // 테두리 (토마토색) — 편집 중에도 항상 표시
        HPEN hPen = CreatePen(PS_SOLID, (selected || editing) ? 2 : 1,
            (selected || editing) ? RGB(255, 80, 60) : RGB(220, 80, 60));
        HPEN oPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH oBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, oPen); SelectObject(hdc, oBr);
        DeleteObject(hPen);

        // 선택된 박스(편집 중 아닐 때)만 크기 조절 핸들 그리기
        if (selected && !editing)
        {
            HPEN hpH   = CreatePen(PS_SOLID, 1, RGB(255, 80, 60));
            HBRUSH hbH = CreateSolidBrush(RGB(255, 255, 255));
            HPEN   opH = (HPEN)SelectObject(hdc, hpH);
            HBRUSH obH = (HBRUSH)SelectObject(hdc, hbH);
            for (int h = 0; h < 8; ++h)
            {
                RECT hr = TextBoxHandleRect(r, h);
                Rectangle(hdc, hr.left, hr.top, hr.right, hr.bottom);
            }
            SelectObject(hdc, opH); SelectObject(hdc, obH);
            DeleteObject(hpH); DeleteObject(hbH);
        }
    }
}

static int HitTestTextBox(int screenX, int screenY, const RECT& /*vp*/)
{
    for (int ti = (int)g_textBoxes.size() - 1; ti >= 0; --ti)
    {
        RECT r = TextBoxScreenRect(g_textBoxes[ti]);
        POINT pt = { screenX, screenY };
        if (PtInRect(&r, pt)) return ti;
    }
    return -1;
}

static void CommitTextBoxEdit()
{
    if (!g_hTextBoxEdit || g_editingTextBox < 0) return;
    // 재진입 방지
    HWND hEdit = g_hTextBoxEdit;
    int  idx   = g_editingTextBox;
    g_hTextBoxEdit   = nullptr;
    g_editingTextBox = -1;

    WCHAR buf[2048] = {};
    GetWindowTextW(hEdit, buf, _countof(buf));
    if (idx < (int)g_textBoxes.size())
    {
        g_textBoxes[idx].text = buf;
        g_dirty = true;
    }
    DestroyWindow(hEdit);
    if (g_hMainWnd) { RECT cr; GetClientRect(g_hMainWnd, &cr); cr.top = TOOLBAR_HEIGHT; InvalidateRect(g_hMainWnd, &cr, FALSE); }
}

static LRESULT CALLBACK TextBoxEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    WNDPROC orig = (WNDPROC)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

    // Edit 컨트롤 배경 지우기 차단 — 깜빡임 방지 (부모가 이미 반투명 노랑을 백버퍼에 그림)
    if (msg == WM_ERASEBKGND)
        return 1;

    // 모든 키 입력을 Edit 컨트롤이 직접 받도록 처리
    if (msg == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;

    if (msg == WM_KEYDOWN)
    {
        if (wParam == VK_ESCAPE)
        {
            CommitTextBoxEdit();
            return 0;
        }
    }
    if (msg == WM_KILLFOCUS)
    {
        CommitTextBoxEdit();
        return 0;
    }
    // 편집 컨트롤 위에서 마우스 클릭 시 편집 종료 후 부모로 메시지 전달 (이동 가능하게)
    if (msg == WM_LBUTTONDOWN)
    {
        HWND parent = GetParent(hWnd);
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &pt);
        ScreenToClient(parent, &pt);
        CommitTextBoxEdit();
        SendMessage(parent, WM_LBUTTONDOWN, wParam, MAKELPARAM(pt.x, pt.y));
        return 0;
    }
    return CallWindowProcW(orig, hWnd, msg, wParam, lParam);
}

static void StartTextBoxEdit(HWND hWnd, int tbIdx)
{
    if (tbIdx < 0 || tbIdx >= (int)g_textBoxes.size()) return;
    CommitTextBoxEdit(); // 기존 편집 마무리

    RECT r = TextBoxScreenRect(g_textBoxes[tbIdx]);
    if (r.right <= r.left || r.bottom <= r.top) return;

    g_editingTextBox = tbIdx;
    // 테두리(2px)만큼 안쪽에 Edit 배치 → Rectangle 테두리가 가려지지 않음
    const int B = 2;
    g_hTextBoxEdit = CreateWindowExW(
        0, L"EDIT", g_textBoxes[tbIdx].text.c_str(),
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        r.left + B, r.top + B, (r.right - r.left) - B * 2, (r.bottom - r.top) - B * 2,
        hWnd, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!g_hTextBoxEdit) { g_editingTextBox = -1; return; }

    SetWindowPos(g_hTextBoxEdit, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    HFONT hFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessage(g_hTextBoxEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

    // 서브클래스로 ESC/포커스 잃음 처리
    WNDPROC orig = (WNDPROC)SetWindowLongPtrW(g_hTextBoxEdit, GWLP_WNDPROC, (LONG_PTR)TextBoxEditProc);
    SetWindowLongPtrW(g_hTextBoxEdit, GWLP_USERDATA, (LONG_PTR)orig);

    SetFocus(g_hTextBoxEdit);
    // 커서를 텍스트 끝으로 (전체 선택 없이)
    int len = GetWindowTextLengthW(g_hTextBoxEdit);
    SendMessage(g_hTextBoxEdit, EM_SETSEL, len, len);
}

static void AddTextBoxAtPdfPos(HWND hWnd, int pageIndex, double pdfX, double pdfY)
{
    TextBoxItem tb;
    tb.pageIndex = pageIndex;
    tb.pdfX = pdfX;
    tb.pdfY = pdfY;
    tb.pdfW = 150.0;
    tb.pdfH = 60.0;
    tb.text = L"";
    g_textBoxes.push_back(tb);
    g_selectedTextBox = (int)g_textBoxes.size() - 1;
    g_dirty = true;
    RECT cr; GetClientRect(hWnd, &cr); cr.top = TOOLBAR_HEIGHT;
    InvalidateRect(hWnd, &cr, FALSE);
}

// 간단한 메모 입력 다이얼로그 — DialogBoxParam 표준 방식
// ─── 서명 헬퍼 ───────────────────────────────────────────────────────────────

static RECT SigScreenRect(const SignatureItem& sig)
{
    if (sig.pageIndex < 0 || sig.pageIndex >= g_pageCount || g_pageLayouts.empty())
        return {};
    RECT cr; GetClientRect(g_hMainWnd, &cr);
    const RECT vp = GetViewportRect(cr);
    int pgLeft, pgTop;
    GetPageScreenPos(sig.pageIndex, vp, &pgLeft, &pgTop);
    double pageW = g_pageLayouts[sig.pageIndex].pdfW;
    double pageH = g_pageLayouts[sig.pageIndex].pdfH;
    int sx, sy, sx2, sy2;
    PdfToScreen(sig.pdfX,            sig.pdfY,            pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(sig.pageIndex), &sx,  &sy);
    PdfToScreen(sig.pdfX + sig.pdfW, sig.pdfY - sig.pdfH, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(sig.pageIndex), &sx2, &sy2);
    RECT r = { std::min(sx,sx2), std::min(sy,sy2), std::max(sx,sx2), std::max(sy,sy2) };
    return r;
}

static int HitTestSigHandle(const RECT& r, int sx, int sy)
{
    constexpr int HS = 7;
    int cx = (r.left + r.right)  / 2 - HS/2;
    int cy = (r.top  + r.bottom) / 2 - HS/2;
    RECT handles[8] = {
        { r.left  - HS/2,     r.top    - HS/2,     r.left  + HS/2+1, r.top    + HS/2+1 },
        { cx,                  r.top    - HS/2,     cx+HS,             r.top    + HS/2+1 },
        { r.right - HS/2,     r.top    - HS/2,     r.right + HS/2+1, r.top    + HS/2+1 },
        { r.left  - HS/2,     cy,                  r.left  + HS/2+1, cy+HS             },
        { r.right - HS/2,     cy,                  r.right + HS/2+1, cy+HS             },
        { r.left  - HS/2,     r.bottom - HS/2,     r.left  + HS/2+1, r.bottom + HS/2+1},
        { cx,                  r.bottom - HS/2,     cx+HS,             r.bottom + HS/2+1},
        { r.right - HS/2,     r.bottom - HS/2,     r.right + HS/2+1, r.bottom + HS/2+1},
    };
    for (int i = 0; i < 8; i++) {
        POINT pt = { sx, sy };
        if (PtInRect(&handles[i], pt)) return i;
    }
    return -1;
}

static int HitTestSignature(int screenX, int screenY, const RECT& vp)
{
    POINT pt = { screenX, screenY };
    for (int i = (int)g_signatures.size() - 1; i >= 0; i--) {
        RECT r = SigScreenRect(g_signatures[i]);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

static void DrawRedacts(HDC hdc, const RECT& clientRect)
{
    if (!g_pdfDoc || g_pageLayouts.empty()) return;

    RECT vp = GetViewportRect(clientRect);
    Gdiplus::Graphics g(hdc);

    // 저장된 redact 박스 그리기
    for (int ri = 0; ri < (int)g_redacts.size(); ri++)
    {
        const auto& rd = g_redacts[ri];
        if (rd.pageIndex < 0 || rd.pageIndex >= (int)g_pageLayouts.size()) continue;
        int pgL, pgT;
        GetPageScreenPos(rd.pageIndex, vp, &pgL, &pgT);
        double pageW = g_pageLayouts[rd.pageIndex].pdfW;
        double pageH = g_pageLayouts[rd.pageIndex].pdfH;
        int sxA, syA, sxB, syB;
        PdfToScreen(rd.pdfX,           rd.pdfY,           pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(rd.pageIndex), &sxA, &syA);
        PdfToScreen(rd.pdfX + rd.pdfW, rd.pdfY - rd.pdfH, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(rd.pageIndex), &sxB, &syB);
        int sx1 = std::min(sxA, sxB), sy1 = std::min(syA, syB);
        int sx2 = std::max(sxA, sxB), sy2 = std::max(syA, syB);

        // 반투명 빨간 채우기
        Gdiplus::SolidBrush fillBr(Gdiplus::Color(120, 220, 20, 20));
        g.FillRectangle(&fillBr, sx1, sy1, sx2 - sx1, sy2 - sy1);

        // 테두리
        Gdiplus::Color borderCol = (ri == g_selectedRedact)
            ? Gdiplus::Color(255, 255, 0, 0)
            : Gdiplus::Color(200, 180, 0, 0);
        Gdiplus::Pen pen(borderCol, ri == g_selectedRedact ? 2.f : 1.f);
        g.DrawRectangle(&pen, sx1, sy1, sx2 - sx1, sy2 - sy1);
    }

    // 드래그 중인 새 영역 미리보기
    if (g_isDraggingRedact)
    {
        int x1 = std::min(g_redactDragStartX, g_redactDragCurX);
        int y1 = std::min(g_redactDragStartY, g_redactDragCurY);
        int x2 = std::max(g_redactDragStartX, g_redactDragCurX);
        int y2 = std::max(g_redactDragStartY, g_redactDragCurY);
        Gdiplus::SolidBrush previewBr(Gdiplus::Color(80, 220, 20, 20));
        g.FillRectangle(&previewBr, x1, y1, x2 - x1, y2 - y1);
        Gdiplus::Pen previewPen(Gdiplus::Color(220, 200, 0, 0), 1.f);
        previewPen.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawRectangle(&previewPen, x1, y1, x2 - x1, y2 - y1);
    }
}

static void DrawShapes(HDC hdc, const RECT& clientRect)
{
    if (!g_pdfDoc || g_pageLayouts.empty()) return;

    RECT vp = GetViewportRect(clientRect);
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // 저장된 도형 그리기
    for (int i = 0; i < (int)g_shapes.size(); i++)
    {
        const auto& sh = g_shapes[i];
        if (sh.pageIndex < 0 || sh.pageIndex >= (int)g_pageLayouts.size()) continue;
        int pgL, pgT;
        GetPageScreenPos(sh.pageIndex, vp, &pgL, &pgT);
        double pageW = g_pageLayouts[sh.pageIndex].pdfW;
        double pageH = g_pageLayouts[sh.pageIndex].pdfH;
        int sxA, syA, sxB, syB;
        PdfToScreen(sh.pdfX,           sh.pdfY,           pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sh.pageIndex), &sxA, &syA);
        PdfToScreen(sh.pdfX + sh.pdfW, sh.pdfY - sh.pdfH, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sh.pageIndex), &sxB, &syB);
        int sx1 = std::min(sxA, sxB), sy1 = std::min(syA, syB);
        int sx2 = std::max(sxA, sxB), sy2 = std::max(syA, syB);
        int w   = sx2 - sx1, h = sy2 - sy1;

        BYTE r = GetRValue(sh.color), gv = GetGValue(sh.color), b = GetBValue(sh.color);
        bool selected = (i == g_selectedShape);
        float lw = sh.lineWidth * std::sqrt(g_zoom);
        lw = std::max(0.8f, lw);

        // 테두리
        Gdiplus::Pen pen(Gdiplus::Color(selected ? 255 : 210, r, gv, b), selected ? lw + 1.f : lw);
        if (selected) {
            Gdiplus::Pen selPen(Gdiplus::Color(255, 255, 255, 0), lw + 2.f);
            if (sh.isCircle) g.DrawEllipse(&selPen, sx1 - 1, sy1 - 1, w + 2, h + 2);
            else             g.DrawRectangle(&selPen, sx1 - 1, sy1 - 1, w + 2, h + 2);
        }
        if (sh.isCircle)
            g.DrawEllipse(&pen, sx1, sy1, w, h);
        else
            g.DrawRectangle(&pen, sx1, sy1, w, h);
    }

    // 드래그 중인 새 도형 미리보기
    if (g_isDraggingShape)
    {
        int x1 = std::min(g_shapeDragStartX, g_shapeDragCurX);
        int y1 = std::min(g_shapeDragStartY, g_shapeDragCurY);
        int x2 = std::max(g_shapeDragStartX, g_shapeDragCurX);
        int y2 = std::max(g_shapeDragStartY, g_shapeDragCurY);
        int w  = x2 - x1, h = y2 - y1;
        BYTE r = GetRValue(g_shapeColor), gv = GetGValue(g_shapeColor), b = GetBValue(g_shapeColor);
        bool isCircle = (g_shapeMode == 2);
        Gdiplus::Pen previewPen(Gdiplus::Color(200, r, gv, b), g_shapeLineWidth);
        previewPen.SetDashStyle(Gdiplus::DashStyleDash);
        if (isCircle) {
            g.DrawEllipse(&previewPen, x1, y1, w, h);
        } else {
            g.DrawRectangle(&previewPen, x1, y1, w, h);
        }
    }

    // 완료된 잉크 스트로크 오버레이 (페이지 재렌더 전까지 유지)
    if (!g_inkStrokes.empty() && !g_pageLayouts.empty())
    {
        RECT vp0 = GetViewportRect(clientRect);
        for (int si = 0; si < (int)g_inkStrokes.size(); ++si)
        {
            const auto& stroke = g_inkStrokes[si];
            if (stroke.pdfPts.size() < 2) continue;
            if (stroke.pageIndex < 0 || stroke.pageIndex >= (int)g_pageLayouts.size()) continue;
            int pgL, pgT;
            GetPageScreenPos(stroke.pageIndex, vp0, &pgL, &pgT);
            double pageW = g_pageLayouts[stroke.pageIndex].pdfW;
            double pageH = g_pageLayouts[stroke.pageIndex].pdfH;

            bool selected = (si == g_selectedInkStroke);
            BYTE r = GetRValue(stroke.color), gv = GetGValue(stroke.color), b = GetBValue(stroke.color);
            float scaledWidth = stroke.lineWidth * (float)std::sqrt(g_zoom);

            // 선택 시 노란 후광 펜을 먼저 그림
            if (selected)
            {
                Gdiplus::Pen selPen(Gdiplus::Color(220, 255, 220, 0), scaledWidth + 4.0f);
                selPen.SetLineJoin(Gdiplus::LineJoinRound);
                selPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
                for (int i = 1; i < (int)stroke.pdfPts.size(); ++i)
                {
                    int sx1, sy1, sx2, sy2;
                    PdfToScreen(stroke.pdfPts[i-1].first, stroke.pdfPts[i-1].second,
                                pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(stroke.pageIndex), &sx1, &sy1);
                    PdfToScreen(stroke.pdfPts[i].first,   stroke.pdfPts[i].second,
                                pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(stroke.pageIndex), &sx2, &sy2);
                    g.DrawLine(&selPen, (INT)sx1, (INT)sy1, (INT)sx2, (INT)sy2);
                }
            }

            Gdiplus::Pen inkPen(Gdiplus::Color(selected ? 255 : 220, r, gv, b), scaledWidth);
            inkPen.SetLineJoin(Gdiplus::LineJoinRound);
            inkPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);

            for (int i = 1; i < (int)stroke.pdfPts.size(); ++i)
            {
                int sx1, sy1, sx2, sy2;
                PdfToScreen(stroke.pdfPts[i-1].first, stroke.pdfPts[i-1].second,
                            pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(stroke.pageIndex), &sx1, &sy1);
                PdfToScreen(stroke.pdfPts[i].first,   stroke.pdfPts[i].second,
                            pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(stroke.pageIndex), &sx2, &sy2);
                g.DrawLine(&inkPen, (INT)sx1, (INT)sy1, (INT)sx2, (INT)sy2);
            }
        }
    }

    // 진행 중인 잉크 스트로크 오버레이
    if (g_isDrawingInk && g_inkStroke.size() >= 2)
    {
        BYTE r = GetRValue(g_inkColor), gv = GetGValue(g_inkColor), b = GetBValue(g_inkColor);
        float scaledWidth = 2.0f * (float)std::sqrt(g_zoom);
        Gdiplus::Pen inkPen(Gdiplus::Color(220, r, gv, b), scaledWidth);
        inkPen.SetLineJoin(Gdiplus::LineJoinRound);
        inkPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        for (int i = 1; i < (int)g_inkStroke.size(); ++i)
        {
            g.DrawLine(&inkPen,
                (INT)g_inkStroke[i-1].x, (INT)g_inkStroke[i-1].y,
                (INT)g_inkStroke[i].x,   (INT)g_inkStroke[i].y);
        }
    }
}

static void DrawSignatures(HDC hdc, const RECT& clientRect)
{
    if (g_signatures.empty() || !g_pdfDoc || g_pageLayouts.empty()) return;

    RECT vp = GetViewportRect(clientRect);

    for (int si = 0; si < (int)g_signatures.size(); si++)
    {
        const auto& sig = g_signatures[si];
        RECT r = SigScreenRect(sig);
        if (r.right <= r.left || r.bottom <= r.top) continue;

        int sw = r.right  - r.left;
        int sh = r.bottom - r.top;

        // 이미지 서명 그리기
        if (sig.isImage && sig.bitmap) {
            Gdiplus::Graphics g(hdc);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(sig.bitmap,
                Gdiplus::RectF((float)r.left, (float)r.top,
                               (float)sw,     (float)sh));
        }

        // GDI+로 획 데이터 그리기
        if (!sig.strokes.empty()) {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            // 박스 화면 크기 기반으로 선 두께 결정
            double pdfPenW = std::max(sig.pdfW, sig.pdfH) * 0.012; // PDF 단위
            // PDF → 화면 스케일
            double screenScale = (sig.pdfW > 0) ? (double)sw / sig.pdfW : g_zoom;
            float penW = std::max(1.0f, (float)(pdfPenW * screenScale));

            Gdiplus::Pen pen(Gdiplus::Color(255, 20, 20, 20), penW);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);

            // PDF 좌표 → 화면 좌표 변환 함수 (sig 박스 기준)
            int pgLeft, pgTop;
            RECT cr2; GetClientRect(g_hMainWnd, &cr2);
            RECT vp2 = GetViewportRect(cr2);
            GetPageScreenPos(sig.pageIndex, vp2, &pgLeft, &pgTop);
            double pageW2 = g_pageLayouts[sig.pageIndex].pdfW;
            double pageH2 = g_pageLayouts[sig.pageIndex].pdfH;

            for (auto& stroke : sig.strokes) {
                if (stroke.empty()) continue;
                if (stroke.size() == 1) {
                    int sx2, sy2;
                    PdfToScreen(stroke[0].x, stroke[0].y, pageW2, pageH2,
                        pgLeft, pgTop, g_zoom, GetEffectiveRotation(sig.pageIndex), &sx2, &sy2);
                    float r2 = penW / 2.0f;
                    Gdiplus::SolidBrush dot(Gdiplus::Color(255, 20, 20, 20));
                    g.FillEllipse(&dot, (float)sx2 - r2, (float)sy2 - r2, penW, penW);
                    continue;
                }
                for (int i = 1; i < (int)stroke.size(); i++) {
                    int x1, y1, x2, y2;
                    PdfToScreen(stroke[i-1].x, stroke[i-1].y, pageW2, pageH2,
                        pgLeft, pgTop, g_zoom, GetEffectiveRotation(sig.pageIndex), &x1, &y1);
                    PdfToScreen(stroke[i].x,   stroke[i].y,   pageW2, pageH2,
                        pgLeft, pgTop, g_zoom, GetEffectiveRotation(sig.pageIndex), &x2, &y2);
                    g.DrawLine(&pen, x1, y1, x2, y2);
                }
            }
        }

        // 선택 시 테두리 + 핸들
        if (si == g_selectedSig) {
            HPEN pen = CreatePen(PS_DASH, 1, RGB(0, 120, 212));
            HPEN old = (HPEN)SelectObject(hdc, pen);
            HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH ob = (HBRUSH)SelectObject(hdc, nb);
            Rectangle(hdc, r.left, r.top, r.right, r.bottom);
            SelectObject(hdc, old); DeleteObject(pen);
            SelectObject(hdc, ob);

            // 핸들 8개
            constexpr int HS = 7;
            HBRUSH hbr = CreateSolidBrush(RGB(255,255,255));
            HPEN  hpen = CreatePen(PS_SOLID, 1, RGB(0,120,212));
            SelectObject(hdc, hpen);
            SelectObject(hdc, hbr);
            int cx = (r.left+r.right)/2 - HS/2;
            int cy = (r.top+r.bottom)/2 - HS/2;
            RECT handles[8] = {
                {r.left-HS/2,  r.top-HS/2,    r.left+HS/2+1,  r.top+HS/2+1},
                {cx,            r.top-HS/2,    cx+HS,           r.top+HS/2+1},
                {r.right-HS/2, r.top-HS/2,    r.right+HS/2+1, r.top+HS/2+1},
                {r.left-HS/2,  cy,             r.left+HS/2+1,  cy+HS},
                {r.right-HS/2, cy,             r.right+HS/2+1, cy+HS},
                {r.left-HS/2,  r.bottom-HS/2, r.left+HS/2+1,  r.bottom+HS/2+1},
                {cx,            r.bottom-HS/2, cx+HS,           r.bottom+HS/2+1},
                {r.right-HS/2, r.bottom-HS/2, r.right+HS/2+1, r.bottom+HS/2+1},
            };
            for (auto& h : handles) Rectangle(hdc, h.left, h.top, h.right, h.bottom);
            DeleteObject(hbr); DeleteObject(hpen);
        }
    }
}

// ─── 환경 설정 ───────────────────────────────────────────────────────────────

// ── 환경 설정은 Preferences.h / Preferences.cpp 참조 ────────────────────────

// ─── 메모 입력 → CleanPopup::ShowNoteInput 사용 ─────────────────────────────

#if 0 // legacy — replaced by CleanPopup::ShowNoteInput
static INT_PTR CALLBACK NoteDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        auto* p = reinterpret_cast<NoteDialogParam*>(lParam);
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)p);
        SetDlgItemTextW(hDlg, IDC_NOTE_EDIT, p->initialText.c_str());
        SendDlgItemMessageW(hDlg, IDC_NOTE_EDIT, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        // 부모 중앙 배치
        HWND hParent = GetParent(hDlg);
        if (hParent) {
            RECT rp, rd;
            GetWindowRect(hParent, &rp);
            GetWindowRect(hDlg,    &rd);
            int w = rd.right - rd.left, h = rd.bottom - rd.top;
            int x = rp.left + (rp.right  - rp.left - w) / 2;
            int y = rp.top  + (rp.bottom - rp.top  - h) / 2;
            SetWindowPos(hDlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        return TRUE;  // focus를 IDC_NOTE_EDIT으로
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            auto* p = reinterpret_cast<NoteDialogParam*>(GetWindowLongPtrW(hDlg, DWLP_USER));
            if (p) {
                int len = GetWindowTextLengthW(GetDlgItem(hDlg, IDC_NOTE_EDIT));
                p->resultText.resize(len);
                if (len > 0) GetWindowTextW(GetDlgItem(hDlg, IDC_NOTE_EDIT),
                                            p->resultText.data(), len + 1);
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// 전역 노트 다이얼로그 상태
static HWND g_noteWnd  = nullptr;
static NoteDialogParam* g_noteData = nullptr;

// DialogBoxIndirectParam으로 메모 입력창 표시
static bool ShowNoteDialog(HWND hParent, std::wstring& text, const wchar_t* title)
{
    // DLGTEMPLATE을 메모리에서 동적으로 생성
    // 구조: DLGTEMPLATE + DLGITEMTEMPLATE(에디트) + DLGITEMTEMPLATE(확인) + DLGITEMTEMPLATE(취소)
    //
    // 단위는 Dialog Unit (DU). 1 DU ≈ 1/4 평균 문자폭(X), 1/8 평균 문자높이(Y)
    // 창 크기: 220 x 100 DU

    const WORD DLG_W = 220, DLG_H = 100;
    const WORD EDT_X = 7,  EDT_Y = 18, EDT_W = 206, EDT_H = 58;
    const WORD OK_X  = 108,OK_Y = 82, OK_W  = 48,  OK_H  = 14;
    const WORD CAN_X = 162,CAN_Y= 82, CAN_W = 48,  CAN_H = 14;

    // 버퍼 (넉넉하게)
    std::vector<BYTE> buf(1024, 0);
    BYTE* p = buf.data();

    auto align4 = [&]() { p = buf.data() + ((p - buf.data() + 3) & ~3); };
    auto writeW = [&](WORD v)  { memcpy(p, &v, 2); p += 2; };
    auto writeDW= [&](DWORD v) { memcpy(p, &v, 4); p += 4; };
    auto writeStr = [&](const wchar_t* s) {
        size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
        memcpy(p, s, n); p += n;
    };
    auto writeAtom = [&](WORD atom) { writeW(0xFFFF); writeW(atom); };

    // ── DLGTEMPLATE ──
    writeDW(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    writeDW(0);             // exStyle
    writeW(3);              // cDlgItems
    writeW(0); writeW(0);   // x, y (DS_CENTER가 처리)
    writeW(DLG_W); writeW(DLG_H);
    writeW(0);              // menu: none
    writeW(0);              // class: predefined
    writeStr(title);        // title
    // DS_SETFONT 시 폰트 정보
    writeW(9);              // point size
    writeStr(L"Segoe UI");

    // ── EDIT 컨트롤 ──
    align4();
    writeDW(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN);
    writeDW(0);
    writeW(EDT_X); writeW(EDT_Y); writeW(EDT_W); writeW(EDT_H);
    writeW(IDC_NOTE_EDIT);
    writeAtom(0x0081);      // EDIT class atom
    writeStr(text.c_str()); // 초기 텍스트
    writeW(0);              // no extra data

    // ── 확인 버튼 ──
    align4();
    writeDW(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON);
    writeDW(0);
    writeW(OK_X); writeW(OK_Y); writeW(OK_W); writeW(OK_H);
    writeW(IDOK);
    writeAtom(0x0080);      // BUTTON class atom
    writeStr(L"확인");
    writeW(0);

    // ── 취소 버튼 ──
    align4();
    writeDW(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON);
    writeDW(0);
    writeW(CAN_X); writeW(CAN_Y); writeW(CAN_W); writeW(CAN_H);
    writeW(IDCANCEL);
    writeAtom(0x0080);
    writeStr(L"취소");
    writeW(0);

    NoteDialogParam param{ text, L"" };
    INT_PTR res = DialogBoxIndirectParamW(
        hInst,
        reinterpret_cast<LPCDLGTEMPLATEW>(buf.data()),
        hParent,
        NoteDialogProc,
        reinterpret_cast<LPARAM>(&param));

    if (res == IDOK) {
        text = param.resultText;
        return true;
    }
    return false;
}
#endif // legacy

// 지정 페이지·PDF 좌표에 메모 추가
static void AddNoteAtPdfPos(HWND hWnd, int pageIndex, double pdfX, double pdfY)
{
    std::wstring text;
    if (!CleanPopup::ShowNoteInput(hWnd, L"메모 추가", text)) return;
    if (text.empty()) return;

    NoteItem note;
    note.pageIndex = pageIndex;
    note.pdfX      = pdfX;
    note.pdfY      = pdfY;
    note.text      = text;
    g_notes.push_back(note);
    g_dirty = true;
    InvalidateRect(hWnd, nullptr, FALSE);
}

// PDF 파일에서 FreeText 어노테이션을 메모로 로드
static void LoadNoteAnnotations(AnnotLoadResult* out, FPDF_DOCUMENT doc)
{
    if (!doc) doc = g_pdfDoc;
    std::vector<NoteItem>& notes = out ? out->notes : g_notes;
    if (!out) notes.clear();
    if (!doc) return;
    const int pageCount = FPDF_GetPageCount(doc);

    for (int pi = 0; pi < pageCount; ++pi)
    {
        bool isCurrentPage = !out && (pi == g_currentPage && g_pdfPage);
        FPDF_PAGE page = isCurrentPage ? g_pdfPage : FPDF_LoadPage(doc, pi);
        if (!page) continue;

        int annotCount = FPDFPage_GetAnnotCount(page);
        std::vector<int> toRemove;

        for (int ai = 0; ai < annotCount; ++ai)
        {
            FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, ai);
            if (!annot) continue;

            FPDF_ANNOTATION_SUBTYPE subtype = FPDFAnnot_GetSubtype(annot);
            if (subtype == FPDF_ANNOT_FREETEXT || subtype == FPDF_ANNOT_TEXT)
            {
                // TextBox 어노테이션은 LoadTextBoxAnnotations에서 처리 — 여기서 건너뜀
                unsigned long tLen = FPDFAnnot_GetStringValue(annot, "T", nullptr, 0);
                if (tLen > 0)
                {
                    std::vector<unsigned short> tbuf(tLen / sizeof(unsigned short) + 2, 0);
                    FPDFAnnot_GetStringValue(annot, "T",
                        reinterpret_cast<FPDF_WCHAR*>(tbuf.data()), tLen);
                    std::wstring tval = reinterpret_cast<wchar_t*>(tbuf.data());
                    if (tval == L"PlainPDFTextBox")
                    {
                        FPDFPage_CloseAnnot(annot);
                        continue;
                    }
                }

                FS_RECTF rect{};
                FPDFAnnot_GetRect(annot, &rect);

                unsigned long bufLen = FPDFAnnot_GetStringValue(annot, "Contents", nullptr, 0);
                std::wstring content;
                if (bufLen > 0)
                {
                    std::vector<unsigned short> buf(bufLen / sizeof(unsigned short) + 2, 0);
                    FPDFAnnot_GetStringValue(annot, "Contents",
                        reinterpret_cast<FPDF_WCHAR*>(buf.data()), bufLen);
                    content = reinterpret_cast<wchar_t*>(buf.data());
                }

                if (!content.empty())
                {
                    NoteItem note;
                    note.pageIndex = pi;
                    note.pdfX      = rect.left;
                    note.pdfY      = rect.top;
                    note.text      = content;
                    notes.push_back(note);
                    toRemove.push_back(ai);  // 메모리로 옮겼으므로 PDF에서 제거
                }
            }
            FPDFPage_CloseAnnot(annot);
        }

        // 역순으로 제거 (인덱스 밀림 방지)
        for (int i = static_cast<int>(toRemove.size()) - 1; i >= 0; --i)
            FPDFPage_RemoveAnnot(page, toRemove[i]);
        if (!toRemove.empty())
            FPDFPage_GenerateContent(page);

        if (!isCurrentPage) FPDF_ClosePage(page);
    }
}

// 서명을 PDF Ink 어노테이션으로 저장 (재열 시 복원 가능)
void EmbedSignatureObjects()
{
    if (!g_pdfDoc || g_signatures.empty()) return;

    for (const auto& sig : g_signatures)
    {
        if (sig.pageIndex < 0 || sig.pageIndex >= g_pageCount) continue;

        FPDF_PAGE page = (sig.pageIndex == g_currentPage && g_pdfPage)
            ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, sig.pageIndex);
        if (!page) continue;
        bool isCurrentPage = (sig.pageIndex == g_currentPage && g_pdfPage);

        if (sig.isImage && sig.bitmap)
        {
            // GDI+ Bitmap → FPDF_BITMAP → FPDFPageObj 삽입
            int iw = (int)sig.bitmap->GetWidth();
            int ih = (int)sig.bitmap->GetHeight();
            if (iw > 0 && ih > 0)
            {
                FPDF_BITMAP fBitmap = FPDFBitmap_Create(iw, ih, 1); // BGRA with alpha
                if (fBitmap)
                {
                    void* buf2   = FPDFBitmap_GetBuffer(fBitmap);
                    int   stride = FPDFBitmap_GetStride(fBitmap);

                    Gdiplus::BitmapData bmpData;
                    Gdiplus::Rect lockRc(0, 0, iw, ih);
                    if (sig.bitmap->LockBits(&lockRc,
                        Gdiplus::ImageLockModeRead,
                        PixelFormat32bppARGB, &bmpData) == Gdiplus::Ok)
                    {
                        for (int row = 0; row < ih; row++)
                            memcpy((BYTE*)buf2 + row * stride,
                                   (BYTE*)bmpData.Scan0 + row * bmpData.Stride,
                                   iw * 4);
                        sig.bitmap->UnlockBits(&bmpData);
                    }

                    FPDF_PAGEOBJECT imgObj = FPDFPageObj_NewImageObj(g_pdfDoc);
                    if (imgObj)
                    {
                        FPDF_PAGE pages[1] = { page };
                        if (FPDFImageObj_SetBitmap(pages, 1, imgObj, fBitmap))
                        {
                            double l = sig.pdfX;
                            double b = sig.pdfY - sig.pdfH;
                            double r2 = sig.pdfX + sig.pdfW;
                            double t  = sig.pdfY;
                            FPDFPageObj_Transform(imgObj, r2 - l, 0, 0, t - b, l, b);
                            FPDFPage_InsertObject(page, imgObj);
                        }
                        else
                        {
                            FPDFPageObj_Destroy(imgObj);
                        }
                    }
                    FPDFBitmap_Destroy(fBitmap);
                }
            }
        }
        else if (!sig.strokes.empty())
        {
            // 잉크 서명: 어노테이션 대신 path 오브젝트로 삽입 (비가역적, 이미지 서명과 동일)
            double pdfPenW = std::max(sig.pdfW, sig.pdfH) * 0.012;

            for (auto& stroke : sig.strokes)
            {
                if (stroke.empty()) continue;

                FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(stroke[0].x, stroke[0].y);
                if (!path) continue;

                for (int i = 1; i < (int)stroke.size(); i++)
                    FPDFPath_LineTo(path, stroke[i].x, stroke[i].y);

                FPDFPageObj_SetStrokeColor(path, 20, 20, 20, 255);
                FPDFPageObj_SetFillColor(path, 0, 0, 0, 0);
                FPDFPageObj_SetStrokeWidth(path, (float)pdfPenW);
                FPDFPath_SetDrawMode(path, 0, 1); // stroke only

                FPDFPage_InsertObject(page, path);
            }
        }

        FPDFPage_GenerateContent(page);
        if (!isCurrentPage) FPDF_ClosePage(page);
    }

    // 삽입 완료 → 오버레이 초기화
    for (auto& s : g_signatures) delete s.bitmap;
    g_signatures.clear();
    g_selectedSig = -1;
}

// Ink 어노테이션을 로드하여 오버레이로 복원
static void LoadSignatureAnnotations(AnnotLoadResult* out, FPDF_DOCUMENT doc)
{
    if (!doc) doc = g_pdfDoc;
    std::vector<SignatureItem>& signatures = out ? out->signatures : g_signatures;
    if (!out)
    {
        for (auto& s : signatures) delete s.bitmap;
        signatures.clear();
        g_selectedSig = -1;
    }
    if (!doc) return;
    const int pageCount = FPDF_GetPageCount(doc);

    for (int pi = 0; pi < pageCount; ++pi)
    {
        bool isCurrentPage = !out && (pi == g_currentPage && g_pdfPage);
        FPDF_PAGE page = isCurrentPage ? g_pdfPage : FPDF_LoadPage(doc, pi);
        if (!page) continue;

        int annotCount = FPDFPage_GetAnnotCount(page);
        std::vector<int> toRemove;

        for (int ai = 0; ai < annotCount; ++ai)
        {
            FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, ai);
            if (!annot) continue;

            if (FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_INK)
            {
                unsigned long tLen = FPDFAnnot_GetStringValue(annot, "T", nullptr, 0);
                std::wstring author;
                if (tLen > 0) {
                    std::vector<unsigned short> tbuf(tLen / sizeof(unsigned short) + 2, 0);
                    FPDFAnnot_GetStringValue(annot, "T",
                        reinterpret_cast<FPDF_WCHAR*>(tbuf.data()), tLen);
                    author = reinterpret_cast<wchar_t*>(tbuf.data());
                }
                if (author == L"PlainPDFSignature")
                {
                    FS_RECTF rect{};
                    FPDFAnnot_GetRect(annot, &rect);

                    SignatureItem item;
                    item.pageIndex = pi;
                    item.pdfX = rect.left;
                    item.pdfY = rect.top;
                    item.pdfW = rect.right - rect.left;
                    item.pdfH = rect.top - rect.bottom;

                    // 획 데이터 복원
                    int strokeCount = FPDFAnnot_GetInkListCount(annot);
                    for (int si = 0; si < strokeCount; ++si)
                    {
                        int ptCount = FPDFAnnot_GetInkListPath(annot, si, nullptr, 0);
                        if (ptCount <= 0) continue;
                        std::vector<FS_POINTF> pts(ptCount);
                        FPDFAnnot_GetInkListPath(annot, si, pts.data(), ptCount);
                        std::vector<SigStrokePt> stroke;
                        stroke.reserve(ptCount);
                        for (auto& p : pts) stroke.push_back({ p.x, p.y });
                        item.strokes.push_back(std::move(stroke));
                    }

                    if (!item.strokes.empty()) {
                        signatures.push_back(std::move(item));
                        toRemove.push_back(ai);
                    }
                }
            }
            FPDFPage_CloseAnnot(annot);
        }

        for (int i = (int)toRemove.size() - 1; i >= 0; --i)
            FPDFPage_RemoveAnnot(page, toRemove[i]);
        if (!toRemove.empty())
            FPDFPage_GenerateContent(page);

        if (!isCurrentPage) FPDF_ClosePage(page);
    }
}

void EmbedTextBoxAnnotations()
{
    if (!g_pdfDoc || g_textBoxes.empty()) return;
    for (const auto& tb : g_textBoxes)
    {
        if (tb.pageIndex < 0 || tb.pageIndex >= g_pageCount) continue;
        FPDF_PAGE page = (tb.pageIndex == g_currentPage && g_pdfPage)
            ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, tb.pageIndex);
        if (!page) continue;
        bool isCurrentPage = (tb.pageIndex == g_currentPage && g_pdfPage);

        FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT);
        if (annot)
        {
            const auto& layout = g_pageLayouts[tb.pageIndex];
            FS_RECTF rect = {};
            rect.left   = (float)tb.pdfX;
            rect.top    = (float)tb.pdfY;
            rect.right  = (float)(tb.pdfX + tb.pdfW);
            rect.bottom = (float)(tb.pdfY - tb.pdfH);
            FPDFAnnot_SetRect(annot, &rect);
            FPDFAnnot_SetStringValue(annot, "Contents",
                reinterpret_cast<const FPDF_WCHAR*>(tb.text.c_str()));
            // 마커: PlainPDFTextBox 로 구분
            FPDFAnnot_SetStringValue(annot, "T",
                reinterpret_cast<const FPDF_WCHAR*>(L"PlainPDFTextBox"));
            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 255, 255, 180, 180);
            FPDFPage_CloseAnnot(annot);
        }
        FPDFPage_GenerateContent(page);
        if (!isCurrentPage) FPDF_ClosePage(page);
    }
}

// PDF 로드 시 TextBox 어노테이션 불러오기
static void LoadTextBoxAnnotations(AnnotLoadResult* out, FPDF_DOCUMENT doc)
{
    if (!doc) doc = g_pdfDoc;
    std::vector<TextBoxItem>& textBoxes = out ? out->textBoxes : g_textBoxes;
    if (!out) textBoxes.clear();
    if (!doc) return;
    const int pageCount = FPDF_GetPageCount(doc);

    for (int pi = 0; pi < pageCount; ++pi)
    {
        bool isCurrentPage = !out && (pi == g_currentPage && g_pdfPage);
        FPDF_PAGE page = isCurrentPage ? g_pdfPage : FPDF_LoadPage(doc, pi);
        if (!page) continue;

        int annotCount = FPDFPage_GetAnnotCount(page);
        std::vector<int> toRemove;

        for (int ai = 0; ai < annotCount; ++ai)
        {
            FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, ai);
            if (!annot) continue;

            if (FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_FREETEXT)
            {
                // "T" 키로 PlainPDFTextBox 확인
                unsigned long tLen = FPDFAnnot_GetStringValue(annot, "T", nullptr, 0);
                std::wstring author;
                if (tLen > 0)
                {
                    std::vector<unsigned short> tbuf(tLen / sizeof(unsigned short) + 2, 0);
                    FPDFAnnot_GetStringValue(annot, "T",
                        reinterpret_cast<FPDF_WCHAR*>(tbuf.data()), tLen);
                    author = reinterpret_cast<wchar_t*>(tbuf.data());
                }
                if (author == L"PlainPDFTextBox")
                {
                    FS_RECTF rect{};
                    FPDFAnnot_GetRect(annot, &rect);

                    unsigned long cLen = FPDFAnnot_GetStringValue(annot, "Contents", nullptr, 0);
                    std::wstring content;
                    if (cLen > 0)
                    {
                        std::vector<unsigned short> cbuf(cLen / sizeof(unsigned short) + 2, 0);
                        FPDFAnnot_GetStringValue(annot, "Contents",
                            reinterpret_cast<FPDF_WCHAR*>(cbuf.data()), cLen);
                        content = reinterpret_cast<wchar_t*>(cbuf.data());
                    }

                    TextBoxItem tb;
                    tb.pageIndex = pi;
                    tb.pdfX = rect.left;
                    tb.pdfY = rect.top;
                    tb.pdfW = rect.right - rect.left;
                    tb.pdfH = rect.top - rect.bottom;
                    tb.text = content;
                    textBoxes.push_back(tb);
                    toRemove.push_back(ai);
                }
            }
            FPDFPage_CloseAnnot(annot);
        }

        for (int i = (int)toRemove.size() - 1; i >= 0; --i)
            FPDFPage_RemoveAnnot(page, toRemove[i]);
        if (!toRemove.empty())
            FPDFPage_GenerateContent(page);

        if (!isCurrentPage) FPDF_ClosePage(page);
    }
}

static void LoadShapeAnnotations(AnnotLoadResult* out, FPDF_DOCUMENT doc)
{
    if (!doc) doc = g_pdfDoc;
    if (!doc) return;
    std::vector<ShapeItem>* dest = out ? &out->shapes : &g_shapes;
    dest->clear();
    const int pageCount = FPDF_GetPageCount(doc);

    for (int pi = 0; pi < pageCount; ++pi)
    {
        bool isCur = !out && (pi == g_currentPage) && g_pdfPage;
        FPDF_PAGE pg = isCur ? g_pdfPage : FPDF_LoadPage(doc, pi);
        if (!pg) continue;

        int cnt = FPDFPage_GetAnnotCount(pg);
        for (int ai = 0; ai < cnt; ++ai)
        {
            FPDF_ANNOTATION ann = FPDFPage_GetAnnot(pg, ai);
            if (!ann) continue;
            FPDF_ANNOTATION_SUBTYPE st = FPDFAnnot_GetSubtype(ann);
            if (st != FPDF_ANNOT_SQUARE && st != FPDF_ANNOT_CIRCLE)
            {
                FPDFPage_CloseAnnot(ann);
                continue;
            }

            FS_RECTF rect{};
            FPDFAnnot_GetRect(ann, &rect);

            unsigned int R = 255, G = 0, B = 0, A = 255;
            FPDFAnnot_GetColor(ann, FPDFANNOT_COLORTYPE_Color, &R, &G, &B, &A);

            ShapeItem sh;
            sh.pageIndex = pi;
            sh.isCircle  = (st == FPDF_ANNOT_CIRCLE);
            sh.color     = RGB(R, G, B);
            sh.lineWidth = 1.5f;
            // FS_RECTF: left/bottom/right/top
            sh.pdfX = rect.left;
            sh.pdfY = rect.top;                    // 상단 (하단원점)
            sh.pdfW = rect.right - rect.left;
            sh.pdfH = rect.top   - rect.bottom;    // 높이 (양수)
            FPDFPage_CloseAnnot(ann);
            dest->push_back(sh);
        }

        if (!isCur) FPDF_ClosePage(pg);
    }
}

// ── 어노테이션 로드 (메인 스레드 전용 — pdfium 스레드 불안전 회피) ──────────────
static void LoadInkAnnotations(AnnotLoadResult* out, FPDF_DOCUMENT doc)
{
    if (!doc) doc = g_pdfDoc;
    if (!doc) return;

    std::vector<InkStrokeItem>& inkStrokes = out ? out->inkStrokes : g_inkStrokes;

    if (!out)
    {
        inkStrokes.clear();
        g_selectedInkStroke = -1;
    }

    const int pageCount = FPDF_GetPageCount(doc);

    for (int pi = 0; pi < pageCount; ++pi)
    {
        bool isCur = !out && (pi == g_currentPage) && g_pdfPage;
        FPDF_PAGE pg = isCur ? g_pdfPage : FPDF_LoadPage(doc, pi);
        if (!pg) continue;

        int cnt = FPDFPage_GetAnnotCount(pg);
        for (int ai = 0; ai < cnt; ++ai)
        {
            FPDF_ANNOTATION ann = FPDFPage_GetAnnot(pg, ai);
            if (!ann) continue;
            if (FPDFAnnot_GetSubtype(ann) != FPDF_ANNOT_INK)
            {
                FPDFPage_CloseAnnot(ann);
                continue;
            }

            unsigned int R = 255, G = 0, B = 0, A = 255;
            FPDFAnnot_GetColor(ann, FPDFANNOT_COLORTYPE_Color, &R, &G, &B, &A);

            int strokeCount = FPDFAnnot_GetInkListCount(ann);
            for (int si = 0; si < strokeCount; ++si)
            {
                int ptCount = FPDFAnnot_GetInkListPath(ann, si, nullptr, 0);
                if (ptCount < 2) continue;

                std::vector<FS_POINTF> pts(ptCount);
                FPDFAnnot_GetInkListPath(ann, si, pts.data(), ptCount);

                InkStrokeItem sk;
                sk.pageIndex = pi;
                sk.color     = RGB(R, G, B);
                sk.lineWidth = 2.0f;
                for (auto& p : pts)
                    sk.pdfPts.emplace_back(p.x, p.y);
                inkStrokes.push_back(std::move(sk));
            }

            FPDFPage_CloseAnnot(ann);
        }

        if (!isCur) FPDF_ClosePage(pg);
    }
}

static void RunAnnotLoad(HWND hWnd)
{
    // UI 스레드 전용 — 전역 초기화 후 가시 페이지부터 지연 로딩 시작
    if (!g_pdfDoc) return;

    // 전역 초기화
    g_annotLoadReady = false;
    g_highlights.clear();
    g_underlines.clear();        g_selectedUnderline = -1;
    g_strikeouts.clear();        g_selectedStrikeout = -1;
    g_squigglies.clear();        g_selectedSquiggly  = -1;
    g_notes.clear();
    for (auto& s : g_signatures) delete s.bitmap;
    g_signatures.clear();        g_selectedSig       = -1;
    g_textBoxes.clear();
    g_shapes.clear();            g_selectedShape     = -1;
    g_inkStrokes.clear();        g_selectedInkStroke = -1;

    // 지연 로딩 상태 초기화
    const int pageCount = FPDF_GetPageCount(g_pdfDoc);
    g_annotPageLoaded.assign(pageCount, false);
    g_annotPagePosted.assign(pageCount, false);

    // 현재 화면에 보이는 페이지를 우선 처리하고, 나머지는 스크롤 시 처리
    g_annotLoadReady = true;
    TriggerAnnotLoadForVisiblePages(hWnd);
}

// 현재 뷰포트에서 보이는 페이지 인덱스 범위를 반환 [first, last]
static void GetVisiblePageRange(HWND hWnd, int& first, int& last)
{
    first = 0; last = 0;
    if (!g_pdfDoc || g_pageLayouts.empty()) return;

    RECT cr; GetClientRect(hWnd, &cr);
    RECT vp = GetViewportRect(cr);
    int vpTop    = g_scrollY;
    int vpBottom = g_scrollY + (vp.bottom - vp.top);

    first = g_pageCount - 1;
    last  = 0;
    for (int i = 0; i < g_pageCount; ++i)
    {
        int pageTop = g_pageLayouts[i].yOffset;
        int pageBot = pageTop + g_pageLayouts[i].renderH;
        if (pageBot < vpTop)  continue;   // 아직 화면 위
        if (pageTop > vpBottom) break;    // 이미 화면 아래
        if (i < first) first = i;
        if (i > last)  last  = i;
    }
    // 버퍼로 앞뒤 1페이지 추가 (스크롤 전 미리 로드)
    first = std::max(0, first - 1);
    last  = std::min(g_pageCount - 1, last + 1);
}

// 가시 범위에서 아직 로드되지 않은 페이지만 WM_ANNOT_PAGE로 Post
static void TriggerAnnotLoadForVisiblePages(HWND hWnd)
{
    if (!g_pdfDoc || !g_annotLoadReady) return;
    int first, last;
    GetVisiblePageRange(hWnd, first, last);
    for (int i = first; i <= last; ++i)
    {
        if (i < 0 || i >= (int)g_annotPageLoaded.size()) continue;
        if (g_annotPageLoaded[i] || g_annotPagePosted[i]) continue;
        g_annotPagePosted[i] = true;
        PostMessageW(hWnd, WM_ANNOT_PAGE, (WPARAM)i, 0);
    }
}

// WM_ANNOT_PAGE 핸들러에서 호출 — [startPage, endPage) 처리
static void RunAnnotLoadBatch(HWND hWnd, int startPage, int endPage)
{
    if (!g_pdfDoc) return;
    // PDFium은 스레드 안전하지 않으므로 백그라운드 렌더 워커와 동시에 호출되지 않도록 직렬화
    std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);
    for (int pi = startPage; pi < endPage; ++pi)
    {
        bool isCurPage = (pi == g_currentPage && g_pdfPage != nullptr);
        FPDF_PAGE page = isCurPage ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, pi);
        if (!page) continue;

        // 텍스트 페이지는 하이라이트 처리 시에만 필요
        FPDF_TEXTPAGE textPage = nullptr;

        const int annotCount = FPDFPage_GetAnnotCount(page);
        std::vector<int> removeIdx; // 전송/재삽입된 어노테이션 인덱스

        for (int ai = 0; ai < annotCount; ++ai)
        {
            FPDF_ANNOTATION ann = FPDFPage_GetAnnot(page, ai);
            if (!ann) continue;

            FPDF_ANNOTATION_SUBTYPE st = FPDFAnnot_GetSubtype(ann);

            // ── 하이라이트 / 밑줄 / 취소선 / 물결 ──────────────────────────
            if (st == FPDF_ANNOT_HIGHLIGHT || st == FPDF_ANNOT_UNDERLINE ||
                st == FPDF_ANNOT_STRIKEOUT || st == FPDF_ANNOT_SQUIGGLY)
            {
                unsigned int R = 255, G = 255, B = 80, A = 150;
                FPDFAnnot_GetColor(ann, FPDFANNOT_COLORTYPE_Color, &R, &G, &B, &A);
                COLORREF color = RGB(R, G, B);

                // ── 1. PlainPDFIdx 필드에서 charStart:charEnd 복원 (PlainPDFViewer 저장 주석) ──
                int foundStart = -1, foundEnd = -1;
                {
                    unsigned long cLen = FPDFAnnot_GetStringValue(ann, "PlainPDFIdx", nullptr, 0);
                    if (cLen > 2)
                    {
                        std::vector<unsigned short> cbuf(cLen / sizeof(unsigned short) + 2, 0);
                        FPDFAnnot_GetStringValue(ann, "PlainPDFIdx",
                            reinterpret_cast<FPDF_WCHAR*>(cbuf.data()), cLen);
                        const wchar_t* ws = reinterpret_cast<const wchar_t*>(cbuf.data());
                        // BOM(0xFEFF) 건너뜀
                        if (*ws == 0xFEFF) ws++;
                        // "charStart:charEnd" 형식 파싱
                        const wchar_t* colon = wcschr(ws, L':');
                        if (colon)
                        {
                            foundStart = _wtoi(ws);
                            foundEnd   = _wtoi(colon + 1);
                        }
                    }
                }

                // ── 2. PlainPDFIdx 파싱 실패 시 quadpoints → 텍스트 위치 역매핑 ──
                if (foundStart < 0 || foundEnd < 0 || foundStart > foundEnd)
                {
                    if (!textPage)
                        textPage = isCurPage ? g_textPage : FPDFText_LoadPage(page);

                    if (textPage)
                    {
                        int qStart = INT_MAX, qEnd = INT_MIN;
                        size_t qpCount = FPDFAnnot_CountAttachmentPoints(ann);
                        for (size_t qi = 0; qi < qpCount; ++qi)
                        {
                            FS_QUADPOINTSF qp;
                            if (!FPDFAnnot_GetAttachmentPoints(ann, qi, &qp)) continue;
                            double qMinX = std::min({ (double)qp.x1,(double)qp.x2,(double)qp.x3,(double)qp.x4 });
                            double qMaxX = std::max({ (double)qp.x1,(double)qp.x2,(double)qp.x3,(double)qp.x4 });
                            double qMinY = std::min({ (double)qp.y1,(double)qp.y2,(double)qp.y3,(double)qp.y4 });
                            double qMaxY = std::max({ (double)qp.y1,(double)qp.y2,(double)qp.y3,(double)qp.y4 });
                            double cx = (qMinX + qMaxX) * 0.5, cy = (qMinY + qMaxY) * 0.5;
                            struct { double x, y; } probes[] = {
                                {cx,cy},{qMinX,qMaxY},{qMaxX,qMinY},{qMinX,qMinY},{qMaxX,qMaxY}
                            };
                            for (auto& probe : probes)
                            {
                                for (double tol : {5.0, 15.0, 30.0})
                                {
                                    int ci = FPDFText_GetCharIndexAtPos(textPage, probe.x, probe.y, tol, tol);
                                    if (ci >= 0) { if (ci < qStart) qStart = ci; if (ci > qEnd) qEnd = ci; break; }
                                }
                            }
                        }
                        if (qStart <= qEnd && qStart != INT_MAX)
                        {
                            foundStart = std::min(qStart, qEnd);
                            foundEnd   = std::max(qStart, qEnd);
                        }
                    }
                }

                if (foundStart >= 0 && foundEnd >= foundStart)
                {
                    if (st == FPDF_ANNOT_STRIKEOUT) { StrikeoutItem o{}; o.pageIndex=pi; o.charStart=foundStart; o.charEnd=foundEnd; o.color=color; g_strikeouts.push_back(o); }
                    else if (st == FPDF_ANNOT_SQUIGGLY) { SquigglyItem o; o.pageIndex=pi; o.charStart=foundStart; o.charEnd=foundEnd; o.color=color; g_squigglies.push_back(o); }
                    else if (st == FPDF_ANNOT_UNDERLINE) { UnderlineItem o{}; o.pageIndex=pi; o.charStart=foundStart; o.charEnd=foundEnd; o.color=color; g_underlines.push_back(o); }
                    else { HighlightItem o; o.pageIndex=pi; o.charStart=foundStart; o.charEnd=foundEnd; o.color=color; g_highlights.push_back(o); }
                    removeIdx.push_back(ai);
                }
                FPDFPage_CloseAnnot(ann);
                continue;
            }

            // ── FreeText: 메모 또는 텍스트박스 ─────────────────────────────
            if (st == FPDF_ANNOT_FREETEXT || st == FPDF_ANNOT_TEXT)
            {
                unsigned long tLen = FPDFAnnot_GetStringValue(ann, "T", nullptr, 0);
                std::wstring author;
                if (tLen > 0)
                {
                    std::vector<unsigned short> tbuf(tLen / sizeof(unsigned short) + 2, 0);
                    FPDFAnnot_GetStringValue(ann, "T", reinterpret_cast<FPDF_WCHAR*>(tbuf.data()), tLen);
                    author = reinterpret_cast<wchar_t*>(tbuf.data());
                }
                if (author == L"PlainPDFTextBox")
                {
                    FS_RECTF rect{};
                    FPDFAnnot_GetRect(ann, &rect);
                    unsigned long cLen = FPDFAnnot_GetStringValue(ann, "Contents", nullptr, 0);
                    std::wstring content;
                    if (cLen > 0)
                    {
                        std::vector<unsigned short> cbuf(cLen / sizeof(unsigned short) + 2, 0);
                        FPDFAnnot_GetStringValue(ann, "Contents", reinterpret_cast<FPDF_WCHAR*>(cbuf.data()), cLen);
                        content = reinterpret_cast<wchar_t*>(cbuf.data());
                    }
                    TextBoxItem tb; tb.pageIndex=pi; tb.pdfX=rect.left; tb.pdfY=rect.top;
                    tb.pdfW=rect.right-rect.left; tb.pdfH=rect.top-rect.bottom; tb.text=content;
                    g_textBoxes.push_back(tb);
                    removeIdx.push_back(ai);
                }
                else
                {
                    // 일반 메모
                    FS_RECTF rect{}; FPDFAnnot_GetRect(ann, &rect);
                    unsigned long cLen = FPDFAnnot_GetStringValue(ann, "Contents", nullptr, 0);
                    std::wstring content;
                    if (cLen > 0)
                    {
                        std::vector<unsigned short> cbuf(cLen / sizeof(unsigned short) + 2, 0);
                        FPDFAnnot_GetStringValue(ann, "Contents", reinterpret_cast<FPDF_WCHAR*>(cbuf.data()), cLen);
                        content = reinterpret_cast<wchar_t*>(cbuf.data());
                    }
                    if (!content.empty())
                    {
                        NoteItem note; note.pageIndex=pi; note.pdfX=rect.left; note.pdfY=rect.top; note.text=content;
                        g_notes.push_back(note);
                    }
                }
                FPDFPage_CloseAnnot(ann);
                continue;
            }

            // ── 도형 (사각형 / 원) ──────────────────────────────────────────
            if (st == FPDF_ANNOT_SQUARE || st == FPDF_ANNOT_CIRCLE)
            {
                FS_RECTF rect{}; FPDFAnnot_GetRect(ann, &rect);
                unsigned int R=255,G=0,B=0,A=255;
                FPDFAnnot_GetColor(ann, FPDFANNOT_COLORTYPE_Color, &R, &G, &B, &A);
                ShapeItem sh; sh.pageIndex=pi; sh.isCircle=(st==FPDF_ANNOT_CIRCLE);
                sh.color=RGB(R,G,B); sh.lineWidth=1.5f;
                sh.pdfX=rect.left; sh.pdfY=rect.top;
                sh.pdfW=rect.right-rect.left; sh.pdfH=rect.top-rect.bottom;
                g_shapes.push_back(sh);
                FPDFPage_CloseAnnot(ann);
                continue;
            }

            // ── 잉크 (서명 또는 일반 잉크) ──────────────────────────────────
            if (st == FPDF_ANNOT_INK)
            {
                unsigned long tLen = FPDFAnnot_GetStringValue(ann, "T", nullptr, 0);
                std::wstring author;
                if (tLen > 0)
                {
                    std::vector<unsigned short> tbuf(tLen / sizeof(unsigned short) + 2, 0);
                    FPDFAnnot_GetStringValue(ann, "T", reinterpret_cast<FPDF_WCHAR*>(tbuf.data()), tLen);
                    author = reinterpret_cast<wchar_t*>(tbuf.data());
                }

                if (author == L"PlainPDFSignature")
                {
                    FS_RECTF rect{}; FPDFAnnot_GetRect(ann, &rect);
                    SignatureItem item; item.pageIndex=pi;
                    item.pdfX=rect.left; item.pdfY=rect.top;
                    item.pdfW=rect.right-rect.left; item.pdfH=rect.top-rect.bottom;
                    int sc = FPDFAnnot_GetInkListCount(ann);
                    for (int si2=0; si2<sc; ++si2)
                    {
                        int ptc = FPDFAnnot_GetInkListPath(ann, si2, nullptr, 0);
                        if (ptc <= 0) continue;
                        std::vector<FS_POINTF> pts(ptc);
                        FPDFAnnot_GetInkListPath(ann, si2, pts.data(), ptc);
                        std::vector<SigStrokePt> stroke;
                        for (auto& pt : pts) stroke.push_back({pt.x, pt.y});
                        item.strokes.push_back(std::move(stroke));
                    }
                    if (!item.strokes.empty()) { g_signatures.push_back(std::move(item)); removeIdx.push_back(ai); }
                }
                else
                {
                    unsigned int R=255,G=0,B=0,A=255;
                    FPDFAnnot_GetColor(ann, FPDFANNOT_COLORTYPE_Color, &R, &G, &B, &A);
                    int sc = FPDFAnnot_GetInkListCount(ann);
                    for (int si2=0; si2<sc; ++si2)
                    {
                        int ptc = FPDFAnnot_GetInkListPath(ann, si2, nullptr, 0);
                        if (ptc < 2) continue;
                        std::vector<FS_POINTF> pts(ptc);
                        FPDFAnnot_GetInkListPath(ann, si2, pts.data(), ptc);
                        InkStrokeItem sk; sk.pageIndex=pi; sk.color=RGB(R,G,B); sk.lineWidth=2.0f;
                        for (auto& pt : pts) sk.pdfPts.emplace_back(pt.x, pt.y);
                        g_inkStrokes.push_back(std::move(sk));
                    }
                }
                FPDFPage_CloseAnnot(ann);
                continue;
            }

            FPDFPage_CloseAnnot(ann);
        } // for ai

        // 역순으로 어노테이션 제거 (인덱스 밀림 방지)
        for (int i = (int)removeIdx.size()-1; i >= 0; --i)
            FPDFPage_RemoveAnnot(page, removeIdx[i]);
        if (!removeIdx.empty())
            FPDFPage_GenerateContent(page);

        if (!isCurPage)
        {
            if (textPage && textPage != g_textPage) FPDFText_ClosePage(textPage);
            FPDF_ClosePage(page);
        }

        // 이 페이지 주석 로드 완료 표시
        if (pi >= 0 && pi < (int)g_annotPageLoaded.size())
            g_annotPageLoaded[pi] = true;
    } // for pi
}

// 파일을 메모리로 읽어 FPDF_LoadMemDocument로 로드한다.
// FPDF_LoadDocument는 내부적으로 mmap을 사용하여 CloseDocument 이후에도
// OS가 파일 핸들을 즉시 해제하지 않을 수 있다.
// 이 함수는 파일 핸들을 읽기 직후 해제하여 저장 시 교체가 가능하게 한다.
// pFileData / pFileSize 가 non-null 이면 호출자에게 버퍼를 넘겨준다 (FPDF_LoadMemDocument 수명 관리).
static FPDF_DOCUMENT LoadPDFFromFile(const std::wstring& wpath,
                                     std::vector<char>* pFileData,
                                     const char* password)
{
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, wpath.c_str(), L"rb") != 0 || !fp)
        return nullptr;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) { fclose(fp); return nullptr; }

    std::vector<char> buf(static_cast<size_t>(size));
    size_t rd = fread(buf.data(), 1, buf.size(), fp);
    fclose(fp);  // 즉시 파일 핸들 해제
    if (rd != buf.size()) return nullptr;

    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(buf.data(), static_cast<int>(buf.size()), password);
    if (doc && pFileData)
        *pFileData = std::move(buf);
    return doc;
}

static bool AskPassword(HWND hWnd, std::wstring& out, bool retry)
{
    const wchar_t* title = LS(STR_PW_TITLE);
    const wchar_t* label = retry ? LS(STR_PW_RETRY) : LS(STR_PW_PROMPT);
    return CleanPopup::ShowPasswordInput(hWnd, title, label, out);
}

FPDF_DOCUMENT OpenPdfByPathWithPrompt(HWND hWnd, const std::string& u8Path, std::string* pwOut)
{
    if (pwOut) pwOut->clear();
    FPDF_DOCUMENT doc = FPDF_LoadDocument(u8Path.c_str(), pwOut && !pwOut->empty() ? pwOut->c_str() : nullptr);
    if (doc || FPDF_GetLastError() != FPDF_ERR_PASSWORD) return doc;
    for (int tries = 0; tries < 5; ++tries)
    {
        std::wstring wp;
        if (!AskPassword(hWnd, wp, tries > 0)) return nullptr;
        int n = WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string pw(n > 0 ? n : 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), -1, pw.data(), n, nullptr, nullptr);
        SecureZeroMemory(wp.data(), wp.size() * sizeof(wchar_t));
        doc = FPDF_LoadDocument(u8Path.c_str(), pw.c_str());
        if (doc)
        {
            if (pwOut) pwOut->assign(pw.c_str());
            SecureZeroMemory(pw.data(), pw.size());
            return doc;
        }
        SecureZeroMemory(pw.data(), pw.size());
        if (FPDF_GetLastError() != FPDF_ERR_PASSWORD) return nullptr;
    }
    return nullptr;
}

static FPDF_DOCUMENT LoadPDFWithPasswordPrompt(HWND hWnd, const std::wstring& path,
                                               std::vector<char>* pFileData)
{
    g_pwPromptCancelled = false;
    FPDF_DOCUMENT doc = LoadPDFFromFile(path, pFileData);
    if (doc || FPDF_GetLastError() != FPDF_ERR_PASSWORD) return doc;
    for (int tries = 0; tries < 5; ++tries)
    {
        std::wstring wp;
        if (!AskPassword(hWnd, wp, tries > 0)) { g_pwPromptCancelled = true; return nullptr; }
        int n = WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string pw(n > 0 ? n : 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), -1, pw.data(), n, nullptr, nullptr);
        SecureZeroMemory(wp.data(), wp.size() * sizeof(wchar_t));
        doc = LoadPDFFromFile(path, pFileData, pw.c_str());
        if (doc)
        {
            std::lock_guard<std::mutex> lk(g_pdfiumMtx);
            g_docPassword.assign(pw.c_str());
        }
        SecureZeroMemory(pw.data(), pw.size());
        if (doc) return doc;
        if (FPDF_GetLastError() != FPDF_ERR_PASSWORD) return nullptr;
    }
    return nullptr;
}

// FPDF_FILEWRITE 를 파일에 쓰기 위한 래퍼
struct FileWriteContext {
    FPDF_FILEWRITE fileWrite;
    FILE*          fp;
};

static int FileWriteBlock(FPDF_FILEWRITE_* self, const void* data, unsigned long size)
{
    auto* ctx = reinterpret_cast<FileWriteContext*>(self);
    return (fwrite(data, 1, size, ctx->fp) == size) ? 1 : 0;
}

// 저장 전 메모리 컬렉션에서 중복 항목을 제거한다.
// 미로드 페이지 강제 로드 시 PDF에 누적된 중복 주석이 그대로 append될 수 있으므로
// embed 전에 반드시 호출해야 한다.
static void DeduplicateAnnotations()
{
    // ── 하이라이트 (pageIndex + charStart + charEnd + color) ─────────────
    {
        using Key = std::tuple<int,int,int,COLORREF>;
        std::set<Key> seen;
        std::vector<HighlightItem> out;
        out.reserve(g_highlights.size());
        for (auto& h : g_highlights)
        {
            Key k{ h.pageIndex, h.charStart, h.charEnd, h.color };
            if (seen.insert(k).second) out.push_back(h);
        }
        g_highlights = std::move(out);
    }
    // ── 밑줄 ─────────────────────────────────────────────────────────────
    {
        using Key = std::tuple<int,int,int,COLORREF>;
        std::set<Key> seen;
        std::vector<UnderlineItem> out;
        out.reserve(g_underlines.size());
        for (auto& h : g_underlines)
        {
            Key k{ h.pageIndex, h.charStart, h.charEnd, h.color };
            if (seen.insert(k).second) out.push_back(h);
        }
        g_underlines = std::move(out);
    }
    // ── 취소선 ────────────────────────────────────────────────────────────
    {
        using Key = std::tuple<int,int,int,COLORREF>;
        std::set<Key> seen;
        std::vector<StrikeoutItem> out;
        out.reserve(g_strikeouts.size());
        for (auto& h : g_strikeouts)
        {
            Key k{ h.pageIndex, h.charStart, h.charEnd, h.color };
            if (seen.insert(k).second) out.push_back(h);
        }
        g_strikeouts = std::move(out);
    }
    // ── 물결 밑줄 ─────────────────────────────────────────────────────────
    {
        using Key = std::tuple<int,int,int,COLORREF>;
        std::set<Key> seen;
        std::vector<SquigglyItem> out;
        out.reserve(g_squigglies.size());
        for (auto& h : g_squigglies)
        {
            Key k{ h.pageIndex, h.charStart, h.charEnd, h.color };
            if (seen.insert(k).second) out.push_back(h);
        }
        g_squigglies = std::move(out);
    }
    // ── 메모 (pageIndex + 위치 + 내용) ───────────────────────────────────
    {
        using Key = std::tuple<int,int,int,std::wstring>;
        std::set<Key> seen;
        std::vector<NoteItem> out;
        out.reserve(g_notes.size());
        for (auto& n : g_notes)
        {
            Key k{ n.pageIndex, (int)n.pdfX, (int)n.pdfY, n.text };
            if (seen.insert(k).second) out.push_back(n);
        }
        g_notes = std::move(out);
    }
    // ── 텍스트박스 (pageIndex + 위치 + 내용) ─────────────────────────────
    {
        using Key = std::tuple<int,int,int,std::wstring>;
        std::set<Key> seen;
        std::vector<TextBoxItem> out;
        out.reserve(g_textBoxes.size());
        for (auto& t : g_textBoxes)
        {
            Key k{ t.pageIndex, (int)t.pdfX, (int)t.pdfY, t.text };
            if (seen.insert(k).second) out.push_back(t);
        }
        g_textBoxes = std::move(out);
    }
    // ── 도형 (pageIndex + 위치 + 크기 + isCircle) ────────────────────────
    {
        using Key = std::tuple<int,int,int,int,int,bool>;
        std::set<Key> seen;
        std::vector<ShapeItem> out;
        out.reserve(g_shapes.size());
        for (auto& s : g_shapes)
        {
            Key k{ s.pageIndex, (int)s.pdfX, (int)s.pdfY, (int)s.pdfW, (int)s.pdfH, s.isCircle };
            if (seen.insert(k).second) out.push_back(s);
        }
        g_shapes = std::move(out);
    }
    // ── 잉크 스트로크 (pageIndex + 첫 두 점) ─────────────────────────────
    {
        using Key = std::tuple<int,int,int,int,int>;
        std::set<Key> seen;
        std::vector<InkStrokeItem> out;
        out.reserve(g_inkStrokes.size());
        for (auto& sk : g_inkStrokes)
        {
            int x0 = sk.pdfPts.empty() ? 0 : (int)sk.pdfPts[0].first;
            int y0 = sk.pdfPts.empty() ? 0 : (int)sk.pdfPts[0].second;
            int x1 = sk.pdfPts.size() < 2 ? 0 : (int)sk.pdfPts[1].first;
            int y1 = sk.pdfPts.size() < 2 ? 0 : (int)sk.pdfPts[1].second;
            Key k{ sk.pageIndex, x0, y0, x1, y1 };
            if (seen.insert(k).second) out.push_back(sk);
        }
        g_inkStrokes = std::move(out);
    }
}

// 메모리 내 하이라이트·밑줄을 PDF 어노테이션으로 삽입 (현재 페이지만)
void EmbedHighlightAnnotations()
{
    if (!g_pdfDoc) return;
    if (g_highlights.empty() && g_underlines.empty() && g_strikeouts.empty() && g_squigglies.empty()) return;

    // 저장 전에 파일에 이미 존재하는 동종 annotation을 모두 제거한다.
    // 제거하지 않으면 저장→재실행 시 파일 annotation과 메모리 annotation이 이중으로 누적된다.
    for (int pi = 0; pi < g_pageCount; ++pi)
    {
        // 항상 독립 핸들 사용 — g_pdfPage와 충돌 방지
        FPDF_PAGE pg = FPDF_LoadPage(g_pdfDoc, pi);
        if (!pg) continue;

        bool modified = false;
        // 역순으로 순회해야 인덱스 밀림 없이 삭제 가능
        int cnt = FPDFPage_GetAnnotCount(pg);
        for (int ai = cnt - 1; ai >= 0; --ai)
        {
            FPDF_ANNOTATION ann = FPDFPage_GetAnnot(pg, ai);
            if (!ann) continue;
            FPDF_ANNOTATION_SUBTYPE st = FPDFAnnot_GetSubtype(ann);
            FPDFPage_CloseAnnot(ann);
            if (st == FPDF_ANNOT_HIGHLIGHT || st == FPDF_ANNOT_UNDERLINE ||
                st == FPDF_ANNOT_STRIKEOUT || st == FPDF_ANNOT_SQUIGGLY)
            {
                FPDFPage_RemoveAnnot(pg, ai);
                modified = true;
            }
        }
        if (modified)
            FPDFPage_GenerateContent(pg);
        FPDF_ClosePage(pg);
    }

    // 하이라이트와 밑줄, 취소선, 물결 밑줄을 통합 처리하기 위한 구조체
    struct AnnotItem {
        int pageIndex, charStart, charEnd;
        COLORREF color;
        bool isUnderline;
        bool isStrikeout;
        bool isSquiggly;
    };

    std::vector<AnnotItem> items;
    items.reserve(g_highlights.size() + g_underlines.size() + g_strikeouts.size() + g_squigglies.size());
    for (const auto& hl : g_highlights)
        items.push_back({ hl.pageIndex, hl.charStart, hl.charEnd, hl.color, false, false, false });
    for (const auto& ul : g_underlines)
        items.push_back({ ul.pageIndex, ul.charStart, ul.charEnd, ul.color, true, false, false });
    for (const auto& so : g_strikeouts)
        items.push_back({ so.pageIndex, so.charStart, so.charEnd, so.color, false, true, false });
    for (const auto& sq : g_squigglies)
        items.push_back({ sq.pageIndex, sq.charStart, sq.charEnd, sq.color, false, false, true });

    // 페이지별로 그룹화하여 페이지 로드 횟수 최소화
    // pageIndex 순으로 정렬
    std::sort(items.begin(), items.end(),
        [](const AnnotItem& a, const AnnotItem& b){ return a.pageIndex < b.pageIndex; });

    int lastPage = -1;
    FPDF_PAGE     workPage     = nullptr;
    FPDF_TEXTPAGE workTextPage = nullptr;
    bool          isCurrentPg  = false;

    auto closePage = [&]()
    {
        if (workTextPage && !isCurrentPg) { FPDFText_ClosePage(workTextPage); }
        workTextPage = nullptr;
        if (workPage && !isCurrentPg) { FPDF_ClosePage(workPage); }
        workPage = nullptr;
        lastPage = -1;
    };

    for (const auto& item : items)
    {
        if (item.pageIndex < 0 || item.pageIndex >= g_pageCount) continue;

        // 페이지가 바뀌면 이전 페이지 콘텐츠 생성 후 교체
        if (item.pageIndex != lastPage)
        {
            if (workPage && lastPage >= 0)
            {
                FPDFPage_GenerateContent(workPage);
            }
            closePage();

            isCurrentPg = false; // 항상 독립 핸들 사용 — g_pdfPage 공유 시 충돌 방지
            {
                workPage = FPDF_LoadPage(g_pdfDoc, item.pageIndex);
                if (!workPage) continue;
                workTextPage = FPDFText_LoadPage(workPage);
                if (!workTextPage) { FPDF_ClosePage(workPage); workPage = nullptr; continue; }
            }
            lastPage = item.pageIndex;
        }
        if (!workPage || !workTextPage) continue;

        const int firstIdx = std::min(item.charStart, item.charEnd);
        const int lastIdx  = std::max(item.charStart, item.charEnd);
        const int count    = lastIdx - firstIdx + 1;
        if (count <= 0) continue;

        const int nRects = FPDFText_CountRects(workTextPage, firstIdx, count);
        if (nRects <= 0) continue;

        struct PdfRect { double left, top, right, bottom; };
        std::vector<PdfRect> rects(nRects);
        for (int ri = 0; ri < nRects; ++ri)
            FPDFText_GetRect(workTextPage, ri, &rects[ri].left, &rects[ri].top,
                &rects[ri].right, &rects[ri].bottom);

        // 같은 줄 높이 통일
        for (size_t i = 0; i < rects.size(); ++i)
        {
            double ci = (rects[i].top + rects[i].bottom) * 0.5;
            double hi = rects[i].top - rects[i].bottom;
            if (hi <= 0) continue;
            for (size_t j = i + 1; j < rects.size(); ++j)
            {
                double cj = (rects[j].top + rects[j].bottom) * 0.5;
                double hj = rects[j].top - rects[j].bottom;
                if (hj <= 0) continue;
                double maxH = std::max(hi, hj);
                if (std::abs(ci - cj) < maxH * 0.7)
                {
                    double unionTop = std::max(rects[i].top, rects[j].top);
                    double unionBot = std::min(rects[i].bottom, rects[j].bottom);
                    rects[i].top = rects[j].top = unionTop;
                    rects[i].bottom = rects[j].bottom = unionBot;
                }
            }
        }

        // 같은 줄 무조건 병합
        for (size_t i = 0; i < rects.size(); ++i)
        {
            if (rects[i].left >= rects[i].right) continue;
            double lineH_i = rects[i].top - rects[i].bottom;
            bool merged = true;
            while (merged)
            {
                merged = false;
                for (size_t j = i + 1; j < rects.size(); ++j)
                {
                    if (rects[j].left >= rects[j].right) continue;
                    double lineH_j = rects[j].top - rects[j].bottom;
                    double maxH = std::max(lineH_i, lineH_j);
                    double ci = (rects[i].top + rects[i].bottom) * 0.5;
                    double cj = (rects[j].top + rects[j].bottom) * 0.5;
                    if (maxH <= 0 || std::abs(ci - cj) >= maxH * 0.7)
                        continue;
                    rects[i].left = std::min(rects[i].left, rects[j].left);
                    rects[i].right = std::max(rects[i].right, rects[j].right);
                    rects[i].top = std::max(rects[i].top, rects[j].top);
                    rects[i].bottom = std::min(rects[i].bottom, rects[j].bottom);
                    lineH_i = rects[i].top - rects[i].bottom;
                    rects[j].right = rects[j].left;
                    merged = true;
                }
            }
        }

        FPDF_ANNOTATION_SUBTYPE annotType = item.isStrikeout ? FPDF_ANNOT_STRIKEOUT
            : item.isSquiggly ? FPDF_ANNOT_SQUIGGLY
            : item.isUnderline ? FPDF_ANNOT_UNDERLINE : FPDF_ANNOT_HIGHLIGHT;
        FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(workPage, annotType);
        if (!annot) continue;

        // alpha=200: 텍스트 가독성을 위해 약간 투명하게 저장
        // annotation 종류는 subtype(FPDF_ANNOT_HIGHLIGHT 등)으로 구분하므로 alpha 태그 불필요.
        FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color,
            GetRValue(item.color), GetGValue(item.color), GetBValue(item.color), 200);

        double allLeft = 1e9, allTop = -1e9, allRight = -1e9, allBottom = 1e9;
        for (size_t ri = 0; ri < rects.size(); ++ri)
        {
            if (rects[ri].left >= rects[ri].right) continue;

            FS_QUADPOINTSF qp = {};
            qp.x1 = static_cast<float>(rects[ri].left);   qp.y1 = static_cast<float>(rects[ri].top);
            qp.x2 = static_cast<float>(rects[ri].right);  qp.y2 = static_cast<float>(rects[ri].top);
            qp.x3 = static_cast<float>(rects[ri].left);   qp.y3 = static_cast<float>(rects[ri].bottom);
            qp.x4 = static_cast<float>(rects[ri].right);  qp.y4 = static_cast<float>(rects[ri].bottom);
            FPDFAnnot_AppendAttachmentPoints(annot, &qp);

            allLeft   = std::min(allLeft,   rects[ri].left);
            allTop    = std::max(allTop,    rects[ri].top);
            allRight  = std::max(allRight,  rects[ri].right);
            allBottom = std::min(allBottom, rects[ri].bottom);
        }

        FS_RECTF rect = {};
        rect.left   = static_cast<float>(allLeft);
        rect.top    = static_cast<float>(allTop);
        rect.right  = static_cast<float>(allRight);
        rect.bottom = static_cast<float>(allBottom);
        FPDFAnnot_SetRect(annot, &rect);

        // charStart:charEnd 를 PlainPDFIdx 필드에 저장 — 재오픈 시 복원 용
        // (Contents 필드는 비워 두어 Acrobat 등 외부 뷰어에서 팔업 아이콘 미표시)
        {
            wchar_t buf[64];
            swprintf_s(buf, L"%d:%d", item.charStart, item.charEnd);
            FPDFAnnot_SetStringValue(annot, "PlainPDFIdx",
                reinterpret_cast<const FPDF_WCHAR*>(buf));
        }

        FPDFPage_CloseAnnot(annot);
    }

    // 마지막 페이지 콘텐츠 생성
    if (workPage && lastPage >= 0)
        FPDFPage_GenerateContent(workPage);
    closePage();
}

// 메모를 PDF FreeText 어노테이션으로 저장
void EmbedNoteAnnotations()
{
    if (!g_pdfDoc || g_notes.empty()) return;

    for (const auto& note : g_notes)
    {
        FPDF_PAGE page = (note.pageIndex == g_currentPage && g_pdfPage)
            ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, note.pageIndex);
        if (!page) continue;
        bool isCurrentPage = (note.pageIndex == g_currentPage && g_pdfPage);

        FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_TEXT);
        if (annot)
        {
            // 아이콘 위치 rect (작은 정사각형)
            FS_RECTF rect = {};
            rect.left   = static_cast<float>(note.pdfX);
            rect.top    = static_cast<float>(note.pdfY);
            rect.right  = static_cast<float>(note.pdfX + 20);
            rect.bottom = static_cast<float>(note.pdfY - 20);
            FPDFAnnot_SetRect(annot, &rect);

            // 내용 설정
            FPDFAnnot_SetStringValue(annot, "Contents",
                reinterpret_cast<const FPDF_WCHAR*>(note.text.c_str()));

            // 아이콘 종류 (Comment, Note, Help, NewParagraph, Paragraph, Insert 등)
            FPDFAnnot_SetStringValue(annot, "Name",
                reinterpret_cast<const FPDF_WCHAR*>(L"Comment"));

            // 노란색 (표준 스티커 메모 색상)
            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 255, 235, 80, 255);

            FPDFPage_CloseAnnot(annot);
        }

        FPDFPage_GenerateContent(page);
        if (!isCurrentPage) FPDF_ClosePage(page);
    }
}


bool SavePDFToFile(const std::wstring& path)
{
    if (!g_pdfDoc) return false;

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp)
        return false;

    FileWriteContext ctx = {};
    ctx.fileWrite.version = 1;
    ctx.fileWrite.WriteBlock = FileWriteBlock;
    ctx.fp = fp;

    FPDF_BOOL ok = FPDF_SaveAsCopy(g_pdfDoc, &ctx.fileWrite, 0);
    fclose(fp);
    return ok != 0;
}

// PDF 내용을 메모리 버퍼로 직렬화한다 (파일 I/O 없음).
// SaveDocument에서 ClosePDF() 이후에 파일에 쓸 수 있도록 분리한다.
static bool SavePDFToBuffer(std::vector<char>& outBuf)
{
    if (!g_pdfDoc) return false;

    struct MemCtx {
        FPDF_FILEWRITE fw;
        std::vector<char>* buf;
    };
    MemCtx ctx = {};
    ctx.fw.version = 1;
    ctx.fw.WriteBlock = [](FPDF_FILEWRITE_* self, const void* data, unsigned long size) -> int {
        auto* mc = reinterpret_cast<MemCtx*>(self);
        const char* p = static_cast<const char*>(data);
        mc->buf->insert(mc->buf->end(), p, p + size);
        return 1;
    };
    ctx.buf = &outBuf;

    outBuf.clear();
    return FPDF_SaveAsCopy(g_pdfDoc, &ctx.fw, 0) != 0;
}

static void EmbedInkAnnotations()
{
    if (!g_pdfDoc || g_inkStrokes.empty()) return;

    for (int pi = 0; pi < g_pageCount; ++pi)
    {
        // 이 페이지에 잉크 스트로크가 있는지 먼저 확인
        bool hasInk = false;
        for (const auto& sk : g_inkStrokes)
            if (sk.pageIndex == pi) { hasInk = true; break; }
        if (!hasInk) continue;

        bool isCur = (pi == g_currentPage) && g_pdfPage;
        FPDF_PAGE pg = isCur ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, pi);
        if (!pg) continue;

        bool modified = false;
        for (const auto& sk : g_inkStrokes)
        {
            if (sk.pageIndex != pi || sk.pdfPts.size() < 2) continue;

            std::vector<FS_POINTF> pts;
            pts.reserve(sk.pdfPts.size());
            for (auto& p : sk.pdfPts)
                pts.push_back({ (float)p.first, (float)p.second });

            FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(pg, FPDF_ANNOT_INK);
            if (!annot) continue;

            FPDFAnnot_AddInkStroke(annot, pts.data(), pts.size());

            // 앱 소유 표시 — 저장 시 외부 Ink(아이폰 서명 등)와 구별하기 위한 태그
            FPDFAnnot_SetStringValue(annot, "T",
                reinterpret_cast<const FPDF_WCHAR*>(L"PlainPDFInk"));

            // 바운딩 Rect
            float xmin = pts[0].x, xmax = pts[0].x;
            float ymin = pts[0].y, ymax = pts[0].y;
            for (auto& p : pts) {
                if (p.x < xmin) xmin = p.x; if (p.x > xmax) xmax = p.x;
                if (p.y < ymin) ymin = p.y; if (p.y > ymax) ymax = p.y;
            }
            constexpr float kPad = 4.0f;
            // FS_RECTF = { left, bottom, right, top } (PDF 하단원점 좌표계)
            // ymin = 페이지 하단 쪽, ymax = 페이지 상단 쪽
            FS_RECTF bbox{ xmin - kPad, ymin - kPad, xmax + kPad, ymax + kPad };
            FPDFAnnot_SetRect(annot, &bbox);

            unsigned int r = GetRValue(sk.color);
            unsigned int gv = GetGValue(sk.color);
            unsigned int b = GetBValue(sk.color);
            FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, r, gv, b, 255);
            FPDFPage_CloseAnnot(annot);
            modified = true;
        }

        if (modified)
            FPDFPage_GenerateContent(pg);
        if (!isCur)
            FPDF_ClosePage(pg);
    }
}

static void EmbedShapeAnnotations()
{
    if (!g_pdfDoc || g_shapes.empty()) return;

    for (int pi = 0; pi < g_pageCount; ++pi)
    {
        bool isCur = (pi == g_currentPage) && g_pdfPage;
        FPDF_PAGE pg = isCur ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, pi);
        if (!pg) continue;

        bool modified = false;
        for (const auto& sh : g_shapes)
        {
            if (sh.pageIndex != pi) continue;
            double pageW = g_pageLayouts[pi].pdfW;
            double pageH = g_pageLayouts[pi].pdfH;

            // PDF 하단원점 좌표계 → FS_RECTF (left, bottom, right, top)
            float pdfLeft   = (float)sh.pdfX;
            float pdfTop    = (float)sh.pdfY;            // 상단 (하단원점)
            float pdfRight  = (float)(sh.pdfX + sh.pdfW);
            float pdfBottom = (float)(sh.pdfY - sh.pdfH); // 하단 (하단원점)

            FPDF_ANNOTATION_SUBTYPE subtype = sh.isCircle ? FPDF_ANNOT_CIRCLE : FPDF_ANNOT_SQUARE;
            FPDF_ANNOTATION ann = FPDFPage_CreateAnnot(pg, subtype);
            if (!ann) continue;

            FS_RECTF rect{ pdfLeft, pdfBottom, pdfRight, pdfTop };
            FPDFAnnot_SetRect(ann, &rect);

            BYTE r = GetRValue(sh.color), gv = GetGValue(sh.color), b = GetBValue(sh.color);
            // 테두리 색
            FPDFAnnot_SetColor(ann, FPDFANNOT_COLORTYPE_Color, r, gv, b, 255);

            FPDFPage_CloseAnnot(ann);
            modified = true;
        }

        if (modified)
            FPDFPage_GenerateContent(pg);
        if (!isCur)
            FPDF_ClosePage(pg);
    }
}

static void SaveDocument(HWND hWnd, bool saveAs)
{
    if (!g_pdfDoc) return;

    std::wstring savePath = g_filePath;

    if (saveAs || savePath.empty())
    {
        WCHAR szFile[MAX_PATH] = {};
        if (!savePath.empty())
            wcsncpy_s(szFile, savePath.c_str(), _TRUNCATE);

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hWnd;
        ofn.lpstrFilter = L"PDF 파일 (*.pdf)\0*.pdf\0모든 파일 (*.*)\0*.*\0";
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        ofn.lpstrDefExt = L"pdf";

        if (!GetSaveFileNameW(&ofn))
            return;

        savePath = szFile;
    }

    // 서명 또는 redact가 있을 경우 저장 전 확인
    if (!g_signatures.empty() || !g_redacts.empty() || g_rotation != 0) {
        std::wstring msg = LS(STR_SAVE_PERMANENT);
        if (!g_signatures.empty()) msg += LS(STR_SAVE_SIGNATURE);
        if (!g_redacts.empty())    msg += LS(STR_SAVE_REDACT);
        if (g_rotation != 0) {
            static const wchar_t* rotNames[] = { L"0°", L"90°", L"180°", L"270°" };
            msg += std::wstring(LS(STR_SAVE_ROTATION)) + rotNames[g_rotation] + L")\n";
        }
        msg += LS(STR_SAVE_QUESTION);
        bool ok = CleanPopup::Show(hWnd, LS(STR_SAVE_CONFIRM_TITLE), msg, true);
        if (!ok) return;
    }

    // 저장 전 — 지연 로딩으로 아직 처리되지 않은 페이지의 주석을 모두 로드
    // (로드되지 않은 채 저장하면 해당 페이지 주석이 삭제됨)
    if (!g_annotPageLoaded.empty())
    {
        for (int pi = 0; pi < g_pageCount; ++pi)
        {
            if (pi < (int)g_annotPageLoaded.size() && !g_annotPageLoaded[pi])
                RunAnnotLoadBatch(hWnd, pi, pi + 1);
        }
    }

    // 강제 로드 후 메모리 컬렉션에 누적된 중복 주석 제거
    DeduplicateAnnotations();

    // Embed 함수들이 g_pdfPage와 독립된 핸들로 동작하도록 먼저 닫기
    if (g_pdfPage)
    {
        if (g_findHandle) { FPDFText_FindClose(g_findHandle); g_findHandle = nullptr; }
        if (g_textPage) { FPDFText_ClosePage(g_textPage); g_textPage = nullptr; }
        FPDF_ClosePage(g_pdfPage);
        g_pdfPage = nullptr;
    }

    // Embed 전에 앱이 관리하는 모든 주석 타입을 일괄 삭제 — 누적/중복 방지
    // (삭제/수정 후 저장해도 기존 주석이 남아 겹치는 문제 해결)
    {
        static const FPDF_ANNOTATION_SUBTYPE kManagedTypes[] = {
            FPDF_ANNOT_HIGHLIGHT, FPDF_ANNOT_UNDERLINE,
            FPDF_ANNOT_STRIKEOUT, FPDF_ANNOT_SQUIGGLY,
            FPDF_ANNOT_TEXT,      FPDF_ANNOT_FREETEXT,
            FPDF_ANNOT_CIRCLE,    FPDF_ANNOT_SQUARE,
        };
        for (int pi = 0; pi < g_pageCount; ++pi)
        {
            FPDF_PAGE pg = FPDF_LoadPage(g_pdfDoc, pi);
            if (!pg) continue;
            bool modified = false;
            int cnt = FPDFPage_GetAnnotCount(pg);
            for (int ai = cnt - 1; ai >= 0; --ai)
            {
                FPDF_ANNOTATION ann = FPDFPage_GetAnnot(pg, ai);
                if (!ann) continue;
                FPDF_ANNOTATION_SUBTYPE st = FPDFAnnot_GetSubtype(ann);

                // Ink 어노테이션은 앱 소유(PlainPDFSignature 태그)인 것만 삭제.
                // 외부에서 삽입된 잉크(아이폰 서명 등)는 보존.
                if (st == FPDF_ANNOT_INK)
                {
                    unsigned long tLen = FPDFAnnot_GetStringValue(ann, "T", nullptr, 0);
                    bool isOwned = false;
                    if (tLen > 0)
                    {
                        std::vector<unsigned short> tbuf(tLen / sizeof(unsigned short) + 2, 0);
                        FPDFAnnot_GetStringValue(ann, "T",
                            reinterpret_cast<FPDF_WCHAR*>(tbuf.data()), tLen);
                        std::wstring tval = reinterpret_cast<wchar_t*>(tbuf.data());
                        isOwned = (tval == L"PlainPDFSignature" || tval == L"PlainPDFInk");
                    }
                    FPDFPage_CloseAnnot(ann);
                    if (isOwned) { FPDFPage_RemoveAnnot(pg, ai); modified = true; }
                    continue;
                }

                FPDFPage_CloseAnnot(ann);
                for (auto t : kManagedTypes)
                {
                    if (st == t) { FPDFPage_RemoveAnnot(pg, ai); modified = true; break; }
                }
            }
            if (modified) FPDFPage_GenerateContent(pg);
            FPDF_ClosePage(pg);
        }
    }

    // 하이라이트를 PDF 어노테이션으로 삽입 (밑줄은 메모리에서만 관리)
    EmbedHighlightAnnotations();
    EmbedNoteAnnotations();
    EmbedTextBoxAnnotations();
    EmbedSignatureObjects();
    EmbedInkAnnotations();
    EmbedShapeAnnotations();

    // Redact 영구 적용: 해당 영역을 검은 사각형으로 덮고 텍스트 오브젝트 제거
    if (!g_redacts.empty())
    {
        for (const auto& rd : g_redacts)
        {
            if (rd.pageIndex < 0 || rd.pageIndex >= g_pageCount) continue;

            // 항상 새로 로드 (g_pdfPage와 별개로 편집용 페이지 사용)
            FPDF_PAGE pg = FPDF_LoadPage(g_pdfDoc, rd.pageIndex);
            if (!pg) continue;

            float l = (float)rd.pdfX;
            float b = (float)(rd.pdfY - rd.pdfH);
            float r = (float)(rd.pdfX + rd.pdfW);
            float t = (float)rd.pdfY;

            // 영역 내 페이지 오브젝트 제거 (텍스트/이미지)
            int objCount = FPDFPage_CountObjects(pg);
            for (int oi = objCount - 1; oi >= 0; oi--)
            {
                FPDF_PAGEOBJECT obj = FPDFPage_GetObject(pg, oi);
                if (!obj) continue;
                float ox1, oy1, ox2, oy2;
                if (FPDFPageObj_GetBounds(obj, &ox1, &oy1, &ox2, &oy2))
                {
                    // 오브젝트가 redact 영역 안에 완전히 포함될 때만 제거
                    bool contained = (ox1 >= l && ox2 <= r && oy1 >= b && oy2 <= t);
                    if (contained)
                    {
                        FPDFPage_RemoveObject(pg, obj);
                        FPDFPageObj_Destroy(obj);
                    }
                }
            }

            // 검은 사각형 path 오브젝트로 덮기
            FPDF_PAGEOBJECT blkRect = FPDFPageObj_CreateNewRect(l, b, r - l, t - b);
            if (blkRect)
            {
                FPDFPageObj_SetFillColor(blkRect, 0, 0, 0, 255);
                FPDFPageObj_SetStrokeColor(blkRect, 0, 0, 0, 255);
                FPDFPath_SetDrawMode(blkRect, 1, 0); // fill only
                FPDFPage_InsertObject(pg, blkRect);
            }

            FPDFPage_GenerateContent(pg);
            FPDF_ClosePage(pg);
        }
        g_redacts.clear();
        g_selectedRedact = -1;

        // 현재 열린 페이지를 다시 로드하여 변경사항 반영
        if (g_pdfPage)
        {
            FPDF_ClosePage(g_pdfPage);
            g_pdfPage = FPDF_LoadPage(g_pdfDoc, g_currentPage);
        }
    }

    // 회전 영구 적용: 전체 페이지에 현재 회전값 저장
    bool anyPageRot = false;
    for (int r : g_pageRot) if (r) { anyPageRot = true; break; }
    if (g_rotation != 0 || anyPageRot)
    {
        for (int pi = 0; pi < g_pageCount; ++pi)
        {
            FPDF_PAGE pg = FPDF_LoadPage(g_pdfDoc, pi);
            if (pg)
            {
                // 기존 페이지 내장 회전에 전체 회전과 페이지별 회전을 누적 적용
                int existing = FPDFPage_GetRotation(pg);
                FPDFPage_SetRotation(pg, (existing + GetEffectiveRotation(pi)) % 4);
                FPDFPage_GenerateContent(pg);
                FPDF_ClosePage(pg);
            }
        }
        g_pageRot.clear();
        g_rotation = 0; // 저장 후 회전 초기화
    }

    // flatten: 서명이 있을 때만 페이지 오브젝트를 병합한다.
    // 서명이 없을 경우 flatten을 건너뜀으로써 하이라이트 등 annotation 객체가
    // PDF 파일에 annotation으로 유지되어 재실행 후에도 다시 로드할 수 있다.
    if (!g_signatures.empty())
    {
        for (int pi = 0; pi < g_pageCount; ++pi) {
            FPDF_PAGE pg = FPDF_LoadPage(g_pdfDoc, pi);
            if (pg) {
                FPDFPage_Flatten(pg, FLAT_PRINT);
                FPDFPage_GenerateContent(pg);
                FPDF_ClosePage(pg);
            }
        }
        // 현재 페이지 새로고침 (flatten 후 반영)
        if (g_pdfPage)
        {
            FPDF_ClosePage(g_pdfPage);
            g_pdfPage = FPDF_LoadPage(g_pdfDoc, g_currentPage);
        }
    }

    // 저장 전 열린 페이지를 반드시 닫아야 PDFium이 안전하게 직렬화한다.
    // (앞에서 이미 닫혔을 수 있으나 flatten 등으로 재오픈된 경우 대비)
    if (g_pdfPage)
    {
        if (g_findHandle) { FPDFText_FindClose(g_findHandle); g_findHandle = nullptr; }
        if (g_textPage) { FPDFText_ClosePage(g_textPage); g_textPage = nullptr; }
        FPDF_ClosePage(g_pdfPage);
        g_pdfPage = nullptr;
    }

    // PDF를 메모리 버퍼로 직렬화 — 파일에 쓰기 전에 ClosePDF()로 핸들을 먼저 해제한다.
    // (파일이 열려 있는 상태에서 임시 파일로 저장 후 이동하면 파일 잠금으로 실패함)
    std::vector<char> pdfBuf;
    if (!SavePDFToBuffer(pdfBuf))
    {
        MessageBoxW(hWnd, LS(STR_SAVE_FAIL), LS(STR_ERROR), MB_OK | MB_ICONERROR);
        return;
    }

    // 현재 상태 기억
    int savedPage = g_currentPage;
    int savedScrollX = g_scrollX;
    int savedScrollY = g_scrollY;
    float savedZoom = g_zoom;
    int savedRotation = g_rotation;
    bool savedDark = g_darkMode;
    std::wstring originalPath = g_filePath;
    bool isSameFile = (savePath == originalPath);

    // flatten 후 저장된 PDF에서는 어노테이션이 페이지 콘텐츠에 병합되어
    // LoadXxxAnnotations()가 빈 결과를 반환한다.
    // 따라서 닫기 전에 메모리 상태를 그대로 보존한다.
    std::vector<HighlightItem>  savedHighlights  = g_highlights;
    std::vector<UnderlineItem>  savedUnderlines  = g_underlines;
    std::vector<StrikeoutItem>  savedStrikeouts  = g_strikeouts;
    std::vector<SquigglyItem>   savedSquigglies  = g_squigglies;
    std::vector<NoteItem>       savedNotes       = g_notes;
    std::vector<TextBoxItem>    savedTextBoxes   = g_textBoxes;
    std::vector<ShapeItem>      savedShapes      = g_shapes;
    std::vector<InkStrokeItem>  savedInkStrokes  = g_inkStrokes;
    std::vector<SignatureItem>  savedSignatures  = g_signatures;
    for (auto& s : savedSignatures)
        s.bitmap = nullptr;

    // PDF 닫기 — 파일 핸들 완전 해제
    // pdfBuffer도 명시적으로 해제 (FPDF_LoadMemDocument 수명 버퍼)
    if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
        g_tabs[g_activeTab].pdfBuffer.reset();
    ClosePDF();

    // 탐색기 미리보기 등 외부 프로세스가 파일을 읽기로 열고 있으면
    // fopen("wb") 직접 덮어쓰기는 SHARING_VIOLATION(32)으로 실패한다.
    // 그러나 FILE_SHARE_DELETE는 허용하므로, 새 이름(.tmp)에 쓴 뒤
    // MoveFileExW(MOVEFILE_REPLACE_EXISTING)으로 원자적 교체하면 성공한다.
    {
        std::wstring tmpPath = savePath + L".tmp";

        // 새 파일명에 버퍼 쓰기 (새 파일이므로 공유 위반 없음)
        bool tmpOk = false;
        {
            FILE* fp = nullptr;
            if (_wfopen_s(&fp, tmpPath.c_str(), L"wb") == 0 && fp)
            {
                size_t wr = fwrite(pdfBuf.data(), 1, pdfBuf.size(), fp);
                fclose(fp);
                tmpOk = (wr == pdfBuf.size());
            }
        }
        if (!tmpOk)
        {
            DeleteFileW(tmpPath.c_str());
            MessageBoxW(hWnd, LS(STR_SAVE_FAIL), LS(STR_ERROR), MB_OK | MB_ICONERROR);
            return;
        }

        // .tmp → 원본 이름으로 원자적 교체
        // 읽기 전용 속성 먼저 해제
        SetFileAttributesW(savePath.c_str(), FILE_ATTRIBUTE_NORMAL);

        bool replaced = false;

        // 1차 시도: MoveFileExW (가장 안전한 원자적 교체)
        if (MoveFileExW(tmpPath.c_str(), savePath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            replaced = true;
        }

        // 2차 시도: ReplaceFileW (열려있는 파일 교체 전용 API)
        if (!replaced)
        {
            if (ReplaceFileW(savePath.c_str(), tmpPath.c_str(), nullptr,
                             REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                             nullptr, nullptr))
                replaced = true;
        }

        // 3차 시도: CreateFileW로 FILE_SHARE_WRITE 허용하며 직접 덮어쓰기
        // 탐색기 미리보기 등이 FILE_SHARE_WRITE를 허용하면 이 방법으로 성공한다.
        if (!replaced)
        {
            HANDLE hDst = CreateFileW(savePath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, TRUNCATE_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);
            if (hDst != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                BOOL ok = WriteFile(hDst, pdfBuf.data(), (DWORD)pdfBuf.size(), &written, nullptr);
                CloseHandle(hDst);
                if (ok && written == (DWORD)pdfBuf.size())
                {
                    DeleteFileW(tmpPath.c_str());
                    replaced = true;
                }
            }
        }
        if (!replaced)
        {
            DeleteFileW(tmpPath.c_str());

            // 파일 교체 실패 — ClosePDF()로 닫힌 문서를 메모리 버퍼로 복원
            // pdfium은 LoadMemDocument에 넘긴 포인터를 문서 수명 동안 계속 참조하므로
            // 탭 버퍼에 먼저 이동(move)한 뒤 그 포인터를 사용해야 한다.
            {
                std::shared_ptr<std::vector<char>> liveBuf;
                if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
                {
                    g_tabs[g_activeTab].filePath  = originalPath;
                    g_tabs[g_activeTab].pdfBuffer = std::make_shared<std::vector<char>>(std::move(pdfBuf));
                    liveBuf = g_tabs[g_activeTab].pdfBuffer;
                }

                if (liveBuf && !liveBuf->empty())
                {
                    FPDF_DOCUMENT recoveredDoc = FPDF_LoadMemDocument(
                        liveBuf->data(), (int)liveBuf->size(), nullptr);
                    if (recoveredDoc)
                    {
                        g_pdfDoc    = recoveredDoc;
                        g_filePath  = originalPath;
                        g_pageCount = FPDF_GetPageCount(g_pdfDoc);

                        g_zoom      = savedZoom;
                        g_rotation  = savedRotation;
                        g_darkMode  = savedDark;
                        g_highlights = std::move(savedHighlights);
                        g_underlines = std::move(savedUnderlines);
                        g_strikeouts = std::move(savedStrikeouts);
                        g_squigglies = std::move(savedSquigglies);
                        g_notes      = std::move(savedNotes);
                        g_textBoxes  = std::move(savedTextBoxes);
                        g_shapes     = std::move(savedShapes);
                        g_inkStrokes = std::move(savedInkStrokes);
                        for (auto& s : g_signatures) delete s.bitmap;
                        g_signatures = std::move(savedSignatures);

                        InitPageCache(g_pageCount);
                        g_annotPageLoaded.assign(g_pageCount, true);
                        g_annotPagePosted.assign(g_pageCount, true);
                        g_annotLoadReady = true;
                        RecalcPageLayouts();
                        LoadPage(hWnd, std::min(savedPage, g_pageCount - 1));
                        g_scrollX  = savedScrollX;
                        g_scrollY  = savedScrollY;
                        g_scrollYf = static_cast<float>(savedScrollY);
                        UpdateScrollBars(hWnd);
                        UpdateTitle(hWnd);
                        InvalidateRect(hWnd, nullptr, TRUE);
                    }
                }
            }

            CleanPopup::Show(hWnd, LS(STR_WARNING), LS(STR_SAVE_REPLACE_WARN));
            return;
        }
    }

    // 저장된 파일 다시 열기
    // zoom/scroll/page 등 현재 상태를 그대로 유지한다.
    {
        std::vector<char> newBuf;
        FPDF_DOCUMENT newDoc = LoadPDFFromFile(savePath, &newBuf);
        if (!newDoc) {
            MessageBoxW(hWnd, LS(STR_SAVE_RELOAD_FAIL), LS(STR_ERROR), MB_OK | MB_ICONERROR);
            return;
        }
        g_pdfDoc    = newDoc;
        g_filePath  = savePath;
        g_pageCount = FPDF_GetPageCount(g_pdfDoc);
        if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
        {
            g_tabs[g_activeTab].filePath = savePath;
            g_tabs[g_activeTab].pdfBuffer = std::make_shared<std::vector<char>>(std::move(newBuf));
        }
    }

    // 재로드된 PDF에 내장된 앱 관리 주석을 메모리상에서 제거한다.
    // 그렇지 않으면 PDFium FPDF_ANNOT 렌더링과 앱 오버레이가 이중으로 그려진다.
    {
        static const FPDF_ANNOTATION_SUBTYPE kManagedTypes[] = {
            FPDF_ANNOT_HIGHLIGHT, FPDF_ANNOT_UNDERLINE,
            FPDF_ANNOT_STRIKEOUT, FPDF_ANNOT_SQUIGGLY,
            FPDF_ANNOT_TEXT,      FPDF_ANNOT_FREETEXT,
            FPDF_ANNOT_CIRCLE,    FPDF_ANNOT_SQUARE,
        };
        int reloadPageCount = FPDF_GetPageCount(g_pdfDoc);
        for (int pi = 0; pi < reloadPageCount; ++pi)
        {
            FPDF_PAGE pg = FPDF_LoadPage(g_pdfDoc, pi);
            if (!pg) continue;
            bool modified = false;
            int cnt = FPDFPage_GetAnnotCount(pg);
            for (int ai = cnt - 1; ai >= 0; --ai)
            {
                FPDF_ANNOTATION ann = FPDFPage_GetAnnot(pg, ai);
                if (!ann) continue;
                FPDF_ANNOTATION_SUBTYPE st = FPDFAnnot_GetSubtype(ann);
                if (st == FPDF_ANNOT_INK)
                {
                    unsigned long tLen = FPDFAnnot_GetStringValue(ann, "T", nullptr, 0);
                    bool isOwned = false;
                    if (tLen > 0)
                    {
                        std::vector<unsigned short> tbuf(tLen / sizeof(unsigned short) + 2, 0);
                        FPDFAnnot_GetStringValue(ann, "T",
                            reinterpret_cast<FPDF_WCHAR*>(tbuf.data()), tLen);
                        std::wstring tval = reinterpret_cast<wchar_t*>(tbuf.data());
                        isOwned = (tval == L"PlainPDFSignature" || tval == L"PlainPDFInk");
                    }
                    FPDFPage_CloseAnnot(ann);
                    if (isOwned) { FPDFPage_RemoveAnnot(pg, ai); modified = true; }
                    continue;
                }
                FPDFPage_CloseAnnot(ann);
                for (auto t : kManagedTypes)
                {
                    if (st == t) { FPDFPage_RemoveAnnot(pg, ai); modified = true; break; }
                }
            }
            if (modified) FPDFPage_GenerateContent(pg);
            FPDF_ClosePage(pg);
        }
    }

    // 저장 전 상태 그대로 복원
    g_zoom      = savedZoom;
    g_rotation  = savedRotation;
    g_darkMode  = savedDark;

    // flatten 후 PDF에서는 어노테이션 객체가 소멸되므로 Load 대신 보존한 데이터 복원
    g_highlights = std::move(savedHighlights);
    g_underlines = std::move(savedUnderlines);
    g_strikeouts = std::move(savedStrikeouts);
    g_squigglies = std::move(savedSquigglies);
    g_notes      = std::move(savedNotes);
    g_textBoxes  = std::move(savedTextBoxes);
    g_shapes     = std::move(savedShapes);
    g_inkStrokes = std::move(savedInkStrokes);
    for (auto& s : g_signatures) delete s.bitmap;
    g_signatures = std::move(savedSignatures);
    // 다른 이름으로 저장한 경우 PDF 파일이 달라질 수 있으므로 rect 캐시 초기화
    if (!isSameFile) {
        for (auto& h : g_highlights) h.cachedRects.clear();
        for (auto& u : g_underlines) u.cachedRects.clear();
        for (auto& s : g_strikeouts) s.cachedRects.clear();
        for (auto& q : g_squigglies) q.cachedRects.clear();
    }
    ExtractBookmarks();
    LoadUserBookmarks();
    AddToRecentFiles(savePath);
    UpdateRecentFilesMenu(hWnd);

    InitPageCache(g_pageCount);
    // 저장 후 재오픈 시 주석은 이미 메모리에 보존되어 있으므로
    // 모든 페이지를 로드 완료 상태로 표시 — PDF에서 다시 읽지 않도록
    g_annotPageLoaded.assign(g_pageCount, true);
    g_annotPagePosted.assign(g_pageCount, true);
    g_annotLoadReady = true;
    RecalcPageLayouts();
    DestroyFormFill();
    InitFormFill(hWnd);
    LoadPage(hWnd, std::min(savedPage, g_pageCount - 1));
    g_scrollX  = savedScrollX;
    g_scrollY  = savedScrollY;
    g_scrollYf = static_cast<float>(savedScrollY);
    UpdateScrollBars(hWnd);
    UpdateTitle(hWnd);

    ClearThumbCache();
    InitThumbCache(g_pageCount);
    InvalidateRect(hWnd, nullptr, TRUE);

    ShowToast(hWnd, LS(STR_SAVED_TOAST));
    g_dirty = false;
}

// ─── 텍스트 선택 하이라이트 그리기 ───────────────────────────────────────────

static void DrawTextSelection(HDC hdc, const RECT& clientRect)
{
    if (!g_pdfDoc) return;
    if (!g_hasSelection && !g_isSelecting) return;
    if (g_selCharStart < 0 || g_selCharEnd < 0) return;
    if (g_selCharStart == g_selCharEnd) return;

    const int selPage = (g_selPage >= 0) ? g_selPage : g_currentPage;
    if (selPage < 0 || selPage >= g_pageCount) return;

    // 뷰포트에 보이지 않는 페이지는 건너뜀
    const RECT vp = GetViewportRect(clientRect);
    int pgTop2 = vp.top + g_pageLayouts[selPage].yOffset - g_scrollY;
    int pgBot2 = pgTop2 + g_pageLayouts[selPage].renderH;
    if (pgBot2 < vp.top || pgTop2 > vp.bottom) return;

    FPDF_PAGE page; FPDF_TEXTPAGE textPage;
    bool needClose = false;
    if (selPage == g_currentPage && g_pdfPage && g_textPage) {
        page = g_pdfPage; textPage = g_textPage;
    } else {
        page = FPDF_LoadPage(g_pdfDoc, selPage);
        if (!page) return;
        textPage = FPDFText_LoadPage(page);
        if (!textPage) { FPDF_ClosePage(page); return; }
        needClose = true;
    }

    const int firstIdx = std::min(g_selCharStart, g_selCharEnd);
    const int lastIdx = std::max(g_selCharStart, g_selCharEnd);
    const int count = lastIdx - firstIdx + 1;
    if (count <= 0) { if (needClose) { FPDFText_ClosePage(textPage); FPDF_ClosePage(page); } return; }

    const double pageW = g_pageLayouts[selPage].pdfW;
    const double pageH = g_pageLayouts[selPage].pdfH;
    int          pgLeft, pgTop;
    GetPageScreenPos(selPage, vp, &pgLeft, &pgTop);

    // 1단계: 모든 사각형 수집 (캐시 재사용)
    struct PdfRect { double left, top, right, bottom; };

    // rect 계산은 PDF 좌표계 기반 — 줌/선택범위만 변경 시 재계산, 스크롤은 무관
    bool cacheHit = (g_selRectCache.charStart == firstIdx &&
                     g_selRectCache.charEnd   == lastIdx  &&
                     g_selRectCache.zoom      == g_zoom);

    if (!cacheHit)
    {
        const int nRaw = FPDFText_CountRects(textPage, firstIdx, count);
        g_selRectCache.rects.resize(nRaw);
        for (int ri = 0; ri < nRaw; ++ri)
        {
            FPDFText_GetRect(textPage, ri,
                &g_selRectCache.rects[ri].left, &g_selRectCache.rects[ri].top,
                &g_selRectCache.rects[ri].right, &g_selRectCache.rects[ri].bottom);
            double h = g_selRectCache.rects[ri].top - g_selRectCache.rects[ri].bottom;
            if (h > 0) g_selRectCache.rects[ri].bottom -= h * 0.1;
        }

        // 2단계: top 기준 내림차순 정렬 후 O(n) 스캔으로 같은 줄 병합
        auto& R = g_selRectCache.rects;

        // 유효한 rect만 걸러낸 뒤 top 내림차순(PDF 좌표계: top이 클수록 위)으로 정렬
        R.erase(std::remove_if(R.begin(), R.end(),
            [](const SelRectCache::R& r){ return r.left >= r.right || r.top <= r.bottom; }), R.end());

        std::sort(R.begin(), R.end(),
            [](const SelRectCache::R& a, const SelRectCache::R& b){ return a.top > b.top; });

        // 같은 줄 판정 후 병합: 중심 y가 현재 그룹 높이의 70% 이내면 같은 줄
        std::vector<SelRectCache::R> mergedRects;
        mergedRects.reserve(R.size());
        for (const SelRectCache::R& r : R)
        {
            double h = r.top - r.bottom;
            if (h <= 0) continue;
            if (!mergedRects.empty())
            {
                SelRectCache::R& last = mergedRects.back();
                double lastH  = last.top - last.bottom;
                double maxH   = lastH > h ? lastH : h;
                double lastCy = (last.top + last.bottom) * 0.5;
                double curCy  = (r.top    + r.bottom)    * 0.5;
                if (maxH > 0 && (lastCy - curCy < 0 ? curCy - lastCy : lastCy - curCy) < maxH * 0.7)
                {
                    // 같은 줄 — 기존 그룹에 흡수
                    if (r.left   < last.left)   last.left   = r.left;
                    if (r.right  > last.right)  last.right  = r.right;
                    if (r.top    > last.top)     last.top    = r.top;
                    if (r.bottom < last.bottom)  last.bottom = r.bottom;
                    continue;
                }
            }
            mergedRects.push_back(r);
        }
        R = mergedRects;

        // 캐시 키 저장
        g_selRectCache.charStart = firstIdx;
        g_selRectCache.charEnd   = lastIdx;
        g_selRectCache.zoom      = g_zoom;
    }
    // 캐시에서 직접 참조 (복사 없음)
    const auto& cachedRects = g_selRectCache.rects;
    const int nRects = (int)cachedRects.size();

    // 3단계: 하이라이트 그리기 — 전역 영구 1x1 DC로 AlphaBlend
    if (!g_selBlendDC)
    {
        g_selBlendDC  = CreateCompatibleDC(hdc);
        g_selBlendBmp = CreateCompatibleBitmap(hdc, 1, 1);
        g_selBlendOld = static_cast<HBITMAP>(SelectObject(g_selBlendDC, g_selBlendBmp));
    }
    // 색상이 바뀔 수 있으므로 매번 채움 (FillRect는 매우 빠름)
    {
        HBRUSH selBr = CreateSolidBrush(g_selectionColor);
        RECT   selFr = { 0, 0, 1, 1 };
        FillRect(g_selBlendDC, &selFr, selBr);
        DeleteObject(selBr);
    }
    BLENDFUNCTION selBf = { AC_SRC_OVER, 0, 110, 0 };

    for (int ri = 0; ri < nRects; ++ri)
    {
        if (cachedRects[ri].left >= cachedRects[ri].right) continue;

        int sx1, sy1, sx2, sy2;
        PdfToScreen(cachedRects[ri].left, cachedRects[ri].top, pageW, pageH,
            pgLeft, pgTop, g_zoom, GetEffectiveRotation(selPage), &sx1, &sy1);
        PdfToScreen(cachedRects[ri].right, cachedRects[ri].bottom, pageW, pageH,
            pgLeft, pgTop, g_zoom, GetEffectiveRotation(selPage), &sx2, &sy2);
        if (sx1 > sx2) std::swap(sx1, sx2);
        if (sy1 > sy2) std::swap(sy1, sy2);

        int cx1 = std::max(sx1, (int)vp.left);
        int cy1 = std::max(sy1, (int)vp.top);
        int cx2 = std::min(sx2, (int)vp.right);
        int cy2 = std::min(sy2, (int)vp.bottom);
        if (cx2 <= cx1 || cy2 <= cy1) continue;

        const int bw = cx2 - cx1, bh = cy2 - cy1;
        AlphaBlend(hdc, cx1, cy1, bw, bh, g_selBlendDC, 0, 0, 1, 1, selBf);
    }

    if (needClose) { FPDFText_ClosePage(textPage); FPDF_ClosePage(page); }
}

// ─── 선택 텍스트 클립보드 복사 ────────────────────────────────────────────────

static void CopySelectionToClipboard(HWND hWnd)
{
    if (!g_pdfDoc || !g_hasSelection) return;
    if (g_selCharStart < 0 || g_selCharEnd < 0) return;

    const int selPage = (g_selPage >= 0) ? g_selPage : g_currentPage;
    FPDF_PAGE page; FPDF_TEXTPAGE textPage;
    bool needClose = false;
    if (selPage == g_currentPage && g_pdfPage && g_textPage) {
        page = g_pdfPage; textPage = g_textPage;
    } else {
        page = FPDF_LoadPage(g_pdfDoc, selPage);
        if (!page) return;
        textPage = FPDFText_LoadPage(page);
        if (!textPage) { FPDF_ClosePage(page); return; }
        needClose = true;
    }

    const int firstIdx = std::min(g_selCharStart, g_selCharEnd);
    const int lastIdx = std::max(g_selCharStart, g_selCharEnd);
    const int count = lastIdx - firstIdx + 1;  // ← +1 추가
    if (count <= 0) { if (needClose) { FPDFText_ClosePage(textPage); FPDF_ClosePage(page); } return; }

    std::vector<WCHAR> buf(count + 2, 0);
    const int written = FPDFText_GetText(textPage, firstIdx, count,
        reinterpret_cast<unsigned short*>(buf.data()));
    if (needClose) { FPDFText_ClosePage(textPage); FPDF_ClosePage(page); }
    if (written <= 0) return;

    const size_t byteSize = static_cast<size_t>(written) * sizeof(WCHAR);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, byteSize);
    if (!hMem) return;

    void* pMem = GlobalLock(hMem);
    if (!pMem) { GlobalFree(hMem); return; }
    memcpy(pMem, buf.data(), byteSize);
    GlobalUnlock(hMem);

    if (OpenClipboard(hWnd))
    {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, hMem);
        CloseClipboard();
    }
    else
    {
        GlobalFree(hMem);
    }
}

// ─── 북마크/목차 사이드바 ─────────────────────────────────────────────────────

static void ExtractBookmarksRecursive(FPDF_DOCUMENT doc, FPDF_BOOKMARK parent, int level)
{
    FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(doc, parent);
    while (child)
    {
        BookmarkItem item;
        item.level = level;
        item.isOpen = (level == 0);  // 루트만 기본 펼침

        // 제목 추출
        unsigned long titleLen = FPDFBookmark_GetTitle(child, nullptr, 0);
        if (titleLen > 0)
        {
            std::vector<unsigned short> buf(titleLen / 2 + 1, 0);
            FPDFBookmark_GetTitle(child, buf.data(), titleLen);
            item.title = reinterpret_cast<const wchar_t*>(buf.data());
        }

        // 목적지 페이지
        FPDF_DEST dest = FPDFBookmark_GetDest(doc, child);
        item.pageIndex = dest ? (int)FPDFDest_GetDestPageIndex(doc, dest) : -1;

        // 자식 존재 여부
        item.hasChildren = (FPDFBookmark_GetFirstChild(doc, child) != nullptr);

        g_bookmarks.push_back(item);

        // 재귀적으로 자식 추출
        if (item.hasChildren)
            ExtractBookmarksRecursive(doc, child, level + 1);

        child = FPDFBookmark_GetNextSibling(doc, child);
    }
}

static void ExtractBookmarks()
{
    g_bookmarks.clear();
    g_sidebarScrollY = 0;
    g_sidebarHoverIdx = -1;
    g_thumbScrollY = 0;
    g_thumbHoverIdx = -1;
    if (!g_pdfDoc) return;
    ExtractBookmarksRecursive(g_pdfDoc, nullptr, 0);
}

// ── 사용자 북마크 사이드카 파일 경로 계산 ────────────────────────────────────
static std::wstring GetUserBookmarksPath()
{
    if (g_filePath.empty()) return L"";

    // 내 문서\PlainPDFViewer\bookmark 폴더 경로 획득
    wchar_t* docPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docPath)))
        return L"";
    std::wstring baseDir = std::wstring(docPath) + L"\\PlainPDFViewer\\bookmark";
    CoTaskMemFree(docPath);

    // 폴더 생성 (없으면)
    CreateDirectoryW((std::wstring(baseDir.c_str(), baseDir.find(L"\\bookmark") + 1) +
                      L"PlainPDFViewer").c_str(), nullptr);
    CreateDirectoryW(baseDir.c_str(), nullptr);

    // PDF 경로에서 파일명(확장자 제외) 추출
    std::wstring fname = g_filePath;
    auto sl = fname.find_last_of(L"\\/");
    if (sl != std::wstring::npos) fname = fname.substr(sl + 1);
    auto dot = fname.rfind(L'.');
    if (dot != std::wstring::npos) fname = fname.substr(0, dot);

    // 전체 경로 기반 해시(8자리 hex)로 동명 파일 충돌 방지
    std::hash<std::wstring> hasher;
    unsigned int h = (unsigned int)(hasher(g_filePath) & 0xFFFFFFFF);
    wchar_t hashStr[16];
    swprintf_s(hashStr, L"_%08X", h);

    return baseDir + L"\\" + fname + hashStr + L".bookmarks";
}

// ── 사용자 북마크 저장 (간단한 텍스트 포맷) ──────────────────────────────────
// 형식: 한 줄에 "pageIndex\ttitle"
static void SaveUserBookmarks()
{
    std::wstring path = GetUserBookmarksPath();
    if (path.empty()) return;

    // 사용자 추가 북마크만 수집
    std::vector<BookmarkItem> userBms;
    for (auto& bm : g_bookmarks)
        if (bm.isUserAdded) userBms.push_back(bm);

    if (userBms.empty())
    {
        // 파일이 있으면 삭제 (모두 지운 경우)
        DeleteFileW(path.c_str());
        return;
    }

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8");
    if (!f) return;
    for (auto& bm : userBms)
        fwprintf(f, L"%d\t%s\n", bm.pageIndex, bm.title.c_str());
    fclose(f);
}

// ── 사용자 북마크 로드 ────────────────────────────────────────────────────────
static void LoadUserBookmarks()
{
    std::wstring path = GetUserBookmarksPath();
    if (path.empty()) return;

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8");
    if (!f) return;

    wchar_t line[1024];
    while (fgetws(line, _countof(line), f))
    {
        // 줄 끝 개행 제거
        int len = (int)wcslen(line);
        while (len > 0 && (line[len-1] == L'\n' || line[len-1] == L'\r'))
            line[--len] = L'\0';
        if (len == 0) continue;

        // pageIndex\ttitle 파싱
        wchar_t* tab = wcschr(line, L'\t');
        if (!tab) continue;
        *tab = L'\0';
        int pageIdx = _wtoi(line);
        std::wstring title(tab + 1);
        if (title.empty()) continue;

        BookmarkItem bm;
        bm.pageIndex   = pageIdx;
        bm.title       = title;
        bm.level       = 0;
        bm.isUserAdded = true;
        g_bookmarks.push_back(bm);
    }
    fclose(f);
}

// ── 현재 페이지를 사용자 북마크로 추가 ───────────────────────────────────────
static void AddUserBookmark(HWND hWnd)
{
    if (!g_pdfDoc) return;

    // 이미 같은 페이지 북마크가 있는지 확인
    for (auto& bm : g_bookmarks)
        if (bm.isUserAdded && bm.pageIndex == g_currentPage) return;

    // 제목: "페이지 N"
    wchar_t title[64];
    swprintf_s(title, L"\xD398\xC774\xC9C0 %d", g_currentPage + 1); // "페이지 N"

    BookmarkItem bm;
    bm.pageIndex   = g_currentPage;
    bm.title       = title;
    bm.level       = 0;
    bm.isUserAdded = true;
    g_bookmarks.push_back(bm);

    SaveUserBookmarks();
    InvalidateRect(hWnd, nullptr, FALSE);
}

// ── 사용자 북마크 삭제 ────────────────────────────────────────────────────────
static void DeleteUserBookmark(HWND hWnd, int idx)
{
    if (idx < 0 || idx >= (int)g_bookmarks.size()) return;
    if (!g_bookmarks[idx].isUserAdded) return;

    wchar_t msg[128];
    swprintf_s(msg, L"\"%s\" 북마크를 삭제하시겠습니까?",
               g_bookmarks[idx].title.c_str());
    if (!ShowConfirmDialog(hWnd, msg)) return;

    g_bookmarks.erase(g_bookmarks.begin() + idx);
    g_sidebarHoverIdx = -1;
    SaveUserBookmarks();
    InvalidateRect(hWnd, nullptr, FALSE);
}

static bool IsBookmarkVisible(int idx)
{
    if (idx < 0 || idx >= (int)g_bookmarks.size()) return false;
    int targetLevel = g_bookmarks[idx].level;
    if (targetLevel == 0) return true;

    // 부모 체인이 모두 열려 있어야 보임
    for (int i = idx - 1; i >= 0; --i)
    {
        if (g_bookmarks[i].level < targetLevel)
        {
            if (!g_bookmarks[i].isOpen) return false;
            targetLevel = g_bookmarks[i].level;
            if (targetLevel == 0) return true;
        }
    }
    return true;
}

static int SidebarHitTest(int x, int y, const RECT& clientRect)
{
    if (!g_sidebarVisible) return -1;
    if (x < 0 || x >= SIDEBAR_WIDTH) return -1;
    int contentTop = GetBelowToolbarY() + SIDEBAR_HEADER_H;
    int contentBottom = clientRect.bottom - STATUSBAR_HEIGHT;
    if (y < contentTop || y >= contentBottom) return -1;

    // "+ 현재 페이지 북마크" 버튼 (하단 28px)
    const int addBtnH = 28;
    if (y >= contentBottom - addBtnH - 4)
        return -2; // 추가 버튼

    int yy = contentTop - g_sidebarScrollY;
    for (int i = 0; i < (int)g_bookmarks.size(); ++i)
    {
        if (!IsBookmarkVisible(i)) continue;
        if (y >= yy && y < yy + SIDEBAR_ITEM_H)
            return i;
        yy += SIDEBAR_ITEM_H;
    }
    return -1;
}

static void GetThumbnailGeometry(int pageIdx, const RECT& clientRect, int scrollY,
    int* outX, int* outY, int* outW, int* outH)
{
    if (!g_pdfDoc || pageIdx < 0 || pageIdx >= g_pageCount) return;
    int availW = SIDEBAR_WIDTH - SIDEBAR_THUMB_PAD * 2;
    int contentTop = GetBelowToolbarY() + SIDEBAR_HEADER_H;
    int yy = contentTop - scrollY;

    for (int i = 0; i <= pageIdx; ++i)
    {
        double pw = g_pageLayouts[i].pdfW;
        double ph = g_pageLayouts[i].pdfH;
        if (pw <= 0) pw = 1;
        int tw = availW;
        int th = (int)(ph / pw * tw);
        if (i == pageIdx)
        {
            *outX = SIDEBAR_THUMB_PAD;
            *outY = yy;
            *outW = tw;
            *outH = th;
            return;
        }
        yy += th + 20 + SIDEBAR_THUMB_GAP;  // thumb + label(20) + gap
    }
}

static int GetThumbnailTotalHeight(const RECT& clientRect)
{
    if (!g_pdfDoc || g_pageLayouts.empty()) return 0;
    int availW = SIDEBAR_WIDTH - SIDEBAR_THUMB_PAD * 2;
    int total = 0;
    for (int i = 0; i < g_pageCount; ++i)
    {
        double pw = g_pageLayouts[i].pdfW;
        double ph = g_pageLayouts[i].pdfH;
        if (pw <= 0) pw = 1;
        int th = (int)(ph / pw * availW);
        total += th + 20 + SIDEBAR_THUMB_GAP;
    }
    return total;
}

static int ThumbnailHitTest(int x, int y, const RECT& clientRect)
{
    if (!g_sidebarVisible || g_sidebarTab != TAB_THUMBNAILS) return -1;
    if (x < 0 || x >= SIDEBAR_WIDTH) return -1;
    int contentTop = GetBelowToolbarY() + SIDEBAR_HEADER_H;
    int contentBottom = clientRect.bottom - STATUSBAR_HEIGHT;
    if (y < contentTop || y >= contentBottom) return -1;

    int availW = SIDEBAR_WIDTH - SIDEBAR_THUMB_PAD * 2;
    int yy = contentTop - g_thumbScrollY;
    for (int i = 0; i < g_pageCount; ++i)
    {
        double pw = g_pageLayouts[i].pdfW;
        double ph = g_pageLayouts[i].pdfH;
        if (pw <= 0) pw = 1;
        int th = (int)(ph / pw * availW);
        int itemH = th + 20 + SIDEBAR_THUMB_GAP;
        if (y >= yy && y < yy + itemH)
            return i;
        yy += itemH;
    }
    return -1;
}

static void DrawSidebarThumbnails(HDC hdc, const RECT& clientRect)
{
    // PDFium은 스레드 안전하지 않으므로 백그라운드 렌더 워커와 동시에 호출되지 않도록 직렬화
    std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);

    int sbTop = GetBelowToolbarY();
    int sbBottom = clientRect.bottom - STATUSBAR_HEIGHT;
    int sbRight = SIDEBAR_WIDTH;
    int contentTop = sbTop + SIDEBAR_HEADER_H;
    int availW = sbRight - SIDEBAR_THUMB_PAD * 2;

    if (!g_pdfDoc || g_pageCount <= 0 || g_pageLayouts.empty())
    {
        HFONT emptyFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT of = static_cast<HFONT>(SelectObject(hdc, emptyFont));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(100, 95, 110));
        RECT er = { 12, contentTop + 12, sbRight - 8, sbBottom };
        DrawTextW(hdc, L"\xBB38\xC11C\xB97C \xC5EC\xC138\xC694", -1, &er, DT_LEFT | DT_TOP | DT_WORDBREAK);  // 문서를 여세요
        SelectObject(hdc, of);
        DeleteObject(emptyFont);
        return;
    }

    // 콘텐츠 클리핑
    HRGN clipRgn = CreateRectRgn(0, contentTop, sbRight, sbBottom);
    SelectClipRgn(hdc, clipRgn);

    int yy = contentTop - g_thumbScrollY;
    for (int i = 0; i < g_pageCount; ++i)
    {
        if (i >= (int)g_pageLayouts.size()) break;
        double pw = g_pageLayouts[i].pdfW;
        double ph = g_pageLayouts[i].pdfH;
        if (pw <= 0) pw = 1;
        int tw = availW;
        int th = std::max(1, std::min((int)(ph / pw * tw), availW * 4)); // 최대 높이 제한
        int tx = SIDEBAR_THUMB_PAD;

        // 화면 밖이면 건너뜀
        if (yy + th + 20 + SIDEBAR_THUMB_GAP < contentTop) { yy += th + 20 + SIDEBAR_THUMB_GAP; continue; }
        if (yy > sbBottom) break;

        bool isCurrent = (i == g_currentPage);
        bool isHover = (i == g_thumbHoverIdx);

        // 현재 페이지 배경 하이라이트
        if (isCurrent)
        {
            RECT hlRect = { tx - 4, yy - 3, tx + tw + 4, yy + th + 20 + 3 };
            HBRUSH hlBr = CreateSolidBrush(RGB(45, 55, 80));
            FillRect(hdc, &hlRect, hlBr);
            DeleteObject(hlBr);
        }
        else if (isHover)
        {
            RECT hlRect = { tx - 4, yy - 3, tx + tw + 4, yy + th + 20 + 3 };
            HBRUSH hlBr = CreateSolidBrush(RGB(40, 38, 55));
            FillRect(hdc, &hlRect, hlBr);
            DeleteObject(hlBr);
        }

        // 썸네일 캐시 확인
        bool thumbValid = (i < (int)g_thumbCache.size()
            && g_thumbCache[i].hBmp
            && g_thumbCache[i].width  == tw
            && g_thumbCache[i].height == th);

        if (thumbValid)
        {
            // 캐시 히트 — BitBlt만 수행
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, g_thumbCache[i].hBmp));
            BitBlt(hdc, tx, yy, tw, th, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteDC(memDC);
        }
        else
        {
            // 캐시 미스 — 렌더링 후 캐시에 저장
            FPDF_PAGE page = (i == g_currentPage) ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, i);
            if (page)
            {
                FPDF_BITMAP fpdfBmp = FPDFBitmap_Create(tw, th, 0);
                if (fpdfBmp)
                {
                    FPDFBitmap_FillRect(fpdfBmp, 0, 0, tw, th, 0xFFFFFFFF);
                    FPDF_RenderPageBitmap(fpdfBmp, page, 0, 0, tw, th, 0, FPDF_LCD_TEXT);
                }

                if (fpdfBmp)
                {
                    BITMAPINFO bmi = {};
                    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bmi.bmiHeader.biWidth = tw;
                    bmi.bmiHeader.biHeight = -th;
                    bmi.bmiHeader.biPlanes = 1;
                    bmi.bmiHeader.biBitCount = 32;
                    bmi.bmiHeader.biCompression = BI_RGB;

                    void* dibBits = nullptr;
                    HDC memDC = CreateCompatibleDC(hdc);
                    HBITMAP hDib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
                    if (hDib && dibBits)
                    {
                        const BYTE* src = static_cast<const BYTE*>(FPDFBitmap_GetBuffer(fpdfBmp));
                        if (src)
                        {
                        const int stride = FPDFBitmap_GetStride(fpdfBmp);
                        BYTE* dst = static_cast<BYTE*>(dibBits);
                        for (int r = 0; r < th; ++r)
                            memcpy(dst + static_cast<ptrdiff_t>(r) * tw * 4,
                                src + static_cast<ptrdiff_t>(r) * stride,
                                static_cast<size_t>(tw) * 4);

                        HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, hDib));
                        BitBlt(hdc, tx, yy, tw, th, memDC, 0, 0, SRCCOPY);
                        SelectObject(memDC, oldBmp);

                        // 캐시에 저장
                        if (i < (int)g_thumbCache.size())
                        {
                            if (g_thumbCache[i].hBmp) DeleteObject(g_thumbCache[i].hBmp);
                            g_thumbCache[i].hBmp   = hDib;
                            g_thumbCache[i].width  = tw;
                            g_thumbCache[i].height = th;
                        }
                        else
                        {
                            DeleteObject(hDib);
                        }
                        }
                        else
                        {
                            DeleteObject(hDib);
                        }
                    }
                    DeleteDC(memDC);
                    FPDFBitmap_Destroy(fpdfBmp);
                }

            if (i != g_currentPage)
                FPDF_ClosePage(page);
            }
        }

        // 테두리
        COLORREF borderColor = isCurrent ? RGB(100, 140, 220) : RGB(60, 58, 75);
        HPEN bPen = CreatePen(PS_SOLID, isCurrent ? 2 : 1, borderColor);
        HPEN oldP = static_cast<HPEN>(SelectObject(hdc, bPen));
        HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
        Rectangle(hdc, tx, yy, tx + tw, yy + th);
        SelectObject(hdc, oldP);
        SelectObject(hdc, oldBr);
        DeleteObject(bPen);

        // 페이지 번호
        WCHAR pageLabel[32];
        swprintf_s(pageLabel, L"%d", i + 1);
        HFONT pgFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, pgFont));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, isCurrent ? RGB(255, 255, 255) : RGB(220, 220, 220));
        RECT labelRect = { tx, yy + th + 2, tx + tw, yy + th + 18 };
        DrawTextW(hdc, pageLabel, -1, &labelRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(pgFont);

        yy += th + 20 + SIDEBAR_THUMB_GAP;
    }

    SelectClipRgn(hdc, nullptr);
    DeleteObject(clipRgn);

    // ── 썸네일 스크롤바 ──────────────────────────────────────────────────────
    {
        int contentH = sbBottom - contentTop;
        int totalH   = GetThumbnailTotalHeight(clientRect);
        if (totalH > contentH)
        {
            int trackX = sbRight - THUMB_SB_W - 2;
            int trackY = contentTop;
            int trackH = contentH;

            // 트랙 배경
            HBRUSH trackBr = CreateSolidBrush(RGB(22, 20, 32));
            RECT trackRc = { trackX, trackY, trackX + THUMB_SB_W, trackY + trackH };
            FillRect(hdc, &trackRc, trackBr);
            DeleteObject(trackBr);

            // 썸 크기·위치
            int thumbH    = std::max(20, trackH * contentH / totalH);
            int maxScroll = totalH - contentH;
            int denom     = trackH - thumbH;
            int thumbY    = trackY + (denom > 0 && maxScroll > 0
                                ? (g_thumbScrollY * denom / maxScroll)
                                : 0);

            HBRUSH thumbBr = CreateSolidBrush(RGB(100, 96, 130));
            RECT thumbRc = { trackX, thumbY, trackX + THUMB_SB_W, thumbY + thumbH };
            // 둥근 직사각형
            HPEN nullPen = CreatePen(PS_NULL, 0, 0);
            HPEN oldPen  = static_cast<HPEN>(SelectObject(hdc, nullPen));
            HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, thumbBr));
            RoundRect(hdc, trackX, thumbY, trackX + THUMB_SB_W, thumbY + thumbH, 4, 4);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBr);
            DeleteObject(nullPen);
            DeleteObject(thumbBr);
        }
    }
}

static void DrawSidebar(HDC hdc, const RECT& clientRect)
{
    if (!g_sidebarVisible && !s_sbClosing) return;

    int sbTop = GetBelowToolbarY();
    int sbBottom = clientRect.bottom - STATUSBAR_HEIGHT;
    int sbRight = SIDEBAR_WIDTH;

    // 사이드바 배경
    RECT bgRect = { 0, sbTop, sbRight, sbBottom };
    if (g_sidebarTab == TAB_THUMBNAILS)
    {
        TRIVERTEX tv[2] = {};
        tv[0].x = bgRect.left;  tv[0].y = bgRect.top;
        tv[0].Red = 0x2800; tv[0].Green = 0x2600; tv[0].Blue = 0x3800;
        tv[1].x = bgRect.right; tv[1].y = bgRect.bottom;
        tv[1].Red = 0x1A00; tv[1].Green = 0x1900; tv[1].Blue = 0x2400;
        GRADIENT_RECT gr = { 0, 1 };
        GradientFill(hdc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    }
    else
    {
        HBRUSH bgBr = CreateSolidBrush(RGB(30, 28, 40));
        FillRect(hdc, &bgRect, bgBr);
        DeleteObject(bgBr);
    }

    // ─── 헤더 ───
    RECT headerRect = { 0, sbTop, sbRight, sbTop + SIDEBAR_HEADER_H };
    HBRUSH headerBr = CreateSolidBrush(RGB(38, 35, 52));
    FillRect(hdc, &headerRect, headerBr);
    DeleteObject(headerBr);

    HFONT hdrFont = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hdrFont));
    SetBkMode(hdc, TRANSPARENT);

    const bool isBm = (g_sidebarTab == TAB_BOOKMARKS);
    const wchar_t* headerLabel = isBm ? L"\xBAA9\xCC28" : L"\xD398\xC774\xC9C0";
    const wchar_t* iconGlyph   = isBm ? L"\xE8FD" : L"\xE7C3";
    RECT labelRect = { 0, sbTop, sbRight, sbTop + SIDEBAR_HEADER_H };
    SetTextColor(hdc, RGB(220, 218, 235));

    HFONT icoFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
    SIZE txtSz = {}, icoSz = {};
    GetTextExtentPoint32W(hdc, headerLabel, (int)wcslen(headerLabel), &txtSz);
    HFONT prevF = static_cast<HFONT>(SelectObject(hdc, icoFont));
    GetTextExtentPoint32W(hdc, iconGlyph, 1, &icoSz);
    const int gap = 6;
    int totalW = icoSz.cx + gap + txtSz.cx;
    int startX = (sbRight - totalW) / 2;
    RECT icoRc = { startX, sbTop, startX + icoSz.cx, sbTop + SIDEBAR_HEADER_H };
    DrawTextW(hdc, iconGlyph, 1, &icoRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, prevF);
    DeleteObject(icoFont);

    RECT txtRc = { startX + icoSz.cx + gap, sbTop, sbRight, sbTop + SIDEBAR_HEADER_H };
    DrawTextW(hdc, headerLabel, -1, &txtRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, oldFont);
    DeleteObject(hdrFont);

    // 헤더 하단 구분선
    HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(55, 50, 70));
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, sepPen));
    MoveToEx(hdc, 0, sbTop + SIDEBAR_HEADER_H - 1, nullptr);
    LineTo(hdc, sbRight, sbTop + SIDEBAR_HEADER_H - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(sepPen);

    // ─── 탭 콘텐츠 ───
    if (g_sidebarTab == TAB_THUMBNAILS)
    {
        DrawSidebarThumbnails(hdc, clientRect);
    }
    else
    {
        // ── "+ 현재 페이지 추가" 버튼 (보라 그라데이션) ──
        const int addBtnH = 28;
        RECT addBtnRect = { 6, sbBottom - addBtnH - 4, sbRight - 6, sbBottom - 4 };
        bool addHovered = (g_sidebarHoverIdx == -2);
        {
            // 둥근 모서리 클리핑
            HRGN btnRgn = CreateRoundRectRgn(addBtnRect.left, addBtnRect.top,
                                              addBtnRect.right, addBtnRect.bottom, 6, 6);
            SelectClipRgn(hdc, btnRgn);

            // 그라데이션 채우기 (위: 밝은 보라 → 아래: 진한 보라)
            TRIVERTEX tv[2] = {};
            tv[0].x     = addBtnRect.left;  tv[0].y = addBtnRect.top;
            tv[1].x     = addBtnRect.right; tv[1].y = addBtnRect.bottom;
            if (addHovered) {
                tv[0].Red = 0xB000; tv[0].Green = 0x7000; tv[0].Blue = 0xFF00; tv[0].Alpha = 0;
                tv[1].Red = 0x7000; tv[1].Green = 0x3000; tv[1].Blue = 0xCC00; tv[1].Alpha = 0;
            } else {
                tv[0].Red = 0x9000; tv[0].Green = 0x5000; tv[0].Blue = 0xE000; tv[0].Alpha = 0;
                tv[1].Red = 0x5800; tv[1].Green = 0x2000; tv[1].Blue = 0xAA00; tv[1].Alpha = 0;
            }
            GRADIENT_RECT gr = { 0, 1 };
            GradientFill(hdc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);

            SelectClipRgn(hdc, nullptr);
            DeleteObject(btnRgn);

            // 테두리
            HPEN btnPn = CreatePen(PS_SOLID, 1, addHovered ? RGB(190, 130, 255) : RGB(150, 90, 220));
            HPEN oPn   = static_cast<HPEN>(SelectObject(hdc, btnPn));
            HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
            RoundRect(hdc, addBtnRect.left, addBtnRect.top, addBtnRect.right, addBtnRect.bottom, 6, 6);
            SelectObject(hdc, oPn); SelectObject(hdc, oBr);
            DeleteObject(btnPn);

            HFONT btnFont = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT oBtnFont = static_cast<HFONT>(SelectObject(hdc, btnFont));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(240, 230, 255));
            DrawTextW(hdc, L"+ \xD604\xC7AC \xD398\xC774\xC9C0 \xBD81\xB9C8\xD06C", -1,
                      &addBtnRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oBtnFont);
            DeleteObject(btnFont);
        }

        int listBottom = sbBottom - addBtnH - 10; // 버튼 위까지만 목록 표시

        // 북마크 목록
        if (g_bookmarks.empty())
        {
            HFONT emptyFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            oldFont = static_cast<HFONT>(SelectObject(hdc, emptyFont));
            SetTextColor(hdc, RGB(100, 95, 110));
            RECT emptyRect = { 12, sbTop + SIDEBAR_HEADER_H + 12, sbRight - 8, listBottom };
            DrawTextW(hdc, L"\xBD81\xB9C8\xD06C\xAC00 \xC5C6\xC2B5\xB2C8\xB2E4", -1, &emptyRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
            SelectObject(hdc, oldFont);
            DeleteObject(emptyFont);
        }
        else
        {
            int contentTop = sbTop + SIDEBAR_HEADER_H;
            HRGN clipRgn = CreateRectRgn(0, contentTop, sbRight, listBottom);
            SelectClipRgn(hdc, clipRgn);

            HFONT itemFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            oldFont = static_cast<HFONT>(SelectObject(hdc, itemFont));

            int yy = contentTop - g_sidebarScrollY;
            for (int i = 0; i < (int)g_bookmarks.size(); ++i)
            {
                if (!IsBookmarkVisible(i)) continue;

                const auto& bm = g_bookmarks[i];
                int itemLeft = 8 + bm.level * SIDEBAR_INDENT;
                RECT itemRect = { 0, yy, sbRight, yy + SIDEBAR_ITEM_H };

                if (yy + SIDEBAR_ITEM_H < contentTop) { yy += SIDEBAR_ITEM_H; continue; }
                if (yy > listBottom) break;

                if (i == g_sidebarHoverIdx)
                {
                    HBRUSH hoverBr = CreateSolidBrush(RGB(50, 48, 65));
                    FillRect(hdc, &itemRect, hoverBr);
                    DeleteObject(hoverBr);
                }

                if (bm.pageIndex == g_currentPage)
                {
                    HBRUSH activeBr = CreateSolidBrush(RGB(45, 55, 80));
                    FillRect(hdc, &itemRect, activeBr);
                    DeleteObject(activeBr);

                    RECT accentBar = { 0, yy + 2, 3, yy + SIDEBAR_ITEM_H - 2 };
                    HBRUSH accentBr = CreateSolidBrush(RGB(100, 140, 220));
                    FillRect(hdc, &accentBar, accentBr);
                    DeleteObject(accentBr);
                }

                if (bm.hasChildren)
                {
                    SetTextColor(hdc, RGB(130, 125, 145));
                    RECT arrowRect = { itemLeft - 14, yy, itemLeft, yy + SIDEBAR_ITEM_H };
                    DrawTextW(hdc, bm.isOpen ? L"\x25BC" : L"\x25B6", -1, &arrowRect,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }

                bool isActive = (bm.pageIndex == g_currentPage);
                SetTextColor(hdc, isActive ? RGB(200, 210, 240) :
                    (i == g_sidebarHoverIdx ? RGB(220, 218, 228) : RGB(170, 168, 180)));

                // 사용자 북마크: 오른쪽에 × 삭제 버튼, 앞에 ★ 표시
                int textRight = sbRight - 8;
                if (bm.isUserAdded)
                {
                    // 아이콘
                    SetTextColor(hdc, RGB(255, 210, 80));
                    RECT starRect = { itemLeft, yy, itemLeft + 16, yy + SIDEBAR_ITEM_H };
                    DrawTextW(hdc, L"\x2605", -1, &starRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    itemLeft += 16;
                    textRight = sbRight - 22;

                    // 삭제 버튼 (hover 시에만 표시)
                    if (i == g_sidebarHoverIdx)
                    {
                        RECT delRect = { sbRight - 22, yy + (SIDEBAR_ITEM_H - 15) / 2,
                                         sbRight - 22 + 15, yy + (SIDEBAR_ITEM_H - 15) / 2 + 15 };
                        DrawPngIcon(hdc, IDB_TRASH_PNG, delRect, 15, true);
                    }
                    SetTextColor(hdc, isActive ? RGB(200, 210, 240) :
                        (i == g_sidebarHoverIdx ? RGB(220, 218, 228) : RGB(170, 168, 180)));
                }

                RECT textRect = { itemLeft, yy, textRight, yy + SIDEBAR_ITEM_H };
                DrawTextW(hdc, bm.title.c_str(), -1, &textRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                yy += SIDEBAR_ITEM_H;
            }

            SelectObject(hdc, oldFont);
            DeleteObject(itemFont);
            SelectClipRgn(hdc, nullptr);
            DeleteObject(clipRgn);
        }
    }

    // 오른쪽 구분선
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(55, 50, 70));
    oldPen = static_cast<HPEN>(SelectObject(hdc, borderPen));
    MoveToEx(hdc, sbRight - 1, sbTop, nullptr);
    LineTo(hdc, sbRight - 1, sbBottom);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);
}

// ─── 렌더링 ───────────────────────────────────────────────────────────────────

static void DrawSpotlightOverlay(HDC hdc, const RECT& clientRect)
{
    if (!(g_spotlightMode && (g_hasSpotlight || g_isDrawingSpotlight))) return;

    RECT cr_sp = clientRect;
    cr_sp.top    = TOOLBAR_HEIGHT;
    cr_sp.bottom = clientRect.bottom - STATUSBAR_HEIGHT;

    Gdiplus::Graphics gfx(hdc);
    RECT& sr = g_spotlightRect;

    // 밝은 사각형을 제외한 4개 영역을 어둡게
    auto fillDim = [&](int x1, int y1, int x2, int y2)
    {
        if (x2 <= x1 || y2 <= y1) return;
        Gdiplus::SolidBrush dimBrush(Gdiplus::Color(160, 0, 0, 0));
        gfx.FillRectangle(&dimBrush, x1, y1, x2 - x1, y2 - y1);
    };

    LONG left   = std::max(sr.left,   (LONG)cr_sp.left);
    LONG top    = std::max(sr.top,    (LONG)cr_sp.top);
    LONG right  = std::min(sr.right,  (LONG)cr_sp.right);
    LONG bottom = std::min(sr.bottom, (LONG)cr_sp.bottom);

    // 위 / 아래 / 왼쪽 / 오른쪽
    fillDim(cr_sp.left, cr_sp.top,  cr_sp.right, top);
    fillDim(cr_sp.left, bottom,     cr_sp.right, cr_sp.bottom);
    fillDim(cr_sp.left, top,        left,        bottom);
    fillDim(right,      top,        cr_sp.right, bottom);

    // 밝은 영역 테두리
    if (right > left && bottom > top)
    {
        Gdiplus::Pen borderPen(Gdiplus::Color(220, 255, 255, 255), 2.0f);
        gfx.DrawRectangle(&borderPen, (INT)left, (INT)top, (INT)(right - left), (INT)(bottom - top));
    }
}

static void RenderPage(HWND hWnd, HDC hdc, const RECT& clientRect)
{
    const RECT viewport = GetViewportRect(clientRect);
    const int  vpW = viewport.right - viewport.left;
    const int  vpH = viewport.bottom - viewport.top;

    // ─── 텍스트 선택 드래그 중 고속 경로 ───────────────────────────────────────
    // 페이지+오버레이 베이스 캐시가 유효하면 BitBlt 1회 + 선택 오버레이만 추가
    if (g_isSelecting && !g_pageBaseDirty
        && g_pageBaseDC
        && vpW == g_pageBaseW && vpH == g_pageBaseH)
    {
        // 베이스(페이지+고정오버레이)를 백버퍼 전체에 복사
        BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom,
               g_pageBaseDC, 0, 0, SRCCOPY);
        // 변경된 선택 오버레이만 다시 그림
        DrawTextSelection(hdc, clientRect);
        return;
    }

    if (g_isDrawingSpotlight && !g_pageBaseDirty
        && g_pageBaseDC
        && vpW == g_pageBaseW && vpH == g_pageBaseH)
    {
        BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom,
               g_pageBaseDC, 0, 0, SRCCOPY);
        DrawSpotlightOverlay(hdc, clientRect);
        if (!g_fullscreen)
        {
            DrawToolbar(hdc, clientRect);
            DrawStatusBar(hdc, clientRect);
        }
        return;
    }

    // ─── 슬라이드쇼 전용: 현재 페이지만 뷰포트 전체에 렌더링 ───
    if (g_slideshowActive && g_pdfDoc && g_pageCount > 0 && g_pdfPage)
    {
        RECT bg = clientRect;
        FillRect(hdc, &bg, (HBRUSH)GetStockObject(BLACK_BRUSH));

        const auto& layout = g_pageLayouts[g_currentPage];
        // 뷰포트에 맞게 비율 유지하며 최대 크기 계산
        float scaleW = (float)vpW / (float)layout.pdfW;
        float scaleH = (float)vpH / (float)layout.pdfH;
        float scale  = std::min(scaleW, scaleH);
        int renderW = (int)(layout.pdfW * scale);
        int renderH = (int)(layout.pdfH * scale);
        int drawX = viewport.left + (vpW - renderW) / 2;
        int drawY = viewport.top  + (vpH - renderH) / 2;

        FPDF_BITMAP fpdfBmp = FPDFBitmap_Create(renderW, renderH, 0);
        if (fpdfBmp)
        {
            FPDFBitmap_FillRect(fpdfBmp, 0, 0, renderW, renderH, 0xFFFFFFFF);
            FPDF_RenderPageBitmap(fpdfBmp, g_pdfPage, 0, 0, renderW, renderH, g_rotation,
                FPDF_ANNOT | FPDF_LCD_TEXT);
        }
        if (fpdfBmp)
        {
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth       = renderW;
            bmi.bmiHeader.biHeight      = -renderH;
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* dibBits = nullptr;
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP hDib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
            if (hDib && dibBits)
            {
                const BYTE* src    = static_cast<const BYTE*>(FPDFBitmap_GetBuffer(fpdfBmp));
                const int   stride = FPDFBitmap_GetStride(fpdfBmp);
                BYTE* dst = static_cast<BYTE*>(dibBits);
                for (int y = 0; y < renderH; ++y)
                    memcpy(dst + (ptrdiff_t)y * renderW * 4,
                           src + (ptrdiff_t)y * stride,
                           (size_t)renderW * 4);

                if (g_darkMode)
                {
                    BYTE* px = static_cast<BYTE*>(dibBits);
                    for (int p = 0; p < renderW * renderH; ++p, px += 4)
                    { px[0]=255-px[0]; px[1]=255-px[1]; px[2]=255-px[2]; }
                }

                HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, hDib));
                BitBlt(hdc, drawX, drawY, renderW, renderH, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, oldBmp);
                DeleteObject(hDib);
            }
            DeleteDC(memDC);
            FPDFBitmap_Destroy(fpdfBmp);
        }

        // ── 우상단 반투명 페이지 표시 ──
        {
            Gdiplus::Graphics gfx(hdc);
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            WCHAR buf[32];
            swprintf_s(buf, L"%d / %d", g_currentPage + 1, g_pageCount);

            Gdiplus::FontFamily ff(L"Segoe UI");
            Gdiplus::Font font(&ff, 16.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::RectF bounds;
            Gdiplus::PointF origin(0, 0);
            gfx.MeasureString(buf, -1, &font, origin, &bounds);

            const int pad  = 10;
            const int rx   = clientRect.left + 6;
            const int ry   = 6;
            const int rw   = (int)bounds.Width  + pad * 2;
            const int rh   = (int)bounds.Height + pad;

            // 반투명 배경 (검은색 60%)
            Gdiplus::SolidBrush bgBrush(Gdiplus::Color(150, 0, 0, 0));
            Gdiplus::GraphicsPath path;
            int r = 8;
            path.AddArc(rx, ry, r*2, r*2, 180, 90);
            path.AddArc(rx+rw-r*2, ry, r*2, r*2, 270, 90);
            path.AddArc(rx+rw-r*2, ry+rh-r*2, r*2, r*2, 0, 90);
            path.AddArc(rx, ry+rh-r*2, r*2, r*2, 90, 90);
            path.CloseFigure();
            gfx.FillPath(&bgBrush, &path);

            // 흰색 텍스트
            Gdiplus::SolidBrush textBrush(Gdiplus::Color(220, 255, 255, 255));
            Gdiplus::RectF textRect(
                (Gdiplus::REAL)(rx + pad),
                (Gdiplus::REAL)(ry + pad / 2),
                bounds.Width, bounds.Height);
            gfx.DrawString(buf, -1, &font, textRect, nullptr, &textBrush);
        }

        // ── 좌우 반투명 화살표 버튼 ──
        {
            Gdiplus::Graphics gfx(hdc);
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            // 이전 페이지 화살표 (왼쪽) — 첫 페이지면 숨김
            if (g_currentPage > 0)
            {
                RECT lr = GetSSArrowRect(clientRect, true);
                BYTE alpha = g_ssArrowHoverLeft ? 160 : 80;

                Gdiplus::SolidBrush bg(Gdiplus::Color(alpha, 0, 0, 0));
                Gdiplus::GraphicsPath rp;
                int r2 = 10;
                rp.AddArc(lr.left, lr.top, r2*2, r2*2, 180, 90);
                rp.AddArc(lr.right-r2*2, lr.top, r2*2, r2*2, 270, 90);
                rp.AddArc(lr.right-r2*2, lr.bottom-r2*2, r2*2, r2*2, 0, 90);
                rp.AddArc(lr.left, lr.bottom-r2*2, r2*2, r2*2, 90, 90);
                rp.CloseFigure();
                gfx.FillPath(&bg, &rp);

                // ◀ 삼각형
                int cx = (lr.left + lr.right) / 2;
                int cy2 = (lr.top + lr.bottom) / 2;
                int as = 14;
                Gdiplus::PointF tri[3] = {
                    { (Gdiplus::REAL)(cx - as/2), (Gdiplus::REAL)cy2 },
                    { (Gdiplus::REAL)(cx + as/2), (Gdiplus::REAL)(cy2 - as) },
                    { (Gdiplus::REAL)(cx + as/2), (Gdiplus::REAL)(cy2 + as) }
                };
                Gdiplus::SolidBrush arrowBrush(Gdiplus::Color(230, 255, 255, 255));
                gfx.FillPolygon(&arrowBrush, tri, 3);
            }

            // 다음 페이지 화살표 (오른쪽) — 마지막 페이지면 숨김
            if (g_currentPage < g_pageCount - 1)
            {
                RECT rr = GetSSArrowRect(clientRect, false);
                BYTE alpha = g_ssArrowHoverRight ? 160 : 80;

                Gdiplus::SolidBrush bg(Gdiplus::Color(alpha, 0, 0, 0));
                Gdiplus::GraphicsPath rp;
                int r2 = 10;
                rp.AddArc(rr.left, rr.top, r2*2, r2*2, 180, 90);
                rp.AddArc(rr.right-r2*2, rr.top, r2*2, r2*2, 270, 90);
                rp.AddArc(rr.right-r2*2, rr.bottom-r2*2, r2*2, r2*2, 0, 90);
                rp.AddArc(rr.left, rr.bottom-r2*2, r2*2, r2*2, 90, 90);
                rp.CloseFigure();
                gfx.FillPath(&bg, &rp);

                // ▶ 삼각형
                int cx = (rr.left + rr.right) / 2;
                int cy2 = (rr.top + rr.bottom) / 2;
                int as = 14;
                Gdiplus::PointF tri[3] = {
                    { (Gdiplus::REAL)(cx + as/2), (Gdiplus::REAL)cy2 },
                    { (Gdiplus::REAL)(cx - as/2), (Gdiplus::REAL)(cy2 - as) },
                    { (Gdiplus::REAL)(cx - as/2), (Gdiplus::REAL)(cy2 + as) }
                };
                Gdiplus::SolidBrush arrowBrush(Gdiplus::Color(230, 255, 255, 255));
                gfx.FillPolygon(&arrowBrush, tri, 3);
            }

            // ── 오른쪽 위 닫기(X) 버튼 ──
            {
                RECT xr = GetSSCloseRect(clientRect);
                BYTE alpha = g_ssCloseHover ? 200 : 90;
                int r2 = 8;

                // 원형 배경
                Gdiplus::SolidBrush bg(Gdiplus::Color(alpha, 0, 0, 0));
                Gdiplus::GraphicsPath rp;
                rp.AddEllipse((Gdiplus::REAL)xr.left, (Gdiplus::REAL)xr.top,
                              (Gdiplus::REAL)(xr.right - xr.left),
                              (Gdiplus::REAL)(xr.bottom - xr.top));
                gfx.FillPath(&bg, &rp);

                // X 선
                int cx = (xr.left + xr.right) / 2;
                int cy2 = (xr.top + xr.bottom) / 2;
                int hs = 9;
                BYTE xAlpha = g_ssCloseHover ? 255 : 200;
                Gdiplus::Pen xPen(Gdiplus::Color(xAlpha, 255, 255, 255), 2.5f);
                xPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
                gfx.DrawLine(&xPen, cx - hs, cy2 - hs, cx + hs, cy2 + hs);
                gfx.DrawLine(&xPen, cx + hs, cy2 - hs, cx - hs, cy2 + hs);
            }
        }

        return;
    }

    DrawGradientBackground(hdc, viewport);

    if (!g_pdfDoc || g_pageCount <= 0 || g_pageLayouts.empty())
    {
        // 툴바 & 상태바
        if (!g_fullscreen)
        {
            DrawToolbar(hdc, clientRect);
            DrawTabBar(hdc, clientRect);
            DrawStatusBar(hdc, clientRect);
        }

        // 안내 문구 (커스텀 폰트)
        HFONT hFont = CreateFontW(48, 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFont));

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(100, 100, 108));

        // file.png 아이콘 (중앙 상단)
        {
            HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDB_FILE_PNG), RT_RCDATA);
            if (hRes) {
                DWORD sz = SizeofResource(hInst, hRes);
                HGLOBAL hMem = LoadResource(hInst, hRes);
                if (hMem) {
                void* pData = LockResource(hMem);
                if (pData && sz > 0) {
                    HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, sz);
                    if (hBuf) {
                    void* pBuf = GlobalLock(hBuf);
                    if (pBuf) {
                        memcpy(pBuf, pData, sz);
                        GlobalUnlock(hBuf);
                        IStream* pStream = nullptr;
                        if (SUCCEEDED(CreateStreamOnHGlobal(hBuf, TRUE, &pStream)) && pStream) {
                            Gdiplus::Image img(pStream);
                            constexpr int MAX_SIZE = 96;
                            UINT origW = img.GetWidth();
                            UINT origH = img.GetHeight();
                            int drawW, drawH;
                            if (origW == 0 || origH == 0) {
                                drawW = drawH = MAX_SIZE;
                            } else if (origW >= origH) {
                                drawW = MAX_SIZE;
                                drawH = (int)(MAX_SIZE * (float)origH / origW);
                            } else {
                                drawH = MAX_SIZE;
                                drawW = (int)(MAX_SIZE * (float)origW / origH);
                            }
                            int ix = (viewport.left + viewport.right - drawW) / 2;
                            int iy = (viewport.top + viewport.bottom) / 2 - drawH - 10;
                            Gdiplus::Graphics gfx(hdc);
                            gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                            gfx.DrawImage(&img, ix, iy, drawW, drawH);
                            pStream->Release();
                        } else {
                            GlobalFree(hBuf);
                        }
                    } // pBuf
                    else { GlobalFree(hBuf); }
                    } // hBuf
                }
                } // hMem
            }
        }

        RECT textArea = viewport;
        textArea.top += 28;
        SetTextColor(hdc, RGB(140, 140, 148));
        HFONT hFont2 = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(hdc, hFont2);
        DrawTextW(hdc, LS(STR_NO_DOC), -1,
            &textArea, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
        DeleteObject(hFont2);

        // 문서 없어도 그룹 패널은 그린다
        GroupPanel::Draw(hdc, clientRect);
        return;
    }

    // 뷰포트 클리핑
    HRGN clipRgn = CreateRectRgn(viewport.left, viewport.top, viewport.right, viewport.bottom);
    SelectClipRgn(hdc, clipRgn);

    // 루프 전체에서 재사용할 메모리 DC (한 번만 생성)
    HDC loopDC = CreateCompatibleDC(hdc);

    // 보이는 페이지들을 렌더링
    for (int i = 0; i < g_pageCount; i++)
    {
        const auto& layout = g_pageLayouts[i];
        int pageLeft, pageTop;
        GetPageScreenPos(i, viewport, &pageLeft, &pageTop);

        // 화면 밖이면 건너뜀
        if (pageTop + layout.renderH < viewport.top) continue;
        if (pageTop > viewport.bottom) break;

        const int renderW = layout.renderW;
        const int renderH = layout.renderH;

        // 다단계 소프트 그림자
        DrawSoftShadow(hdc, pageLeft, pageTop, renderW, renderH);

        // 캐시된 비트맵이 있으면 사용, 없으면 렌더링 후 캐시
        // 폼이 있는 현재 페이지는 캐시를 사용하지 않고 매번 새로 렌더링 (폼 상태 반영)
        const bool isFormPage = (g_formHandle && i == g_currentPage && g_pdfPage);
        bool cacheValid = !isFormPage
            && (i < (int)g_pageCache.size()
            && g_pageCache[i].hBmp
            && g_pageCache[i].width == renderW
            && g_pageCache[i].height == renderH
            && g_pageCache[i].zoom == g_zoom);

        if (cacheValid)
        {
            HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(loopDC, g_pageCache[i].hBmp));
            BitBlt(hdc, pageLeft, pageTop, renderW, renderH, loopDC, 0, 0,
                g_darkMode ? NOTSRCCOPY : SRCCOPY);
            SelectObject(loopDC, oldBmp);
        }
        else if (isFormPage)
        {
            // 폼이 있는 현재 페이지 동기 렌더 (g_pdfPage는 UI 스레드 소유)
            // PDFium은 스레드 안전하지 않으므로 백그라운드 렌더 워커와 동시에 호출되지 않도록 직렬화
            std::lock_guard<std::mutex> pdfLk(g_pdfiumMtx);
            FPDF_BITMAP fpdfBmp = FPDFBitmap_Create(renderW, renderH, 0);
            if (fpdfBmp)
            {
                FPDFBitmap_FillRect(fpdfBmp, 0, 0, renderW, renderH, 0xFFFFFFFF);
                FPDF_RenderPageBitmap(fpdfBmp, g_pdfPage, 0, 0, renderW, renderH, GetEffectiveRotation(i),
                    FPDF_ANNOT | FPDF_LCD_TEXT);
                FPDF_FFLDraw(g_formHandle, fpdfBmp, g_pdfPage,
                    0, 0, renderW, renderH, GetEffectiveRotation(i), FPDF_ANNOT | FPDF_LCD_TEXT);
            }
            if (fpdfBmp)
            {
                BITMAPINFO bmi = {};
                bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth       = renderW;
                bmi.bmiHeader.biHeight      = -renderH;
                bmi.bmiHeader.biPlanes      = 1;
                bmi.bmiHeader.biBitCount    = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                void* dibBits = nullptr;
                HBITMAP hDib = CreateDIBSection(loopDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
                if (hDib && dibBits)
                {
                    const BYTE* src    = static_cast<const BYTE*>(FPDFBitmap_GetBuffer(fpdfBmp));
                    const int   stride = FPDFBitmap_GetStride(fpdfBmp);
                    BYTE*       dst    = static_cast<BYTE*>(dibBits);
                    for (int y = 0; y < renderH; ++y)
                        memcpy(dst + static_cast<ptrdiff_t>(y) * renderW * 4,
                               src + static_cast<ptrdiff_t>(y) * stride,
                               static_cast<size_t>(renderW) * 4);
                    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(loopDC, hDib));
                    BitBlt(hdc, pageLeft, pageTop, renderW, renderH, loopDC, 0, 0,
                        g_darkMode ? NOTSRCCOPY : SRCCOPY);
                    SelectObject(loopDC, oldBmp);
                    // 폼 페이지가 g_currentPage에서 벗어나도(다른 페이지 클릭 등) 계속
                    // 선명하게 보이도록, 방금 그린 결과를 페이지 캐시에도 반영해 둔다.
                    // (isFormPage 분기는 원래 캐시에 저장하지 않아 포커스를 잃으면
                    //  즉시 저해상도 프리뷰로 되돌아가 흐려 보이는 문제가 있었음)
                    if (i < (int)g_pageCache.size())
                    {
                        if (g_pageCache[i].hBmp) DeleteObject(g_pageCache[i].hBmp);
                        g_pageCache[i].hBmp   = hDib;
                        g_pageCache[i].width  = renderW;
                        g_pageCache[i].height = renderH;
                        g_pageCache[i].zoom   = g_zoom;
                        g_pageCache[i].dark   = g_darkMode;
                    }
                    else
                    {
                        DeleteObject(hDib);
                    }
                }
                FPDFBitmap_Destroy(fpdfBmp);
            }
        }
        else
        {
            // 캐시 미스: 프리뷰조차 없으면 즉시 저해상도로 채워 백지 노출을 방지
            if (!(i < (int)g_previewCache.size() && g_previewCache[i].hBmp))
                EnsureQuickPreview(i, renderW, renderH);

            // 캐시 미스: 프리뷰가 있으면 늘려서 깔고, 없으면 흰색 placeholder
            bool drewPreview = false;
            if (i < (int)g_previewCache.size() && g_previewCache[i].hBmp)
            {
                HBITMAP oldBmp = (HBITMAP)SelectObject(loopDC, g_previewCache[i].hBmp);
                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, nullptr);
                StretchBlt(hdc, pageLeft, pageTop, renderW, renderH,
                    loopDC, 0, 0, g_previewCache[i].w, g_previewCache[i].h,
                    g_darkMode ? NOTSRCCOPY : SRCCOPY);
                SelectObject(loopDC, oldBmp);
                drewPreview = true;
            }
            if (!drewPreview)
            {
                RECT pageRect = { pageLeft, pageTop, pageLeft + renderW, pageTop + renderH };
                HBRUSH placeholderBr = CreateSolidBrush(g_darkMode ? RGB(30, 30, 30) : RGB(255, 255, 255));
                FillRect(hdc, &pageRect, placeholderBr);
                DeleteObject(placeholderBr);
            }
            if (!g_thumbTracking || i == g_currentPage)
                PostPrefetch(hWnd, i);
        }

        // 페이지 테두리
        COLORREF borderClr = g_darkMode ? RGB(60, 60, 68) : RGB(180, 180, 184);
        HPEN borderPen = CreatePen(PS_SOLID, 1, borderClr);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, borderPen));
        HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
        {
            int bl = pageLeft, br = pageLeft + renderW;
            bool edgeL = (bl <= clientRect.left + 1);
            bool edgeR = (br >= clientRect.right - 1);
            if (!edgeL && !edgeR)
                Rectangle(hdc, bl, pageTop, br, pageTop + renderH);
            else
            {
                MoveToEx(hdc, bl, pageTop, nullptr);
                LineTo(hdc, br - 1, pageTop);
                MoveToEx(hdc, bl, pageTop + renderH - 1, nullptr);
                LineTo(hdc, br - 1, pageTop + renderH - 1);
                if (!edgeL) { MoveToEx(hdc, bl, pageTop, nullptr); LineTo(hdc, bl, pageTop + renderH); }
                if (!edgeR) { MoveToEx(hdc, br - 1, pageTop, nullptr); LineTo(hdc, br - 1, pageTop + renderH); }
            }
        }
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBr);
        DeleteObject(borderPen);
    }

    DeleteDC(loopDC);

    // 하이라이트 오버레이
    DrawHighlights(hdc, clientRect);

    // 밑줄 오버레이
    DrawUnderlines(hdc, clientRect);

    // 취소선 오버레이
    DrawStrikeouts(hdc, clientRect);

    // 물결 밑줄 오버레이
    DrawSquigglies(hdc, clientRect);

    // 메모 아이콘 오버레이
    DrawNotes(hdc, clientRect);

    // 텍스트 박스 오버레이
    DrawTextBoxes(hdc, clientRect);

    // Redact 오버레이
    DrawRedacts(hdc, clientRect);

    // 도형 주석 오버레이
    DrawShapes(hdc, clientRect);

    // 서명 오버레이
    DrawSignatures(hdc, clientRect);

    // ─── 페이지 베이스 캐시 갱신 ───────────────────────────────────────────────
    // 여기까지의 결과(페이지+고정오버레이)를 g_pageBaseDC에 저장해 두면
    // 텍스트 선택 드래그 중에는 이 캐시를 BitBlt하고 선택 오버레이만 추가한다
    if (vpW > 0 && vpH > 0)
    {
        const int fullW = clientRect.right;
        const int fullH = clientRect.bottom;
        if (!g_pageBaseDC || fullW != g_pageBaseW || fullH != g_pageBaseH)
        {
            if (g_pageBaseDC)
            {
                DeleteDC(g_pageBaseDC);
                DeleteObject(g_pageBaseBmp);
            }
            g_pageBaseDC  = CreateCompatibleDC(hdc);
            g_pageBaseBmp = CreateCompatibleBitmap(hdc, fullW, fullH);
            g_pageBaseOld = static_cast<HBITMAP>(SelectObject(g_pageBaseDC, g_pageBaseBmp));
            g_pageBaseW   = fullW;
            g_pageBaseH   = fullH;
        }
        BitBlt(g_pageBaseDC, 0, 0, fullW, fullH, hdc, 0, 0, SRCCOPY);
        g_pageBaseDirty = false;
    }

    // 텍스트 선택 하이라이트 (활성 페이지)
    DrawTextSelection(hdc, clientRect);

    // 찾기 결과 하이라이트 (활성 페이지)
    DrawFindHighlight(hdc, clientRect);

    // 클리핑 해제
    SelectClipRgn(hdc, nullptr);
    DeleteObject(clipRgn);

    // 스포트라이트 오버레이 (툴바/상태바 제외 영역에 반투명 어둠 + 밝은 사각형)
    DrawSpotlightOverlay(hdc, clientRect);

    // 툴바 & 상태바를 마지막에 그려서 항상 최상위 유지
    if (!g_fullscreen)
    {
        DrawToolbar(hdc, clientRect);
        DrawTabBar(hdc, clientRect);
        DrawFindBar(hdc, clientRect);
        DrawStatusBar(hdc, clientRect);
    }

    // 사이드바 (최상위에 그림)
    if (s_sbAnimating && (g_sidebarVisible || s_sbClosing))
    {
        int sv = SaveDC(hdc);
        IntersectClipRect(hdc, 0, GetBelowToolbarY(), SIDEBAR_WIDTH + s_sbOffset, clientRect.bottom - STATUSBAR_HEIGHT);
        SetViewportOrgEx(hdc, s_sbOffset, 0, nullptr);
        DrawSidebar(hdc, clientRect);
        RestoreDC(hdc, sv);
    }
    else
        DrawSidebar(hdc, clientRect);

    // 그룹 패널 (최상위에 그림)
    GroupPanel::Draw(hdc, clientRect);
}

// ─── UI 헬퍼 ──────────────────────────────────────────────────────────────────

void UpdateTitle(HWND hWnd)
{
    if (!g_pdfDoc) { SetWindowTextW(hWnd, szTitle); return; }

    const size_t pos = g_filePath.find_last_of(L"\\/");
    const std::wstring fn = (pos != std::wstring::npos) ? g_filePath.substr(pos + 1) : g_filePath;

    WCHAR title[512];
    swprintf_s(title, L"%s \u2014 %s",
        fn.c_str(), szTitle);
    SetWindowTextW(hWnd, title);
}

// 레이저 포인터를 지정한 DC에 직접 그림 (백버퍼와 분리된 화면 DC용)
static void DrawLaserOverlay(HDC hdc, int cx, int cy)
{
    // 오프스크린 32비트 비트맵에 레이저를 그린 뒤 AlphaBlend로 한 번에 합성
    // → WM_PAINT 더블버퍼와 충돌 없이 깜박임/번짐 없는 렌더링
    const int SZ = 28; // 오버레이 크기 (짝수)
    const int HALF = SZ / 2;

    // 32bpp DIB 생성
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = SZ;
    bmi.bmiHeader.biHeight      = -SZ; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hBmp) return;

    HDC offDC = CreateCompatibleDC(hdc);
    HBITMAP hOld = (HBITMAP)SelectObject(offDC, hBmp);

    // 배경을 완전 투명(0)으로 초기화
    memset(pBits, 0, SZ * SZ * 4);

    {
        Gdiplus::Bitmap bmpObj(SZ, SZ, SZ * 4, PixelFormat32bppARGB, (BYTE*)pBits);
        Gdiplus::Graphics gfx(&bmpObj);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        gfx.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

        // 외곽 후광 (부드러운 붉은 원)
        Gdiplus::SolidBrush glowBrush(Gdiplus::Color(70, 255, 0, 0));
        gfx.FillEllipse(&glowBrush, 2, 2, SZ - 4, SZ - 4);

        // 중간 글로우
        Gdiplus::SolidBrush midBrush(Gdiplus::Color(130, 255, 20, 20));
        gfx.FillEllipse(&midBrush, HALF - 6, HALF - 6, 12, 12);

        // 중심 점
        Gdiplus::SolidBrush dotBrush(Gdiplus::Color(240, 255, 40, 40));
        gfx.FillEllipse(&dotBrush, HALF - 4, HALF - 4, 8, 8);

        // 흰 하이라이트 (입체감)
        Gdiplus::SolidBrush hilightBrush(Gdiplus::Color(180, 255, 210, 210));
        gfx.FillEllipse(&hilightBrush, HALF - 2, HALF - 3, 4, 3);
    }

    // 32비트 DIB → 실제로 그려진 픽셀의 알파 채널을 premultiplied 로 보정
    BYTE* p = (BYTE*)pBits;
    for (int i = 0; i < SZ * SZ; i++, p += 4)
    {
        BYTE a = p[3];
        p[0] = (BYTE)(p[0] * a / 255);
        p[1] = (BYTE)(p[1] * a / 255);
        p[2] = (BYTE)(p[2] * a / 255);
    }

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(hdc, cx - HALF, cy - HALF, SZ, SZ, offDC, 0, 0, SZ, SZ, bf);

    SelectObject(offDC, hOld);
    DeleteDC(offDC);
    DeleteObject(hBmp);
}

void UpdateScrollBars(HWND hWnd)
{
    if (g_fullscreen)
    {
        ShowScrollBar(hWnd, SB_BOTH, FALSE);
        return;
    }
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;

    if (!g_pdfDoc || g_pageLayouts.empty())
    {
        si.nMin = si.nMax = si.nPage = si.nPos = 0;
        SetScrollInfo(hWnd, SB_HORZ, &si, TRUE);
        SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
        return;
    }

    RECT cr;
    GetClientRect(hWnd, &cr);
    const RECT vp = GetViewportRect(cr);
    const int  vpW = vp.right - vp.left;
    const int  vpH = vp.bottom - vp.top;

    const int sbW = GetSystemMetrics(SM_CXVSCROLL);
    const int vpWsb = vpW - sbW;

    // 수평
    // 실제 콘텐츠 너비(페이지 + 여백)가 뷰포트를 넘지 않으면 가로 스크롤바 숨김
    const int contentW = g_maxPageWidth + SIDE_MARGIN * 2;
    if (contentW <= vpW)
    {
        // 가로 스크롤 불필요 — 스크롤바 숨기고 scrollX 초기화
        g_scrollX = 0;
        si.nMin = si.nMax = si.nPage = si.nPos = 0;
        SetScrollInfo(hWnd, SB_HORZ, &si, TRUE);
        ShowScrollBar(hWnd, SB_HORZ, FALSE);
    }
    else
    {
        si.nMin  = 0;
        si.nMax  = contentW;
        si.nPage = vpWsb;
        si.nPos  = g_scrollX = std::clamp(g_scrollX, 0, std::max(0, contentW - vpWsb));
        SetScrollInfo(hWnd, SB_HORZ, &si, TRUE);
        ShowScrollBar(hWnd, SB_HORZ, TRUE);
    }

    // 수직 — 전체 문서 높이 사용
    si.nMin = 0;
    si.nMax = g_totalDocHeight;
    si.nPage = vpH;
    si.nPos = g_scrollY = std::clamp(g_scrollY, 0, std::max(0, g_totalDocHeight - vpH));
    SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
}

void ZoomTo(HWND hWnd, float newZoom)
{
    if (!g_pdfDoc) return;
    // 줌 변경 전 현재 페이지 저장
    int savedPage = g_currentPage;
    g_zoom = std::clamp(newZoom, ZOOM_MIN, ZOOM_MAX);
    RecalcPageLayouts();
    ClearPageCache();
    InitPageCache(g_pageCount);
    ClearThumbCache();
    InitThumbCache(g_pageCount);
    g_scrollX = 0;
    // 저장된 페이지 위치로 복원 (유효한 경우)
    if (savedPage > 0 && savedPage < (int)g_pageLayouts.size()) {
        g_scrollY  = g_pageLayouts[savedPage].yOffset;
        g_scrollYf = (float)g_scrollY;
    } else {
        g_scrollY  = 0;
        g_scrollYf = 0.0f;
    }
    g_hasSelection = g_isSelecting = false;
    g_selPage = -1;
    // 줌 변경 시 PDF 좌표 → 화면 변환 결과가 달라지므로 rect 캐시 무효화
    for (auto& h : g_highlights)  h.cachedRects.clear();
    for (auto& u : g_underlines)  u.cachedRects.clear();
    for (auto& s : g_strikeouts)  s.cachedRects.clear();
    for (auto& q : g_squigglies)  q.cachedRects.clear();
    UpdateTitle(hWnd);
    UpdateScrollBars(hWnd);
    InvalidateRect(hWnd, nullptr, TRUE);
    TriggerAnnotLoadForVisiblePages(hWnd);
}

void FitToWidth(HWND hWnd)
{
    if (!g_pdfDoc || g_pageLayouts.empty()) return;
    RECT cr;
    GetClientRect(hWnd, &cr);
    const RECT vp = GetViewportRect(cr);
    int vpW = vp.right - vp.left;
    // 수직 스크롤바가 아직 표시되지 않은 경우에만 너비를 미리 보정
    // (이미 표시 중이면 GetClientRect가 스크롤바 너비를 이미 제외한 값을 반환)
    {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hWnd, SB_VERT, &si);
        bool vsbVisible = si.nMax > static_cast<int>(si.nPage);
        if (!vsbVisible)
            vpW -= GetSystemMetrics(SM_CXVSCROLL);
    }

    if (g_twoPageView)
    {
        // 두쪽 보기: 가장 넓은 행(좌+우 페이지 pdfW 합 + 간격) 기준
        constexpr double CENTER_GAP_PT = 4.0; // 픽셀 간격을 PDF 포인트로 근사
        double maxRowPdfW = 0;
        bool swapped = (g_rotation == 1 || g_rotation == 3);
        for (int i = 0; i < g_pageCount; i += 2)
        {
            double w0 = swapped ? g_pageLayouts[i].pdfH : g_pageLayouts[i].pdfW;
            double w1 = 0;
            if (i + 1 < g_pageCount)
                w1 = swapped ? g_pageLayouts[i+1].pdfH : g_pageLayouts[i+1].pdfW;
            double rowW = w0 + (w1 > 0 ? CENTER_GAP_PT / g_zoom + w1 : 0);
            if (rowW > maxRowPdfW) maxRowPdfW = rowW;
        }
        if (maxRowPdfW > 0.0)
            ZoomTo(hWnd, static_cast<float>((vpW - SIDE_MARGIN * 2) / maxRowPdfW));
    }
    else
    {
        // 단쪽 보기: 가장 넓은 페이지 기준
        double maxPdfW = 0;
        bool swapped = (g_rotation == 1 || g_rotation == 3);
        for (int i = 0; i < g_pageCount; i++)
        {
            double w = swapped ? g_pageLayouts[i].pdfH : g_pageLayouts[i].pdfW;
            if (w > maxPdfW) maxPdfW = w;
        }
        if (maxPdfW > 0.0)
            ZoomTo(hWnd, static_cast<float>((vpW - SIDE_MARGIN * 2) / maxPdfW));
    }
}

void FitToPage(HWND hWnd)
{
    if (!g_pdfDoc || g_pageLayouts.empty()) return;
    RECT cr;
    GetClientRect(hWnd, &cr);
    const RECT vp = GetViewportRect(cr);
    const int  vpW = vp.right - vp.left;
    const int  vpH = vp.bottom - vp.top;
    const double pageW = g_pageLayouts[g_currentPage].pdfW;
    const double pageH = g_pageLayouts[g_currentPage].pdfH;
    if (pageW <= 0.0 || pageH <= 0.0) return;

    // zoomH: 페이지 높이 + 페이지 간격이 정확히 뷰포트를 채우도록 계산
    const float zoomW = static_cast<float>((vpW - 20) / pageW);
    const float zoomH = static_cast<float>((vpH - PAGE_GAP) / pageH);
    ZoomTo(hWnd, std::min(zoomW, zoomH));
}

void NavigatePage(HWND hWnd, int delta)
{
    if (!g_pdfDoc) return;
    const int newPage = g_currentPage + delta;
    if (newPage < 0 || newPage >= g_pageCount) return;
    ScrollToPage(hWnd, newPage);
}

void HandleScroll(HWND hWnd, int bar, WPARAM wParam)
{
    // 자동 스크롤 중단
    if (bar == SB_VERT && g_autoScrolling)
    {
        g_autoScrolling = false;
        KillTimer(hWnd, AUTO_SCROLL_TIMER_ID);
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    if (bar == SB_VERT && g_slideshowActive)
    {
        g_slideshowActive = false;
        ExitFullscreen(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);
    }

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(hWnd, bar, &si);

    int& pos = (bar == SB_VERT) ? g_scrollY : g_scrollX;

    switch (LOWORD(wParam))
    {
    case SB_LINEUP:         pos -= SCROLL_LINE;   break;
    case SB_LINEDOWN:       pos += SCROLL_LINE;   break;
    case SB_PAGEUP:         pos -= (int)si.nPage; break;
    case SB_PAGEDOWN:       pos += (int)si.nPage; break;
    case SB_THUMBTRACK:     pos = si.nTrackPos; g_thumbTracking = true;  break;
    case SB_THUMBPOSITION:  pos = si.nTrackPos; g_thumbTracking = false; break;
    case SB_ENDSCROLL:      g_thumbTracking = false; break;
    case SB_TOP:            pos = si.nMin;        break;
    case SB_BOTTOM:         pos = si.nMax;        break;
    }

    const int maxPos = std::max(0, (int)si.nMax - (int)si.nPage);
    pos = std::clamp(pos, 0, maxPos);
    if (bar == SB_VERT)
    {
        g_scrollYf = (float)g_scrollY;
        g_scrollTargetY = g_scrollYf;
    }

    si.fMask = SIF_POS;
    si.nPos = pos;
    SetScrollInfo(hWnd, bar, &si, TRUE);

    UpdateActivePageFromScroll(hWnd);
    if (bar == SB_VERT)
    {
        if (!g_thumbTracking)
        {
            // 드래그 완료 또는 클릭 이동: 기존 큐를 버리고 현재 위치 기준으로 prefetch
            FlushRenderQueue();
            SchedulePrefetch(hWnd);
        }
        // SB_THUMBTRACK 중에는 prefetch 억제 — 큐 폭발 방지
    }
    g_pageBaseDirty = true;
    { RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT; InvalidateRect(hWnd, &ir, TRUE); }
    // 스크롤바 드래그가 막 끝난 경우(ENDSCROLL/THUMBPOSITION) thumbTracking이
    // 이미 false 이므로 여기서 한 번 더 prefetch를 보장
    if (bar == SB_VERT && !g_thumbTracking)
        SchedulePrefetch(hWnd);
}

// ─── 색상 메뉴 아이템 (Owner-draw)
// ColorMenuItem 구조체는 WindowsOps.h 참조

// 컨텍스트 메뉴에서 사용하는 색상 아이템 목록 (메뉴가 열려있는 동안 유지)
std::vector<ColorMenuItem> g_colorMenuItems;

// 색상 서브메뉴에 owner-draw 아이템을 추가하는 헬퍼
void AppendColorMenuItem(HMENU hMenu, UINT id, COLORREF color, const wchar_t* label, bool enabled)
{
    ColorMenuItem item{ color, label };
    g_colorMenuItems.push_back(std::move(item));

    MENUITEMINFOW mii = {};
    mii.cbSize     = sizeof(mii);
    mii.fMask      = MIIM_ID | MIIM_TYPE | MIIM_DATA | MIIM_STATE;
    mii.fType      = MFT_OWNERDRAW;
    mii.fState     = enabled ? MFS_ENABLED : MFS_GRAYED;
    mii.wID        = id;
    mii.dwItemData = (ULONG_PTR)&g_colorMenuItems.back();
    InsertMenuItemW(hMenu, -1, FALSE, &mii);
}

// ─── WinMain ──────────────────────────────────────────────────────────────────

// EXE 자신의 VS_VERSION_INFO 에서 버전 문자열을 읽어 반환. 실패 시 fallback.
static std::wstring GetAppVersionString()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    DWORD dummy = 0;
    const DWORD infoSize = GetFileVersionInfoSizeW(path, &dummy);
    if (infoSize == 0) return L"Version 0.29";

    std::vector<BYTE> buf(infoSize);
    if (!GetFileVersionInfoW(path, 0, infoSize, buf.data())) return L"Version 0.29";

    VS_FIXEDFILEINFO* fi = nullptr;
    UINT fiLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &fiLen) || !fi)
        return L"Version 0.29";

    const WORD major = HIWORD(fi->dwProductVersionMS);
    const WORD minor = LOWORD(fi->dwProductVersionMS);
    const WORD patch = HIWORD(fi->dwProductVersionLS);
    const WORD build = LOWORD(fi->dwProductVersionLS);

    wchar_t ver[64];
    if (build > 0)
        swprintf_s(ver, L"Version %d.%d.%d.%d", major, minor, patch, build);
    else if (patch > 0)
        swprintf_s(ver, L"Version %d.%d.%d", major, minor, patch);
    else
        swprintf_s(ver, L"Version %d.%d", major, minor);

    return ver;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    Lang_Init();

    // PrintDlgW 등 comdlg32는 프로세스 UI 언어를 사용하므로
    // 시작 시 프로세스 전체 언어를 앱 설정 언어로 지정한다.
    {
        static const wchar_t* s_procLangNames[] = {
            L"ko-KR\0",  // AppLang::Korean
            L"en-US\0",  // AppLang::English
            L"ja-JP\0",  // AppLang::Japanese
            L"zh-CN\0",  // AppLang::Chinese
            L"es-ES\0",  // AppLang::Spanish
            L"fr-FR\0",  // AppLang::French
            L"de-DE\0",  // AppLang::German
            L"pt-PT\0",  // AppLang::Portuguese
            L"ru-RU\0",  // AppLang::Russian
        };
        int li = (int)g_appLang;
        if (li < 0 || li >= (int)_countof(s_procLangNames)) li = 0;
        ULONG n = 0;
        SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, s_procLangNames[li], &n);
        SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, s_procLangNames[li], &n);
        const LANGID s_langIds[] = {
            MAKELANGID(LANG_KOREAN,     SUBLANG_DEFAULT),         // Korean
            MAKELANGID(LANG_ENGLISH,    SUBLANG_DEFAULT),         // English
            MAKELANGID(LANG_JAPANESE,   SUBLANG_DEFAULT),         // Japanese
            MAKELANGID(LANG_CHINESE,    SUBLANG_CHINESE_SIMPLIFIED), // Chinese
            MAKELANGID(LANG_SPANISH,    SUBLANG_DEFAULT),         // Spanish
            MAKELANGID(LANG_FRENCH,     SUBLANG_DEFAULT),         // French
            MAKELANGID(LANG_GERMAN,     SUBLANG_DEFAULT),         // German
            MAKELANGID(LANG_PORTUGUESE, SUBLANG_DEFAULT),         // Portuguese
            MAKELANGID(LANG_RUSSIAN,    SUBLANG_DEFAULT),         // Russian
        };
        SetThreadUILanguage(s_langIds[li]);
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    GroupPanel::LoadGroups();

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_PLAINPDFVIEWER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow))
        return FALSE;

    // 커맨드라인으로 전달된 파일 경로 처리 (파일 연결 / 더블 클릭으로 열기)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            // argv[0] = 실행 파일 경로, argv[1] 이후가 인수
            if (argc >= 2 && argv[1][0] != L'\0')
            {
                std::wstring path = argv[1];
                // 따옴표 제거 (탐색기가 붙이는 경우 대비)
                if (!path.empty() && path.front() == L'"') path = path.substr(1);
                if (!path.empty() && path.back()  == L'"') path.pop_back();
                if (!path.empty())
                    OpenFileFromPath(g_hMainWnd, path, true);
            }
            LocalFree(argv);
        }
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_PLAINPDFVIEWER));
    MSG msg = {};

    while (GetMessage(&msg, nullptr, 0, 0))
    {
        // 찾기 에디트에 포커스가 있으면 Ctrl+F 등 액셀러레이터를 차단하지 않되
        // 일반 타이핑은 에디트로 전달되도록 처리
        bool inFindEdit = (g_hFindEdit && GetFocus() == g_hFindEdit);

        if (inFindEdit && msg.message == WM_KEYDOWN)
        {
            // F3, Shift+F3, Ctrl+F는 액셀러레이터로 처리
            if (msg.wParam == VK_F3 ||
                (msg.wParam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)))
            {
                if (TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
                    continue;
            }
            // 나머지는 에디트로 직접 전달
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // 텍스트 박스 편집 중이면 액셀러레이터 차단 — Edit 컨트롤이 모든 키 처리
        bool inTextBoxEdit = (g_hTextBoxEdit && GetFocus() == g_hTextBoxEdit);
        if (inTextBoxEdit)
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg) || g_formFieldFocused)
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return (int)msg.wParam;
}

// ─── 툴바 툴팁 팝업 WndProc ──────────────────────────────────────────────────
static LRESULT CALLBACK TipWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);

        Gdiplus::Graphics gfx(hdc);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        int w = rc.right, h = rc.bottom;

        // 배경: 어두운 반투명 느낌의 단색 (다크 슬레이트)
        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(245, 30, 30, 36));
        Gdiplus::GraphicsPath path;
        int r = 6;
        path.AddArc(0, 0, r*2, r*2, 180, 90);
        path.AddArc(w-1-r*2, 0, r*2, r*2, 270, 90);
        path.AddArc(w-1-r*2, h-1-r*2, r*2, r*2, 0, 90);
        path.AddArc(0, h-1-r*2, r*2, r*2, 90, 90);
        path.CloseFigure();
        gfx.FillPath(&bgBrush, &path);

        // 테두리: 약간 밝은 선
        Gdiplus::Pen borderPen(Gdiplus::Color(180, 80, 80, 95), 1.0f);
        gfx.DrawPath(&borderPen, &path);

        // 텍스트 (탭 툴팁이면 g_tabTipText, 아니면 g_toolbarTipText)
        const std::wstring& tipText = (hWnd == g_hTabTooltip) ? g_tabTipText : g_toolbarTipText;
        Gdiplus::FontFamily fontFamily(L"Segoe UI");
        Gdiplus::Font font(&fontFamily, 9.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 220, 220, 228));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF rcText((float)(r + 4), 0.0f, (float)(w - r*2 - 8), (float)h);
        gfx.DrawString(tipText.c_str(), -1, &font, rcText, &sf, &textBrush);

        EndPaint(hWnd, &ps);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─── 창 클래스 등록 ────────────────────────────────────────────────────────────

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    // 툴팁 팝업 클래스 등록
    WNDCLASSEXW tcex = {};
    tcex.cbSize        = sizeof(WNDCLASSEXW);
    tcex.lpfnWndProc   = TipWndProc;
    tcex.hInstance     = hInstance;
    tcex.hbrBackground = (HBRUSH)(COLOR_INFOBK + 1);
    tcex.lpszClassName = L"PlainPDFTip";
    RegisterClassExW(&tcex);
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_PLAINPDFVIEWER));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_PLAINPDFVIEWER);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_PLAINPDFVIEWER));
    return RegisterClassExW(&wcex);
}

// ─── 인스턴스 초기화 ────────────────────────────────────────────────────────────

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    int wx, wy, ww, wh;
    bool wasMaximized = false;
    bool hasSaved = LoadWindowPlacement(&wx, &wy, &ww, &wh, &wasMaximized);

    if (!hasSaved)
    {
        // 최초 실행: 기본 모니터의 80% 크기, 중앙 배치
        HMONITOR hMon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);
        int monW = mi.rcWork.right - mi.rcWork.left;
        int monH = mi.rcWork.bottom - mi.rcWork.top;
        ww = monW * 80 / 100;
        wh = monH * 80 / 100;
        wx = mi.rcWork.left + (monW - ww) / 2;
        wy = mi.rcWork.top + (monH - wh) / 2;
    }
    else
    {
        // 저장된 위치가 모니터 밖이면 보정
        RECT rcTest = { wx, wy, wx + ww, wy + wh };
        HMONITOR hMon = MonitorFromRect(&rcTest, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);
        if (wx + ww < mi.rcWork.left + 50) wx = mi.rcWork.left;
        if (wy + wh < mi.rcWork.top + 50)  wy = mi.rcWork.top;
        if (wx > mi.rcWork.right - 50)  wx = mi.rcWork.right - ww;
        if (wy > mi.rcWork.bottom - 50) wy = mi.rcWork.bottom - wh;
    }

    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        wx, wy, ww, wh,
        nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return FALSE;
    g_hMainWnd = hWnd;

    if (hasSaved && wasMaximized)
        ShowWindow(hWnd, SW_SHOWMAXIMIZED);
    else
        ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    return TRUE;
}

// ─── 메시지 처리 ───────────────────────────────────────────────────────────────

// 슬라이드쇼 화살표 버튼 rect (버튼 너비 60, 높이 화면의 40%, 세로 중앙)
static RECT GetSSArrowRect(const RECT& cr, bool left)
{
    const int W  = 60;
    const int H  = (cr.bottom - cr.top) * 2 / 5;
    const int cy = cr.top + (cr.bottom - cr.top - H) / 2;
    if (left)
        return { cr.left,          cy, cr.left  + W, cy + H };
    else
        return { cr.right - W, cy, cr.right,         cy + H };
}

static RECT GetSSCloseRect(const RECT& cr)
{
    const int SZ  = 40;
    const int PAD = 16;
    return { cr.right - PAD - SZ, cr.top + PAD, cr.right - PAD, cr.top + PAD + SZ };
}

void EnterFullscreen(HWND hWnd)
{
    if (g_fullscreen) return;
    g_windowedStyle = GetWindowLong(hWnd, GWL_STYLE);
    GetWindowRect(hWnd, &g_windowedRect);

    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    // 메뉴바 숨기기
    g_hMainMenu = GetMenu(hWnd);
    SetMenu(hWnd, nullptr);
    // 검색 패널 숨기기
    if (g_hFindPanel) ShowWindow(g_hFindPanel, SW_HIDE);
    // 타이틀/테두리 제거 + 전체화면 플래그 먼저 설정
    g_fullscreen = true;
    SetWindowLong(hWnd, GWL_STYLE,
        (g_windowedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_HSCROLL | WS_VSCROLL)));
    SetWindowPos(hWnd, HWND_TOP,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right  - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    // SetWindowPos 이후 스크롤바 숨기기 (FRAMECHANGED로 재생성된 것까지 제거)
    ShowScrollBar(hWnd, SB_BOTH, FALSE);
}

void ExitFullscreen(HWND hWnd)
{
    if (!g_fullscreen) return;
    g_fullscreen = false;
    // 마우스 캡처 해제 (슬라이드쇼 중 SetCapture가 걸려 있을 수 있음)
    if (GetCapture() == hWnd) ReleaseCapture();
    // 스타일 복원
    SetWindowLong(hWnd, GWL_STYLE, g_windowedStyle);
    SetWindowPos(hWnd, nullptr,
        g_windowedRect.left, g_windowedRect.top,
        g_windowedRect.right  - g_windowedRect.left,
        g_windowedRect.bottom - g_windowedRect.top,
        SWP_FRAMECHANGED | SWP_NOZORDER);
    // 메뉴바 복원
    SetMenu(hWnd, g_hMainMenu);
    g_hMainMenu = nullptr;
    // 검색 패널 복원
    if (g_hFindPanel && g_findBarVisible) ShowWindow(g_hFindPanel, SW_SHOW);
    // 스크롤바 복원
    UpdateScrollBars(hWnd);
    // 창을 포그라운드로 올리고 즉시 재페인트
    // (데스크톱/멀티모니터 환경에서 첫 클릭이 활성화에 소비되는 문제 방지)
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
    UpdateWindow(hWnd);
}

void ToggleSlideshow(HWND hWnd)
{
    if (!g_pdfDoc) return;
    g_slideshowActive = !g_slideshowActive;
    InvalidateToolbarBaseCache();
    if (g_slideshowActive)
    {
        EnterFullscreen(hWnd);
    }
    else
    {
        ExitFullscreen(hWnd);
    }
    InvalidateRect(hWnd, nullptr, FALSE);
}

// ── ShowContextMenu 전방 선언 (본체는 WndProc 내 인라인 블록에 있음) ─────────
// 실제 ShowContextMenu 구현은 WM_CONTEXTMENU case 블록 내에 인라인으로 존재하며,
// WM_RBUTTONDOWN에서 g_contextMenuPt를 설정한 후 WM_CONTEXTMENU를 직접 디스패치한다.
static void ShowContextMenu(HWND hWnd, POINT ptScreen)
{
    // WM_CONTEXTMENU를 hWnd에 직접 SendMessage로 처리
    // (PostMessage 대신 SendMessage를 사용해 메시지 큐 개입 없이 동기 호출)
    g_contextMenuPt = ptScreen;
    SendMessage(hWnd, WM_CONTEXTMENU, (WPARAM)hWnd,
                MAKELPARAM((WORD)ptScreen.x, (WORD)ptScreen.y));
}

// 메뉴 문자열을 현재 언어로 교체
static void UpdateMenuLanguage(HWND hWnd)
{
    HMENU hMenu = GetMenu(hWnd);
    if (!hMenu) return;

    // 팝업 제목 교체
    MENUITEMINFOW mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask  = MIIM_STRING;

    auto SetPopup = [&](int pos, const wchar_t* str) {
        mii.dwTypeData = const_cast<LPWSTR>(str);
        SetMenuItemInfoW(hMenu, pos, TRUE, &mii);
    };
    SetPopup(0, LS(STR_MENU_FILE));
    SetPopup(1, LS(STR_MENU_VIEW));
    SetPopup(2, LS(STR_MENU_EDIT));
    SetPopup(3, LS(STR_MENU_GROUP));
    SetPopup(4, LS(STR_MENU_PAGE));
    SetPopup(5, LS(STR_MENU_ANALYZE));
    SetPopup(6, LS(STR_MENU_SETTINGS));
    SetPopup(7, LS(STR_MENU_HELP));
    // 인덱스 8 = Language (항상 고정, 갱신 불필요)

    // 파일 메뉴
    HMENU hFile = GetSubMenu(hMenu, 0);
    auto SetItem = [&](HMENU hSub, UINT id, const wchar_t* str) {
        mii.dwTypeData = const_cast<LPWSTR>(str);
        SetMenuItemInfoW(hSub, id, FALSE, &mii);
    };
    SetItem(hFile, IDM_FILE_OPEN,   LS(STR_MENU_OPEN));
    SetItem(hFile, IDM_SAVE,        LS(STR_MENU_SAVE));
    SetItem(hFile, IDM_SAVE_AS,     LS(STR_MENU_SAVEAS));
    SetItem(hFile, IDM_NEW_TAB,     LS(STR_MENU_NEWTAB));
    SetItem(hFile, IDM_CLOSE_TAB,   LS(STR_MENU_CLOSETAB));
    SetItem(hFile, IDM_PRINT,       LS(STR_MENU_PRINT));
    SetItem(hFile, IDM_COMPRESS,    (::LS)(L"PDF \uC555\uCD95(&C)...", L"Compress PDF(&C)...", L"PDF\u3092\u5727\u7E2E(&C)...", L"\u538B\u7F29 PDF(&C)...",
                                         L"Comprimir PDF(&C)...", L"Compresser le PDF(&C)...", L"PDF komprimieren(&C)...", L"Comprimir PDF(&C)...", L"\u0421\u0436\u0430\u0442\u044C PDF(&C)..."));
    SetItem(hFile, IDM_EXPORT_IMAGE, (::LS)(L"이미지로 내보내기(&E)...", L"Export as Image(&E)...", L"画像として書き出し(&E)...", L"导出为图像(&E)...",
                                         L"Exportar como imagen(&E)...", L"Exporter en image(&E)...", L"Als Bild exportieren(&E)...", L"Exportar como imagem(&E)...", L"Экспорт в изображение(&E)..."));
    SetItem(hFile, IDM_PROPERTIES,  LS(STR_MENU_PROPS));
    SetItem(hFile, IDM_EXIT,        LS(STR_MENU_EXIT));

    // 보기 메뉴
    HMENU hView = GetSubMenu(hMenu, 1);
    SetItem(hView, IDM_PREV_PAGE,        LS(STR_MENU_PREVPAGE));
    SetItem(hView, IDM_NEXT_PAGE,        LS(STR_MENU_NEXTPAGE));
    SetItem(hView, IDM_ZOOM_IN,          LS(STR_MENU_ZOOMIN));
    SetItem(hView, IDM_ZOOM_OUT,         LS(STR_MENU_ZOOMOUT));
    SetItem(hView, IDM_ZOOM_RESET,       LS(STR_MENU_ZOOMRESET));
    SetItem(hView, IDM_FIT_WIDTH,        LS(STR_MENU_FITWIDTH));
    SetItem(hView, IDM_FIND,             LS(STR_MENU_FIND));
    SetItem(hView, IDM_TOGGLE_SIDEBAR,   LS(STR_MENU_SIDEBAR));
    SetItem(hView, IDM_ROTATE_CW,        LS(STR_MENU_ROTATECW));
    SetItem(hView, IDM_ROTATE_CCW,       LS(STR_MENU_ROTATECCW));
    SetItem(hView, IDM_DARK_MODE,        LS(STR_MENU_DARKMODE));
    SetItem(hView, IDM_TWO_PAGE,         LS(STR_MENU_TWOPAGE));
    SetItem(hView, IDM_TOGGLE_TOOLBAR,   (::LS)(L"\uD234\uBC14 \uC228\uAE30\uAE30(&T)", L"Hide Toolbar(&T)", L"\u30C4\u30FC\u30EB\u30D0\u30FC\u3092\u975E\u8868\u793A(&T)", L"\u9690\u85CF\u5DE5\u5177\u680F(&T)",
                                         L"Ocultar barra de herramientas(&T)", L"Masquer la barre d'outils(&T)", L"Symbolleiste ausblenden(&T)", L"Ocultar barra de ferramentas(&T)", L"\u0421\u043A\u0440\u044B\u0442\u044C \u043F\u0430\u043D\u0435\u043B\u044C \u0438\u043D\u0441\u0442\u0440\u0443\u043C\u0435\u043D\u0442\u043E\u0432(&T)"));

    // 편집 메뉴
    HMENU hEdit = GetSubMenu(hMenu, 2);
    SetItem(hEdit, IDM_DELETE_ALL_ANNOTS,  LS(STR_MENU_DELANNO));
    SetItem(hEdit, IDM_KEYWORD_HIGHLIGHT,  LS(STR_MENU_KEYWORD));
    SetItem(hEdit, IDM_SIGN,               LS(STR_MENU_SIGN));
    SetItem(hEdit, IDM_REDACT,             LS(STR_MENU_REDACT));
    SetItem(hEdit, IDM_SHAPE_RECT,         LS(STR_MENU_SHAPE_RECT));
    SetItem(hEdit, IDM_SHAPE_CIRCLE,       LS(STR_MENU_SHAPE_CIRCLE));
    SetItem(hEdit, IDM_SHAPE_COLOR,        LS(STR_MENU_SHAPE_COLOR));

    // 그룹 메뉴
    HMENU hGroup = GetSubMenu(hMenu, 3);
    SetItem(hGroup, IDM_GROUP_PANEL,   LS(STR_MENU_GROUPPANEL));
    SetItem(hGroup, IDM_GROUP_MANAGE,  LS(STR_MENU_GROUPMANAGE));

    // 페이지 메뉴
    HMENU hPage = GetSubMenu(hMenu, 4);
    SetItem(hPage, IDM_PAGE_MANAGER, LS(STR_MENU_PAGEMGR));

    // 분석 메뉴
    HMENU hAnalyze = GetSubMenu(hMenu, 5);
    SetItem(hAnalyze, IDM_WORD_FREQ,   LS(STR_MENU_WORDFREQ));
    SetItem(hAnalyze, IDM_WORD_CLOUD,  LS(STR_MENU_WORDCLOUD));

    // Language 선택 메뉴 (인덱스 8) — 현재 언어에 체크 표시
    HMENU hLang = GetSubMenu(hMenu, 8);
    if (hLang)
    {
        for (int i = IDM_LANG_0; i <= IDM_LANG_8; ++i)
            CheckMenuItem(hLang, i, MF_BYCOMMAND | MF_UNCHECKED);
        CheckMenuItem(hLang, IDM_LANG_0 + (int)g_appLang, MF_BYCOMMAND | MF_CHECKED);
    }

    // 설정 메뉴 (인덱스 6)
    HMENU hSettings = GetSubMenu(hMenu, 6);
    SetItem(hSettings, IDM_PREFERENCES,    LS(STR_MENU_PREFS));
    SetItem(hSettings, IDM_SET_DEFAULT_PDF, LS(STR_MENU_SET_DEFAULT_PDF));

    // 도움말 메뉴 (인덱스 7)
    HMENU hHelp = GetSubMenu(hMenu, 7);
    SetItem(hHelp, IDM_LICENSE, LS(STR_MENU_LICENSE));
    SetItem(hHelp, IDM_ABOUT,   LS(STR_MENU_ABOUT));

    DrawMenuBar(hWnd);
}

static void SidebarFinishLayout(HWND hWnd)
{
    g_sidebarHoverIdx = -1;
    UpdateScrollBars(hWnd);
    InvalidateToolbarBaseCache();
    InvalidateRect(hWnd, nullptr, TRUE);
    FitToWidth(hWnd);
}

static void SidebarAnimStep(HWND hWnd)
{
    if (!s_sbAnimating) { KillTimer(hWnd, SB_ANIM_TIMER); return; }
    double t = (double)(GetTickCount64() - s_sbStart) / (double)SB_ANIM_MS;
    if (t > 1.0) t = 1.0;
    double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    double shown = s_sbClosing ? (1.0 - e) : e;
    s_sbOffset = (int)(SIDEBAR_WIDTH * shown) - SIDEBAR_WIDTH;

    if (t >= 1.0)
    {
        KillTimer(hWnd, SB_ANIM_TIMER);
        s_sbAnimating = false;
        s_sbOffset = 0;
        if (s_sbClosing)
        {
            s_sbClosing = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        else
            InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }
    RECT cr; GetClientRect(hWnd, &cr);
    RECT sbInv = { 0, GetBelowToolbarY(), SIDEBAR_WIDTH, cr.bottom - STATUSBAR_HEIGHT };
    InvalidateRect(hWnd, &sbInv, FALSE);
}

static void ScrollThumbnailToCurrentPage(HWND hWnd)
{
    if (!g_pdfDoc || g_pageLayouts.empty() || g_currentPage < 0 || g_currentPage >= g_pageCount) return;
    RECT cr; GetClientRect(hWnd, &cr);
    const int availW = SIDEBAR_WIDTH - SIDEBAR_THUMB_PAD * 2;
    const int contentH = (cr.bottom - STATUSBAR_HEIGHT) - (GetBelowToolbarY() + SIDEBAR_HEADER_H);
    int top = 0, itemH = 0;
    for (int i = 0; i <= g_currentPage; ++i)
    {
        double pw = g_pageLayouts[i].pdfW;
        double ph = g_pageLayouts[i].pdfH;
        if (pw <= 0) pw = 1;
        itemH = (int)(ph / pw * availW) + 20 + SIDEBAR_THUMB_GAP;
        if (i < g_currentPage) top += itemH;
    }
    const int maxScroll = std::max(0, GetThumbnailTotalHeight(cr) - contentH);
    g_thumbScrollY = std::clamp(top - (contentH - itemH) / 2, 0, maxScroll);
}

static void SidebarToggleAnimated(HWND hWnd, bool close, SidebarTab tab)
{
    // 진행 중인 애니메이션은 즉시 마무리
    if (s_sbAnimating)
    {
        KillTimer(hWnd, SB_ANIM_TIMER);
        s_sbAnimating = false;
        s_sbOffset = 0;
        s_sbClosing = false;
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    bool wasVisible = g_sidebarVisible;
    if (close)
    {
        s_sbClosing = true;
        s_sbAnimating = true;
        s_sbOffset = 0;
        s_sbStart = GetTickCount64();
        g_sidebarVisible = false;
        SidebarFinishLayout(hWnd);
        SetTimer(hWnd, SB_ANIM_TIMER, 15, nullptr);
        return;
    }
    g_sidebarVisible = true;
    g_sidebarTab = tab;
    if (tab == TAB_THUMBNAILS) ScrollThumbnailToCurrentPage(hWnd);
    SidebarFinishLayout(hWnd);
    if (!wasVisible)
    {
        s_sbClosing = false;
        s_sbAnimating = true;
        s_sbOffset = -SIDEBAR_WIDTH;
        s_sbStart = GetTickCount64();
        SetTimer(hWnd, SB_ANIM_TIMER, 15, nullptr);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        FPDF_InitLibrary();
        CleanPopup::Init(((CREATESTRUCT*)lParam)->hInstance);
        DragAcceptFiles(hWnd, TRUE);
        g_curHand = LoadCursor(nullptr, IDC_HAND);
        g_curGrab = LoadCursor(nullptr, IDC_SIZEALL);
        LoadRecentFiles();
        LoadPrefs();
        LoadQuickJumps();
        g_selectionColor  = g_prefs.selectionColor;
        g_maxRecentFiles  = g_prefs.maxRecentFiles;
        UpdateRecentFilesMenu(hWnd);
        UpdateMenuLanguage(hWnd);
        // 찾기 패널 생성 (숨김 상태)
        CreateFindPanel(hWnd);
        // 메모 툴팁 컨트롤 생성
        g_hNoteTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | 0x0020 /*TTS_MULTILINE*/ | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hWnd, nullptr, ((CREATESTRUCT*)lParam)->hInstance, nullptr);
        if (g_hNoteTooltip)
        {
            SendMessageW(g_hNoteTooltip, TTM_SETMAXTIPWIDTH, 0, 400);
            TOOLINFOW ti = {};
            ti.cbSize   = sizeof(ti);
            ti.uFlags   = TTF_TRACK | TTF_ABSOLUTE;
            ti.hwnd     = hWnd;
            ti.uId      = 1;
            ti.lpszText = const_cast<LPWSTR>(L"");
            SendMessageW(g_hNoteTooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        // 툴바 툴팁 팝업 윈도우 생성
        g_hToolbarTooltip = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"PlainPDFTip", L"",
            WS_POPUP,
            0, 0, 10, 10,
            hWnd, nullptr, ((CREATESTRUCT*)lParam)->hInstance, nullptr);
        // 탭 파일명 툴팁 팝업 윈도우 생성 (툴바 툴팁과 동일한 방식)
        g_hTabTooltip = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"PlainPDFTip", L"",
            WS_POPUP,
            0, 0, 10, 10,
            hWnd, nullptr, ((CREATESTRUCT*)lParam)->hInstance, nullptr);
        // memo.png 리소스 로드
        {
            HINSTANCE hInst = ((CREATESTRUCT*)lParam)->hInstance;
            HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDB_MEMO_PNG), RT_RCDATA);
            OutputDebugStringW(hRes ? L"[MEMO] FindResource OK\n" : L"[MEMO] FindResource FAILED\n");
            if (hRes)
            {
                HGLOBAL hGlob = LoadResource(hInst, hRes);
                DWORD sz = SizeofResource(hInst, hRes);
                void* pData = hGlob ? LockResource(hGlob) : nullptr;
                OutputDebugStringW((pData && sz > 0) ? L"[MEMO] LockResource OK\n" : L"[MEMO] LockResource FAILED\n");
                if (pData && sz > 0)
                {
                    HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, sz);
                    if (hBuf)
                    {
                        void* pBuf = GlobalLock(hBuf);
                        if (pBuf)
                        {
                            memcpy(pBuf, pData, sz);
                            GlobalUnlock(hBuf);
                            IStream* pStream = nullptr;
                            if (SUCCEEDED(CreateStreamOnHGlobal(hBuf, TRUE, &pStream)))
                            {
                                Gdiplus::Bitmap* tmp = Gdiplus::Bitmap::FromStream(pStream);
                                if (tmp && tmp->GetLastStatus() == Gdiplus::Ok)
                                {
                                    OutputDebugStringW(L"[MEMO] Bitmap load OK\n");
                                    UINT w = tmp->GetWidth(), h = tmp->GetHeight();
                                    g_pMemoBitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
                                    if (g_pMemoBitmap)
                                    {
                                        Gdiplus::Graphics g(g_pMemoBitmap);
                                        g.DrawImage(tmp, 0, 0, (INT)w, (INT)h);
                                    }
                                    delete tmp;
                                }
                                else
                                {
                                    OutputDebugStringW(L"[MEMO] Bitmap FromStream FAILED\n");
                                    delete tmp;
                                }
                                pStream->Release();
                            }
                        }
                        else { GlobalFree(hBuf); }
                    }
                }
            }
        }
        StartRenderWorker();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDM_DELETE_ALL_ANNOTS:
        {
            if (g_highlights.empty() && g_underlines.empty() && g_strikeouts.empty()
                && g_squigglies.empty() && g_notes.empty() && g_textBoxes.empty()
                && g_signatures.empty())
            {
                CleanPopup::Show(hWnd, LS(STR_ANNOT_DELETE_TITLE), LS(STR_ANNOT_DELETE_CONFIRM_EMPTY));
                break;
            }
            if (CleanPopup::Show(hWnd, LS(STR_ANNOT_DELETE_TITLE), LS(STR_ANNOT_DELETE_CONFIRM), true))
            {
                g_highlights.clear();
                g_underlines.clear();
                g_strikeouts.clear();
                g_squigglies.clear();
                g_notes.clear();
                g_textBoxes.clear();
                for (auto& s : g_signatures) delete s.bitmap;
                g_signatures.clear();
                g_shapes.clear();
                g_selectedHighlight = -1;
                g_selectedUnderline = -1;
                g_selectedStrikeout = -1;
                g_selectedSquiggly  = -1;
                g_selectedNote      = -1;
                g_selectedTextBox   = -1;
                g_selectedSig       = -1;
                g_selectedShape     = -1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }
        case IDM_HIGHLIGHT_MODE:
        {
            g_highlightMode = !g_highlightMode;
            // 하이라이트 모드 켜면 손 도구 / 잉크 모드 해제
            if (g_highlightMode) { g_handMode = false; g_inkMode = false; }
            InvalidateToolbarBaseCache();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case IDM_HIGHLIGHT_COLOR:
        {
            CHOOSECOLORW cc = {};
            static COLORREF customColors[16] = {};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner   = hWnd;
            cc.lpCustColors = customColors;
            cc.rgbResult   = g_highlightColor;
            cc.Flags       = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc))
            {
                g_highlightColor = cc.rgbResult;
                g_pageBaseDirty = true;
                InvalidateToolbarBaseCache();
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }
        case IDM_INK_MODE:
        {
            g_inkMode = !g_inkMode;
            // 잉크 모드 켜면 손 도구 / 하이라이트 모드 해제
            if (g_inkMode) { g_handMode = false; g_highlightMode = false; }
            g_isDrawingInk = false;
            g_inkStroke.clear();
            SetCursor(LoadCursor(nullptr, g_inkMode ? IDC_CROSS : IDC_ARROW));
            InvalidateToolbarBaseCache();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case IDM_INK_COLOR:
        {
            CHOOSECOLORW cc2 = {};
            static COLORREF inkCustomColors[16] = {};
            cc2.lStructSize  = sizeof(cc2);
            cc2.hwndOwner    = hWnd;
            cc2.lpCustColors = inkCustomColors;
            cc2.rgbResult    = g_inkColor;
            cc2.Flags        = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc2))
            {
                g_inkColor = cc2.rgbResult;
                InvalidateToolbarBaseCache();
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }
        case IDM_KEYWORD_HIGHLIGHT:
            if (!g_pdfDoc) {
                CleanPopup::Show(hWnd, LS(STR_KW_TITLE), LS(STR_NEED_PDF_OPEN));
                break;
            }
            ShowKeywordDialog(hWnd);
            break;
        case IDM_WORD_FREQ:
            ShowWordFreqDialog(hWnd);
            break;
        case IDM_WORD_CLOUD:
            ShowWordCloudDialog(hWnd, g_pdfDoc, g_pageCount);
            break;
        case IDM_SIGN:
        {
            if (!g_pdfDoc) {
                CleanPopup::Show(hWnd, LS(STR_SIG_TITLE), LS(STR_NEED_PDF_OPEN));
                break;
            }
            SignatureResult sr = ShowSignaturePad(hWnd);
            if (sr.bmp && (sr.isImage || !sr.strokes.empty()))
            {
                double pageW = g_pageLayouts[g_currentPage].pdfW;
                double pageH = g_pageLayouts[g_currentPage].pdfH;
                double pdfW = 160.0 / g_zoom;
                double pdfH = (sr.bmpH > 0)
                    ? pdfW * (double)sr.bmpH / (double)sr.bmpW
                    : 54.0 / g_zoom;
                double pdfX = pageW / 2.0 - pdfW / 2.0;
                double pdfY = pageH / 2.0 + pdfH / 2.0;

                // 캔버스 픽셀 좌표 → PDF 좌표로 변환하여 획 저장
                double scaleX = pdfW / (double)sr.canvasW;
                double scaleY = pdfH / (double)sr.canvasH;

                SignatureItem item;
                item.pageIndex = g_currentPage;
                item.pdfX = pdfX; item.pdfY = pdfY;
                item.pdfW = pdfW; item.pdfH = pdfH;
                item.bitmap = sr.bmp;
                item.srcW = sr.bmpW; item.srcH = sr.bmpH;
                item.isImage = sr.isImage;
                for (auto& s : sr.strokes) {
                    std::vector<SigStrokePt> stroke;
                    for (auto& p : s) {
                        // 캔버스 Y는 위→아래, PDF Y는 아래→위
                        float px = (float)(pdfX + p.x * scaleX);
                        float py = (float)(pdfY - p.y * scaleY);
                        stroke.push_back({ px, py });
                    }
                    item.strokes.push_back(std::move(stroke));
                }
                g_signatures.push_back(std::move(item));
                g_selectedSig = (int)g_signatures.size() - 1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }
        case IDM_AUTO_SCROLL:
        {
            if (!g_pdfDoc) break;
            g_autoScrolling = !g_autoScrolling;
            if (g_autoScrolling)
                SetTimer(hWnd, AUTO_SCROLL_TIMER_ID, AUTO_SCROLL_INTERVAL, nullptr);
            else
                KillTimer(hWnd, AUTO_SCROLL_TIMER_ID);
            InvalidateToolbarBaseCache();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case IDM_EXTRACT_TEXT:
        {
            if (!g_pdfDoc) break;
            ShowTextViewer(hWnd, g_currentPage);
            break;
        }
        case IDM_SLIDESHOW:
        {
            ToggleSlideshow(hWnd);
            break;
        }
        case IDM_FILE_OPEN:  OpenPDFFile(hWnd);                                         break;
        case IDM_SAVE:       SaveDocument(hWnd, false);                                   break;
        case IDM_SAVE_AS:    SaveDocument(hWnd, true);                                    break;
        case IDM_PRINT:      PrintDocument(hWnd);                                        break;
        case IDM_COMPRESS:   ShowCompressPdfDialog(hWnd, g_filePath);                    break;
        case IDM_EXPORT_IMAGE: ExportPagesAsImages(hWnd);                                break;
        case IDM_PROPERTIES: DocPropertiesDialog::Show(hWnd, g_pdfDoc, g_filePath, g_pageCount); break;
        case IDM_REDACT:
        {
            if (!g_pdfDoc) { CleanPopup::Show(hWnd, LS(STR_REDACT_TITLE), LS(STR_REDACT_NO_PDF)); break; }
            g_redactMode = !g_redactMode;
            // 모드 종료 시 드래그 상태도 초기화
            if (!g_redactMode) g_isDraggingRedact = false;
            HMENU hMenu = GetMenu(hWnd);
            if (hMenu) CheckMenuItem(hMenu, IDM_REDACT,
                MF_BYCOMMAND | (g_redactMode ? MF_CHECKED : MF_UNCHECKED));
            SetCursor(LoadCursor(nullptr, g_redactMode ? IDC_CROSS : IDC_ARROW));
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case IDM_SHAPE_RECT:
        case IDM_SHAPE_CIRCLE:
        {
            if (!g_pdfDoc) break;
            int newMode = (LOWORD(wParam) == IDM_SHAPE_RECT) ? 1 : 2;
            g_shapeMode = (g_shapeMode == newMode) ? 0 : newMode; // 토글
            if (!g_shapeMode) g_isDraggingShape = false;
            HMENU hMenu = GetMenu(hWnd);
            if (hMenu) {
                CheckMenuItem(hMenu, IDM_SHAPE_RECT,   MF_BYCOMMAND | (g_shapeMode == 1 ? MF_CHECKED : MF_UNCHECKED));
                CheckMenuItem(hMenu, IDM_SHAPE_CIRCLE, MF_BYCOMMAND | (g_shapeMode == 2 ? MF_CHECKED : MF_UNCHECKED));
            }
            SetCursor(LoadCursor(nullptr, g_shapeMode ? IDC_CROSS : IDC_ARROW));
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case IDM_SHAPE_COLOR:
        {
            CHOOSECOLORW cc = {};
            static COLORREF cust[16] = {};
            cc.lStructSize  = sizeof(cc);
            cc.hwndOwner    = hWnd;
            cc.lpCustColors = cust;
            cc.rgbResult    = g_shapeColor;
            cc.Flags        = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc))
                g_shapeColor = cc.rgbResult;
            break;
        }
        case IDM_GROUP_PANEL:
        {
            GroupPanel::Toggle(hWnd);
            HMENU hMenu = GetMenu(hWnd);
            if (hMenu) CheckMenuItem(hMenu, IDM_GROUP_PANEL,
                MF_BYCOMMAND | (GroupPanel::IsVisible() ? MF_CHECKED : MF_UNCHECKED));
            InvalidateRect(hWnd, nullptr, FALSE);
            if (g_pdfDoc) FitToWidth(hWnd);
            break;
        }
        case IDM_GROUP_MANAGE:
            GroupPanel::ShowManageDialog(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case IDM_PAGE_MANAGER: ShowPageManager(hWnd);                                    break;
        case IDM_PREV_PAGE:  NavigatePage(hWnd, g_twoPageView ? -2 : -1);                 break;
        case IDM_NEXT_PAGE:  NavigatePage(hWnd, g_twoPageView ? +2 : +1);                 break;
        case IDM_ZOOM_IN:    ZoomTo(hWnd, std::min(g_zoom * ZOOM_STEP, ZOOM_MAX));      break;
        case IDM_ZOOM_OUT:   ZoomTo(hWnd, std::max(g_zoom / ZOOM_STEP, ZOOM_MIN));      break;
        case IDM_ZOOM_RESET: ZoomTo(hWnd, 1.0f);                                        break;
        case IDM_FIT_WIDTH:  FitToWidth(hWnd);                                          break;
        case IDM_FIT_PAGE:  FitToPage(hWnd);                                            break;
        case IDM_FIND:
            if (g_findBarVisible) HideFindBar(hWnd);
            else                  ShowFindBar(hWnd);
            break;
        case IDM_FIND_NEXT:  FindNext(hWnd, true);                                      break;
        case IDM_FIND_PREV:  FindNext(hWnd, false);                                     break;
        case IDM_QUICKJUMP:  ShowQuickJumpPopup(hWnd);                                  break;
        case IDM_ABOUT:      CleanPopup::ShowAbout(hWnd, IDB_PLAIN_PNG, LS(STR_ABOUT_APPNAME), GetAppVersionString().c_str(), L"\u00A9 2026 Nightsky Works."); break;
        case IDM_LANG_0: case IDM_LANG_1: case IDM_LANG_2:
        case IDM_LANG_3: case IDM_LANG_4: case IDM_LANG_5:
        case IDM_LANG_6: case IDM_LANG_7: case IDM_LANG_8:
        {
            Lang_Set(static_cast<AppLang>(LOWORD(wParam) - IDM_LANG_0));
            UpdateMenuLanguage(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        case IDM_PREFERENCES:
        {
            if (CleanPreferences::Show(hWnd, g_prefs))
            {
                g_selectionColor = g_prefs.selectionColor;
                g_maxRecentFiles = g_prefs.maxRecentFiles;
                SavePrefs();
                UpdateMenuLanguage(hWnd);
                if ((int)g_recentFiles.size() > g_prefs.maxRecentFiles)
                    g_recentFiles.resize(g_prefs.maxRecentFiles);
                UpdateRecentFilesMenu(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }

        case IDM_SET_DEFAULT_PDF:
        {
            if (CleanPopup::Show(hWnd, LS(STR_DEFAULT_PDF_TITLE), LS(STR_DEFAULT_PDF_MSG), true))
            {
                ShellExecuteW(nullptr, L"open",
                    L"ms-settings:defaultapps",
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }

        case IDM_LICENSE:
        {
            static const std::wstring licBodyText =
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Ghostscript\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"PDF compression functionality is provided using Ghostscript.\r\n"
                L"\r\n"
                L"This application includes Ghostscript 10.08.0.\r\n"
                L"\r\n"
                L"Ghostscript is Copyright (C) Artifex Software, Inc. and others.\r\n"
                L"\r\n"
                L"Ghostscript 10.08.0 Community Edition is licensed under the GNU Affero General Public License version 3 (AGPL-3.0).\r\n"
                L"\r\n"
                L"Project website:\r\n"
                L"https://ghostscript.com/\r\n"
                L"\r\n"
                L"Corresponding source code for Ghostscript 10.08.0:\r\n"
                L"https://github.com/ArtifexSoftware/ghostpdl-downloads/releases/tag/gs10080\r\n"
                L"\r\n"
                L"A copy of the GNU Affero General Public License v3.0 is available at:\r\n"
                L"https://www.gnu.org/licenses/agpl-3.0.html\r\n"
                L"\r\n"
                L"The corresponding source code for the Ghostscript 10.08.0 binaries distributed with this application is available from the source code link above.\r\n"
                L"\r\n"
                L"\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"PDFium\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Copyright 2014 The PDFium Authors\r\n"
                L"BSD 3-Clause License\r\n"
                L"\r\n"
                L"Redistribution and use in source and binary forms, with or without\r\n"
                L"modification, are permitted provided that the following conditions are met:\r\n"
                L"\r\n"
                L"1. Redistributions of source code must retain the above copyright notice,\r\n"
                L"   this list of conditions and the following disclaimer.\r\n"
                L"2. Redistributions in binary form must reproduce the above copyright notice,\r\n"
                L"   this list of conditions and the following disclaimer in the documentation\r\n"
                L"   and/or other materials provided with the distribution.\r\n"
                L"3. Neither the name of the copyright holder nor the names of its contributors\r\n"
                L"   may be used to endorse or promote products derived from this software\r\n"
                L"   without specific prior written permission.\r\n"
                L"\r\n"
                L"THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\r\n"
                L"AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\r\n"
                L"IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE\r\n"
                L"ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE\r\n"
                L"LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR\r\n"
                L"CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF\r\n"
                L"SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS\r\n"
                L"INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN\r\n"
                L"CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)\r\n"
                L"ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE\r\n"
                L"POSSIBILITY OF SUCH DAMAGE.\r\n"
                L"\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"FreeType\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Copyright (C) 1996-2023 David Turner, Robert Wilhelm, and Werner Lemberg.\r\n"
                L"FreeType License (FTL) \u2014 BSD-style license.\r\n"
                L"https://freetype.org/license.html\r\n"
                L"\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"ICU (International Components for Unicode)\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Copyright (C) 1991-2023 Unicode, Inc.\r\n"
                L"ICU License \u2014 MIT-style license.\r\n"
                L"https://unicode.org/copyright.html\r\n"
                L"\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"libjpeg-turbo\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Copyright (C) 2009-2023 D. R. Commander and contributors.\r\n"
                L"BSD 3-Clause License / IJG License.\r\n"
                L"https://libjpeg-turbo.org/About/License\r\n"
                L"\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"zlib\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Copyright (C) 1995-2023 Jean-loup Gailly and Mark Adler.\r\n"
                L"zlib License.\r\n"
                L"https://zlib.net/zlib_license.html\r\n"
                L"\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"OpenJPEG\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Copyright (C) 2002-2023 Communications and Remote Sensing Laboratory\r\n"
                L"and contributors.\r\n"
                L"BSD 2-Clause License.\r\n"
                L"https://www.openjpeg.org/\r\n"
                L"\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Fluent UI Emoji\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Copyright (c) Microsoft Corporation.\r\n"
                L"MIT License. Used for the open, save and print toolbar icons.\r\n"
                L"https://github.com/microsoft/fluentui-emoji\r\n"
                L"\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Fluent UI System Icons\r\n"
                L"\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\u2501\r\n"
                L"Copyright (c) 2020 Microsoft Corporation.\r\n"
                L"MIT License. Used for toolbar and user interface icons.\r\n"
                L"https://github.com/microsoft/fluentui-system-icons\r\n";
            std::wstring licText =
                std::wstring(L"Plain PDF Viewer\r\n")
                + L"Copyright (C) 2026 Nightsky Works\r\n\r\n"
                + L"Plain PDF Viewer is free software licensed under the GNU Affero General Public License version 3 (AGPL-3.0).\r\n\r\n"
                + L"Source code:\r\n"
                + L"https://github.com/Nightsky6/PlainPDFViewer\r\n\r\n"
                + L"GNU Affero General Public License v3.0:\r\n"
                + L"https://www.gnu.org/licenses/agpl-3.0.html\r\n\r\n"
                + L"Contact:\r\n"
                + L"nightskyworks@gmail.com\r\n"
                + L"https://github.com/Nightsky6/PlainPDFViewer/issues\r\n\r\n\r\n"
                + licBodyText;
            CleanPopup::ShowLicense(hWnd, IDB_PLAIN_PNG,
                LS(STR_LIC_POPUP_TITLE),
                L"Plain PDF Viewer \u00B7 Third-party Licenses",
                licText);
            break;
        }
        case IDM_EXIT:       DestroyWindow(hWnd);                                        break;
        case IDM_GOTO_PAGE:  ShowGotoPageDialog(hWnd); break;
        case IDM_RECENT_FILE1:
        case IDM_RECENT_FILE2:
        case IDM_RECENT_FILE3:
        case IDM_RECENT_FILE4:
        case IDM_RECENT_FILE5:
        case IDM_RECENT_FILE6:
        case IDM_RECENT_FILE7:
        case IDM_RECENT_FILE8:
        case IDM_RECENT_FILE9:
        case IDM_RECENT_FILE10:
        {
            static const int kIds[10] = {
                IDM_RECENT_FILE1, IDM_RECENT_FILE2, IDM_RECENT_FILE3, IDM_RECENT_FILE4, IDM_RECENT_FILE5,
                IDM_RECENT_FILE6, IDM_RECENT_FILE7, IDM_RECENT_FILE8, IDM_RECENT_FILE9, IDM_RECENT_FILE10 };
            int cmd = LOWORD(wParam);
            int idx = -1;
            for (int i = 0; i < 10; i++) if (kIds[i] == cmd) { idx = i; break; }
            if (idx >= 0 && idx < (int)g_recentFiles.size())
            {
                const std::wstring path = g_recentFiles[idx];
                if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    // 파일이 존재하지 않으면 목록에서 제거
                    g_recentFiles.erase(g_recentFiles.begin() + idx);
                    SaveRecentFiles();
                    UpdateRecentFilesMenu(hWnd);
                    MessageBoxW(hWnd,
                        (LS(STR_FILE_NOT_FOUND) + path).c_str(),
                        LS(STR_FILE_NOT_FOUND_TITLE), MB_OK | MB_ICONWARNING);
                }
                else
                {
                    OpenFileFromPath(hWnd, path, true);
                }
            }
            break;
        }
        case IDM_HAND_TOOL:
            g_handMode = !g_handMode;
            g_isPanning = false;
            InvalidateToolbarBaseCache();
            InvalidateRect(hWnd, nullptr, TRUE);
            break;
        case IDM_TOGGLE_SIDEBAR:
        {
            bool close = g_sidebarVisible && g_sidebarTab == TAB_BOOKMARKS;
            SidebarToggleAnimated(hWnd, close, TAB_BOOKMARKS);
            break;
        }
        case IDM_TOGGLE_THUMBNAIL:
        {
            bool close = g_sidebarVisible && g_sidebarTab == TAB_THUMBNAILS;
            SidebarToggleAnimated(hWnd, close, TAB_THUMBNAILS);
            break;
        }
        case IDM_ROTATE_CW:
            if (g_pdfDoc) RotateAllKeepPosition(hWnd, 1);
            break;
        case IDM_ROTATE_CCW:
            if (g_pdfDoc) RotateAllKeepPosition(hWnd, 3);
            break;
        case IDM_DARK_MODE:
            g_darkMode = !g_darkMode;
            InitPageCache(g_pageCount);
            InvalidateToolbarBaseCache();
            InvalidateRect(hWnd, nullptr, TRUE);
            break;
        case IDM_TOGGLE_TOOLBAR:
            g_toolbarHidden = !g_toolbarHidden;
            CheckMenuItem(g_hMainMenu, IDM_TOGGLE_TOOLBAR,
                g_toolbarHidden ? MF_CHECKED : MF_UNCHECKED);
            if (g_toolbarHidden && g_hQJPopup) QJPopupClose();
            InvalidateToolbarBaseCache();
            RepositionFindPanel(hWnd);
            UpdateScrollBars(hWnd);
            g_pageBaseDirty = true;
            InvalidateRect(hWnd, nullptr, TRUE);
            break;
        case IDM_TWO_PAGE:
            g_twoPageView = !g_twoPageView;
            CheckMenuItem(g_hMainMenu, IDM_TWO_PAGE,
                g_twoPageView ? MF_CHECKED : MF_UNCHECKED);
            ClearPageCache();
            ClearThumbCache();
            InitThumbCache(g_pageCount);
            RecalcPageLayouts();
            FitToWidth(hWnd);   // 전환 즉시 창 폭에 맞게 줌 조정
            UpdateScrollBars(hWnd);
            InvalidateStatusBarCache();
            InvalidateRect(hWnd, nullptr, TRUE);
            break;
        case IDM_SCREEN_LOCK:
            g_screenLock = !g_screenLock;
            InvalidateToolbarBaseCache();
            { RECT cr; GetClientRect(hWnd, &cr);
              RECT r = GetScreenLockToolbarBtnRect(cr); InflateRect(&r, 2, 2);
              InvalidateRect(hWnd, &r, FALSE); }
            break;
        case IDM_SPOTLIGHT:
            g_spotlightMode = !g_spotlightMode;
            if (!g_spotlightMode)
            {
                g_hasSpotlight = false;
                g_isDrawingSpotlight = false;
            }
            else
            {
                // 레이저 포인터와 동시 활성화 방지
                g_laserMode = false;
                g_laserPos = { -1, -1 };
            }
            InvalidateToolbarBaseCache();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case IDM_LASER_POINTER:
            g_laserMode = !g_laserMode;
            if (!g_laserMode)
                g_laserPos = { -1, -1 };
            else
            {
                // 스포트라이트와 동시 활성화 방지
                g_spotlightMode = false;
                g_hasSpotlight = false;
                g_isDrawingSpotlight = false;
            }
            InvalidateToolbarBaseCache();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case IDM_NEW_TAB:
            AddNewTab(hWnd);
            break;
        case IDM_CLOSE_TAB:
            CloseTab(hWnd, g_activeTab);
            break;
        case IDM_NEXT_TAB:
            if (g_tabs.size() > 1)
                SwitchToTab(hWnd, (g_activeTab + 1) % (int)g_tabs.size());
            break;
        case IDM_PREV_TAB:
            if (g_tabs.size() > 1)
                SwitchToTab(hWnd, (g_activeTab - 1 + (int)g_tabs.size()) % (int)g_tabs.size());
            break;
        default:             return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;

    case WM_DROPFILES:
    {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        WCHAR szFile[MAX_PATH] = {};
        if (DragQueryFileW(hDrop, 0, szFile, MAX_PATH))
            OpenFileFromPath(hWnd, szFile, true);
        DragFinish(hDrop);
    }
    break;

    case WM_MEASUREITEM:
    {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        // 컨텍스트 메뉴 color owner-draw 아이템 크기 지정
        if (mis->CtlType == ODT_MENU)
        {
            mis->itemWidth  = 130;
            mis->itemHeight = 22;
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlType != ODT_MENU || dis->itemData == 0) break;

        auto* item = reinterpret_cast<ColorMenuItem*>(dis->itemData);
        HDC hdc = dis->hDC;
        RECT rc  = dis->rcItem;

        // 배경
        bool selected = (dis->itemState & ODS_SELECTED) != 0;
        bool grayed   = (dis->itemState & ODS_GRAYED)   != 0;
        FillRect(hdc, &rc, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU));

        // 색상 네모 (왼쪽)
        RECT clrRc = { rc.left + 6, rc.top + 3, rc.left + 20, rc.bottom - 3 };
        COLORREF clr = grayed ? RGB(180, 180, 180) : item->color;
        HBRUSH hBr = CreateSolidBrush(clr);
        HPEN   hPen = CreatePen(PS_SOLID, 1, grayed ? RGB(160,160,160) : RGB(80, 80, 80));
        HPEN   oPen = static_cast<HPEN>(SelectObject(hdc, hPen));
        HBRUSH oBr  = static_cast<HBRUSH>(SelectObject(hdc, hBr));
        Rectangle(hdc, clrRc.left, clrRc.top, clrRc.right, clrRc.bottom);
        SelectObject(hdc, oPen); SelectObject(hdc, oBr);
        DeleteObject(hBr); DeleteObject(hPen);

        // 텍스트
        RECT txtRc = { rc.left + 26, rc.top, rc.right - 4, rc.bottom };
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, grayed ? GetSysColor(COLOR_GRAYTEXT)
                                 : GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT));
        DrawTextW(hdc, item->label.c_str(), -1, &txtRc, DT_VCENTER | DT_SINGLELINE | DT_LEFT);
        return TRUE;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT cr;
        GetClientRect(hWnd, &cr);
        int cw = cr.right, ch = cr.bottom;

        // 더블 버퍼링 — 크기가 바뀔 때만 재생성
        if (!g_backDC || cw != g_backW || ch != g_backH)
        {
            // 리사이즈 드래그 중이고 이전 캐시가 있으면 StretchBlt로 빠르게 표시
            if (g_isSizing && g_backDC && g_backW > 0 && g_backH > 0)
            {
                HDC tmpDC  = CreateCompatibleDC(hdc);
                HBITMAP tmpBmp = CreateCompatibleBitmap(hdc, cw, ch);
                HBITMAP tmpOld = (HBITMAP)SelectObject(tmpDC, tmpBmp);
                SetStretchBltMode(tmpDC, HALFTONE);
                StretchBlt(tmpDC, 0, 0, cw, ch, g_backDC, 0, 0, g_backW, g_backH, SRCCOPY);
                BitBlt(hdc, 0, 0, cw, ch, tmpDC, 0, 0, SRCCOPY);
                SelectObject(tmpDC, tmpOld);
                DeleteObject(tmpBmp);
                DeleteDC(tmpDC);
                EndPaint(hWnd, &ps);
                break;
            }
            if (g_backDC)
            {
                DeleteDC(g_backDC);
                DeleteObject(g_backBmp);
            }
            g_backDC  = CreateCompatibleDC(hdc);
            g_backBmp = CreateCompatibleBitmap(hdc, cw, ch);
            g_backOld = static_cast<HBITMAP>(SelectObject(g_backDC, g_backBmp));
            g_backW   = cw;
            g_backH   = ch;
        }

        // 로딩 중이면 로딩 화면만 그리고 종료
        if (g_isLoading)
        {
            DrawLoadingOverlay(g_backDC, cr);
            BitBlt(hdc, 0, 0, cw, ch, g_backDC, 0, 0, SRCCOPY);
            EndPaint(hWnd, &ps);
            break;
        }

        // rcPaint 영역이 툴바 내부에만 걸려 있으면 PDF 영역 재렌더 생략
        // (슬라이드쇼 중에는 툴바를 그리지 않으므로 항상 전체 렌더)
        const bool toolbarOnlyRepaint =
            !g_slideshowActive &&
            ps.rcPaint.top    >= 0 &&
            ps.rcPaint.bottom <= TOOLBAR_HEIGHT;

        if (!toolbarOnlyRepaint)
            RenderPage(hWnd, g_backDC, cr);
        else
        {
            // 툴바 영역만 변경된 경우: 백버퍼에 툴바만 다시 그린다
            DrawToolbar(g_backDC, cr);
        }

        // 변경된 영역(ps.rcPaint)만 화면에 복사 — 전체 BitBlt 대신
        BitBlt(hdc,
            ps.rcPaint.left, ps.rcPaint.top,
            ps.rcPaint.right  - ps.rcPaint.left,
            ps.rcPaint.bottom - ps.rcPaint.top,
            g_backDC,
            ps.rcPaint.left, ps.rcPaint.top,
            SRCCOPY);

        // 레이저 포인터는 백버퍼 위에 화면 DC에 직접 합성
        if (g_laserMode && g_laserPos.x >= 0 && g_laserPos.y >= TOOLBAR_HEIGHT)
        {
            // 레이저 영역이 이번 paint 영역과 겹칠 때만 그림
            RECT laserRc = { g_laserPos.x - 14, g_laserPos.y - 14,
                             g_laserPos.x + 14, g_laserPos.y + 14 };
            RECT paintRc = ps.rcPaint;
            RECT isect;
            if (IntersectRect(&isect, &laserRc, &paintRc))
                DrawLaserOverlay(hdc, g_laserPos.x, g_laserPos.y);
        }

        EndPaint(hWnd, &ps);
    }
    break;

    case WM_ENABLE:
        // PageManager 등 모달 창이 EnableWindow(FALSE/TRUE)를 호출할 때
        // 시스템이 메인 창을 강제 재렌더해 깜빡임이 생기는 것을 막는다.
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT:
    {
        // 텍스트 박스 인라인 Edit 컨트롤 — 노란 배경
        if (g_hTextBoxEdit && (HWND)lParam == g_hTextBoxEdit)
        {
            HDC hdcEdit = (HDC)wParam;
            static HBRUSH hTBBrush = nullptr;
            if (!hTBBrush) hTBBrush = CreateSolidBrush(RGB(255, 255, 180));
            SetTextColor(hdcEdit, RGB(30, 30, 30));
            SetBkColor(hdcEdit, RGB(255, 255, 180));
            return (LRESULT)hTBBrush;
        }
        break;
    }

    case WM_ENTERSIZEMOVE:
        g_isSizing = true;
    break;

    case WM_SIZE:
        RepositionFindPanel(hWnd);
        UpdateScrollBars(hWnd);
        // 크기 변경 중에는 백버퍼만 무효화 — 캐시는 드래그 완료(WM_EXITSIZEMOVE) 후 정리
        g_backW = g_backH = 0;
        InvalidateRect(hWnd, nullptr, FALSE);
    break;

    case WM_EXITSIZEMOVE:
        g_isSizing = false;
        // 창 크기 조정이 완전히 끝난 시점에 캐시 무효화 및 레이아웃 재계산
        // (드래그 중에는 캐시를 유지해 버벅임 방지)
        if (g_pdfDoc)
        {
            RecalcPageLayouts();
            InitPageCache(g_pageCount);
            InitThumbCache(g_pageCount);
            UpdateScrollBars(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            SchedulePrefetch(hWnd);
        }
    break;

    case WM_HSCROLL: HandleScroll(hWnd, SB_HORZ, wParam); break;
    case WM_VSCROLL: HandleScroll(hWnd, SB_VERT, wParam); break;

    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        // 탭 파일명 툴팁 처리 (break 전에 항상 실행)
        if (g_hTabTooltip && g_tabs.size() >= 1)
        {
            bool closeBtn = false;
            int hit = HitTestTabBar(x, y, &closeBtn);
            int tipIdx = (hit >= 0 && !closeBtn) ? hit : -1;

            if (tipIdx >= 0 && tipIdx < (int)g_tabs.size())
            {
                std::wstring newTip = g_tabs[tipIdx].filePath.empty()
                    ? GetTabTitle(g_tabs[tipIdx])
                    : g_tabs[tipIdx].filePath;

                if (newTip != g_tabTipText || tipIdx != g_tabTooltipIdx)
                {
                    g_tabTipText     = newTip;
                    g_tabTooltipIdx  = tipIdx;
                    // 크기 계산
                    HDC hdc = GetDC(g_hTabTooltip);
                    Gdiplus::Graphics gfx(hdc);
                    Gdiplus::FontFamily ff(L"Segoe UI");
                    Gdiplus::Font font(&ff, 9.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                    Gdiplus::RectF layout(0, 0, 700, 100), bound;
                    gfx.MeasureString(g_tabTipText.c_str(), -1, &font, layout, &bound);
                    ReleaseDC(g_hTabTooltip, hdc);
                    int padX = 16, padY = 8;
                    int tipW = (int)bound.Width  + padX * 2;
                    int tipH = (int)bound.Height + padY * 2;
                    POINT pt; GetCursorPos(&pt);
                    SetWindowPos(g_hTabTooltip, HWND_TOPMOST,
                        pt.x + 12, pt.y + 20, tipW, tipH,
                        SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    HRGN hRgn = CreateRoundRectRgn(0, 0, tipW + 1, tipH + 1, 12, 12);
                    SetWindowRgn(g_hTabTooltip, hRgn, FALSE);
                    InvalidateRect(g_hTabTooltip, nullptr, TRUE);
                }
                else
                {
                    // 위치만 갱신
                    POINT pt; GetCursorPos(&pt);
                    RECT wr; GetWindowRect(g_hTabTooltip, &wr);
                    int tipW = wr.right - wr.left, tipH = wr.bottom - wr.top;
                    SetWindowPos(g_hTabTooltip, HWND_TOPMOST,
                        pt.x + 12, pt.y + 20, tipW, tipH,
                        SWP_NOACTIVATE | SWP_NOSIZE | SWP_SHOWWINDOW);
                }
            }
            else if (g_tabTooltipIdx >= 0)
            {
                g_tabTooltipIdx = -1;
                g_tabTipText.clear();
                ShowWindow(g_hTabTooltip, SW_HIDE);
            }
        }

        // 슬라이드쇼 화살표 hover 처리
        if (g_slideshowActive)
        {
            RECT cr0; GetClientRect(hWnd, &cr0);
            POINT pt = { x, y };
            RECT lr = GetSSArrowRect(cr0, true);
            RECT rr = GetSSArrowRect(cr0, false);
            RECT xr = GetSSCloseRect(cr0);
            bool hl = (PtInRect(&lr, pt) != 0);
            bool hr = (PtInRect(&rr, pt) != 0);
            bool hx = (PtInRect(&xr, pt) != 0);
            if (hl != g_ssArrowHoverLeft || hr != g_ssArrowHoverRight || hx != g_ssCloseHover)
            {
                g_ssArrowHoverLeft  = hl;
                g_ssArrowHoverRight = hr;
                g_ssCloseHover      = hx;
                InvalidateRect(hWnd, &lr, FALSE);
                InvalidateRect(hWnd, &rr, FALSE);
                InvalidateRect(hWnd, &xr, FALSE);
            }
        }

        // Redact 드래그 업데이트
        if (g_isDraggingRedact)
        {
            g_redactDragCurX = x;
            g_redactDragCurY = y;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        if (g_redactMode)
            break; // redact 모드에서는 다른 마우스 이동 처리 차단

        // 잉크 드로잉 업데이트
        if (g_isDrawingInk)
        {
            g_inkStroke.push_back({ x, y });
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // 도형 드래그 업데이트
        if (g_isDraggingShape)
        {
            g_shapeDragCurX = x;
            g_shapeDragCurY = y;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        if (g_shapeMode != 0)
        {
            SetCursor(LoadCursor(nullptr, IDC_CROSS));
            break;
        }

        // 그룹 패널 마우스 이동
        if (GroupPanel::IsVisible()) {
            RECT cr; GetClientRect(hWnd, &cr);
            GroupPanel::OnMouseMove(hWnd, x, y, cr);
        }

        // 스포트라이트 드래그 업데이트
        if (g_isDrawingSpotlight)
        {
            g_spotlightRect.left   = std::min((LONG)g_spotlightStart.x, (LONG)x);
            g_spotlightRect.top    = std::min((LONG)g_spotlightStart.y, (LONG)y);
            g_spotlightRect.right  = std::max((LONG)g_spotlightStart.x, (LONG)x);
            g_spotlightRect.bottom = std::max((LONG)g_spotlightStart.y, (LONG)y);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        if (g_isDraggingNote && g_draggingNoteIdx >= 0)
        {
            RECT cr0; GetClientRect(hWnd, &cr0);
            const RECT vp0 = GetViewportRect(cr0);
            auto& note = g_notes[g_draggingNoteIdx];
            int pgLeft, pgTop;
            GetPageScreenPos(note.pageIndex, vp0, &pgLeft, &pgTop);
            const double pageW = g_pageLayouts[note.pageIndex].pdfW;
            const double pageH = g_pageLayouts[note.pageIndex].pdfH;
            int newSx = x - g_noteDragOffsetX;
            int newSy = y - g_noteDragOffsetY;
            double newPdfX, newPdfY;
            ScreenToPdf(newSx, newSy, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(note.pageIndex), &newPdfX, &newPdfY);
            note.pdfX = newPdfX;
            note.pdfY = newPdfY;
            g_dirty = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // 텍스트 박스 드래그 중
        if (g_isDraggingTextBox && g_selectedTextBox >= 0)
        {
            RECT crtb; GetClientRect(hWnd, &crtb);
            const RECT vptb = GetViewportRect(crtb);
            auto& tb = g_textBoxes[g_selectedTextBox];
            if (tb.pageIndex >= 0 && tb.pageIndex < g_pageCount)
            {
                int pgLeft, pgTop;
                GetPageScreenPos(tb.pageIndex, vptb, &pgLeft, &pgTop);
                const double pageW = g_pageLayouts[tb.pageIndex].pdfW;
                const double pageH = g_pageLayouts[tb.pageIndex].pdfH;
                int newSx = x - g_tbDragOffsetX;
                int newSy = y - g_tbDragOffsetY;
                double newPdfX, newPdfY;
                ScreenToPdf(newSx, newSy, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(tb.pageIndex), &newPdfX, &newPdfY);
                tb.pdfX = newPdfX;
                tb.pdfY = newPdfY;
                g_dirty = true;
                // 편집 컨트롤이 열려 있으면 위치 동기화
                if (g_hTextBoxEdit && g_editingTextBox == g_selectedTextBox)
                {
                    const int B = 2;
                    RECT nr = TextBoxScreenRect(tb);
                    SetWindowPos(g_hTextBoxEdit, nullptr, nr.left + B, nr.top + B,
                        (nr.right - nr.left) - B * 2, (nr.bottom - nr.top) - B * 2,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT;
                InvalidateRect(hWnd, &ir, FALSE);
            }
            break;
        }

        // 텍스트 박스 리사이즈 중
        if (g_isResizingTextBox && g_selectedTextBox >= 0)
        {
            RECT crtb; GetClientRect(hWnd, &crtb);
            const RECT vptb = GetViewportRect(crtb);
            auto& tb = g_textBoxes[g_selectedTextBox];
            if (tb.pageIndex >= 0 && tb.pageIndex < g_pageCount)
            {
                int pgLeft, pgTop;
                GetPageScreenPos(tb.pageIndex, vptb, &pgLeft, &pgTop);
                const double pageW = g_pageLayouts[tb.pageIndex].pdfW;
                const double pageH = g_pageLayouts[tb.pageIndex].pdfH;

                // 마우스 델타 → PDF 좌표 델타
                double ax0, ay0, ax1, ay1;
                ScreenToPdf(g_tbResizeInitMouseX,          g_tbResizeInitMouseY,          pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(tb.pageIndex), &ax0, &ay0);
                ScreenToPdf(g_tbResizeInitMouseX + (x - g_tbResizeInitMouseX), g_tbResizeInitMouseY + (y - g_tbResizeInitMouseY), pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(tb.pageIndex), &ax1, &ay1);
                double dx = ax1 - ax0;
                double dy = ay1 - ay0; // PDF Y는 위가 +이므로 dy 부호 주의

                // 초기 rect
                double lx = g_tbResizeInitX;
                double ty = g_tbResizeInitY;
                double rx = lx + g_tbResizeInitW;
                double by = ty - g_tbResizeInitH; // 좌하단 Y

                int h = g_resizeHandle;
                // 왼쪽 가장자리 이동
                if (h == 0 || h == 3 || h == 5) lx += dx;
                // 오른쪽 가장자리 이동
                if (h == 2 || h == 4 || h == 7) rx += dx;
                // 위쪽 가장자리 이동 (PDF Y 위가 +, 화면 위가 -, 그래서 dy 반전)
                if (h == 0 || h == 1 || h == 2) ty += dy;
                // 아래쪽 가장자리 이동
                if (h == 5 || h == 6 || h == 7) by += dy;

                // 최소 크기 보장 (PDF 단위 10pt)
                const double MIN_PDF = 10.0;
                if (rx - lx < MIN_PDF) { if (h==0||h==3||h==5) lx = rx - MIN_PDF; else rx = lx + MIN_PDF; }
                if (ty - by < MIN_PDF) { if (h==0||h==1||h==2) ty = by + MIN_PDF; else by = ty - MIN_PDF; }

                tb.pdfX = lx;
                tb.pdfY = ty;
                tb.pdfW = rx - lx;
                tb.pdfH = ty - by;
                g_dirty = true;
                // 편집 컨트롤이 열려 있으면 크기/위치 동기화
                if (g_hTextBoxEdit && g_editingTextBox == g_selectedTextBox)
                {
                    const int B = 2;
                    RECT nr = TextBoxScreenRect(tb);
                    SetWindowPos(g_hTextBoxEdit, nullptr, nr.left + B, nr.top + B,
                        (nr.right - nr.left) - B * 2, (nr.bottom - nr.top) - B * 2,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT;
                InvalidateRect(hWnd, &ir, FALSE);
            }
            SetCursor(LoadCursor(nullptr, HandleCursor(g_resizeHandle)));
            break;
        }

        // 서명 드래그 중
        if (g_isDraggingSig && g_selectedSig >= 0)
        {
            RECT cr_s; GetClientRect(hWnd, &cr_s);
            const RECT vp_s = GetViewportRect(cr_s);
            auto& sig = g_signatures[g_selectedSig];
            if (sig.pageIndex >= 0 && sig.pageIndex < g_pageCount)
            {
                int pgLeft, pgTop;
                GetPageScreenPos(sig.pageIndex, vp_s, &pgLeft, &pgTop);
                const double pageW = g_pageLayouts[sig.pageIndex].pdfW;
                const double pageH = g_pageLayouts[sig.pageIndex].pdfH;
                int newSx = x - g_sigDragOffsetX;
                int newSy = y - g_sigDragOffsetY;
                double newPdfX, newPdfY;
                ScreenToPdf(newSx, newSy, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(sig.pageIndex), &newPdfX, &newPdfY);
                double dx = newPdfX - sig.pdfX;
                double dy = newPdfY - sig.pdfY;
                sig.pdfX = newPdfX;
                sig.pdfY = newPdfY;
                // 획 좌표도 이동
                for (auto& stroke : sig.strokes)
                    for (auto& p : stroke) { p.x += (float)dx; p.y += (float)dy; }
                // 잉크 서명만 비트맵 캐시 무효화 (이미지 서명의 bitmap은 원본)
                if (!sig.isImage && sig.bitmap) { delete sig.bitmap; sig.bitmap = nullptr; }
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }

        // 서명 리사이즈 중
        if (g_isResizingSig && g_selectedSig >= 0)
        {
            RECT cr_s; GetClientRect(hWnd, &cr_s);
            const RECT vp_s = GetViewportRect(cr_s);
            auto& sig = g_signatures[g_selectedSig];
            if (sig.pageIndex >= 0 && sig.pageIndex < g_pageCount)
            {
                int pgLeft, pgTop;
                GetPageScreenPos(sig.pageIndex, vp_s, &pgLeft, &pgTop);
                const double pageW = g_pageLayouts[sig.pageIndex].pdfW;
                const double pageH = g_pageLayouts[sig.pageIndex].pdfH;
                double ax0, ay0, ax1, ay1;
                ScreenToPdf(g_sigResizeMouseX, g_sigResizeMouseY, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(sig.pageIndex), &ax0, &ay0);
                ScreenToPdf(x, y, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(sig.pageIndex), &ax1, &ay1);
                double dx = ax1 - ax0, dy = ay1 - ay0;
                double lx = g_sigResizeInitX, ty = g_sigResizeInitY;
                double rx = lx + g_sigResizeInitW, by = ty - g_sigResizeInitH;
                int h = g_sigResizeHandle;
                if (h==0||h==3||h==5) lx += dx;
                if (h==2||h==4||h==7) rx += dx;
                if (h==0||h==1||h==2) ty += dy;
                if (h==5||h==6||h==7) by += dy;
                const double MIN_PDF = 10.0;
                if (rx - lx < MIN_PDF) { if (h==0||h==3||h==5) lx = rx - MIN_PDF; else rx = lx + MIN_PDF; }
                if (ty - by < MIN_PDF) { if (h==0||h==1||h==2) ty = by + MIN_PDF; else by = ty - MIN_PDF; }
                sig.pdfX = lx; sig.pdfY = ty;
                sig.pdfW = rx - lx; sig.pdfH = ty - by;
                // 획 좌표 스케일링: 매 프레임 초기 스냅샷 기준으로 계산 (누적 오차 방지)
                if (g_sigResizeInitW > 0 && g_sigResizeInitH > 0 &&
                    !g_sigResizeInitStrokes.empty())
                {
                    double sx = sig.pdfW / g_sigResizeInitW;
                    double sy = sig.pdfH / g_sigResizeInitH;
                    sig.strokes = g_sigResizeInitStrokes; // 원본 복원 후 스케일
                    for (auto& stroke : sig.strokes)
                        for (auto& p : stroke) {
                            p.x = (float)(lx + (p.x - g_sigResizeInitX) * sx);
                            p.y = (float)(ty + (p.y - g_sigResizeInitY) * sy);
                        }
                }
                // 잉크 서명만 비트맵 캐시 무효화 (이미지 서명의 bitmap은 원본)
                if (!sig.isImage && sig.bitmap) { delete sig.bitmap; sig.bitmap = nullptr; }
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            SetCursor(LoadCursor(nullptr, HandleCursor(g_sigResizeHandle)));
            break;
        }
        if (g_tabs.size() >= 1)
        {
            bool closeBtn = false;
            int prevHover = g_tabHover;
            int prevClose = g_tabCloseHover;
            bool prevPlus = g_tabPlusHover;
            int hit = HitTestTabBar(x, y, &closeBtn);
            g_tabPlusHover = (hit == -2);
            g_tabHover = (hit >= 0) ? hit : -1;
            g_tabCloseHover = closeBtn ? g_tabHover : -1;
            if (g_tabHover != prevHover || g_tabCloseHover != prevClose || g_tabPlusHover != prevPlus)
                InvalidateRect(hWnd, nullptr, FALSE);
        }

        // 텍스트 선택 드래그
        if (g_isSelecting && g_pdfPage && g_textPage)
        {
            // 드래그 임계값을 넘으면 그때 캡처 시작
            if (!g_selCaptured)
            {
                int dx = abs(x - g_selDragStart.x);
                int dy = abs(y - g_selDragStart.y);
                int threshold = GetSystemMetrics(SM_CXDRAG);
                if (dx < threshold && dy < threshold)
                    break; // 아직 임계값 미달 → 캡처 안 함
                SetCapture(hWnd);
                g_selCaptured = true;
            }

            RECT cr; GetClientRect(hWnd, &cr);
            const RECT vp = GetViewportRect(cr);

            // 두쪽 보기에서 마우스가 다른 페이지 위에 있으면 그 페이지 기준으로 계산
            double pdfX2, pdfY2;
            int dragPage = HitTestPageAtPoint(x, y, vp, &pdfX2, &pdfY2);
            if (dragPage >= 0 && dragPage != g_selPage)
            {
                // 선택 시작 페이지와 다른 페이지: 선택 범위를 끝까지 확장
                // (현재는 단일 페이지 선택만 지원하므로 selPage 페이지 끝/처음으로 고정)
                int totalChars = FPDFText_CountChars(g_textPage);
                if (dragPage > g_selPage)
                    g_selCharEnd = totalChars - 1;
                else
                    g_selCharEnd = 0;
                InvalidateRect(hWnd, &vp, FALSE);
                break;
            }

            int pgLeft, pgTop;
            GetPageScreenPos(g_selPage, vp, &pgLeft, &pgTop);

            // 이전 처리 좌표와 2픽셀 이내면 스킵 (FPDFText_GetCharIndexAtPos 호출 절감)
            if (abs(x - g_selLastMouse.x) < 2 && abs(y - g_selLastMouse.y) < 2)
                break;
            g_selLastMouse = { x, y };

            double pdfX = (x - pgLeft) / (double)g_zoom;
            double pdfY = g_pageLayouts[g_selPage].pdfH - (y - pgTop) / (double)g_zoom;
            double tol = 10.0 / g_zoom;
            int charIdx = FPDFText_GetCharIndexAtPos(g_textPage, pdfX, pdfY, tol, tol);
            if (charIdx >= 0 && charIdx != g_selCharEnd)
            {
                g_selCharEnd = charIdx;
                InvalidateRect(hWnd, &vp, FALSE);
            }
            break;
        }

        // 폼 필드 마우스 이동 전달 (커서 모양 변경 등)
        if (g_formHandle && g_pdfDoc && g_pdfPage && !g_isPanning && !g_isSelecting
            && !g_redactMode && !g_inkMode)
        {
            RECT fCr; GetClientRect(hWnd, &fCr);
            const RECT fVp = GetViewportRect(fCr);
            int pgL, pgT;
            GetPageScreenPos(g_currentPage, fVp, &pgL, &pgT);
            const auto& fLayout = g_pageLayouts[g_currentPage];
            int pgX = x - pgL;
            int pgY = y - pgT;
            if (pgX >= 0 && pgX < fLayout.renderW && pgY >= 0 && pgY < fLayout.renderH)
            {
                double pdfX = 0, pdfY = 0;
                FormDeviceToPage(pgX, pgY, fLayout.renderW, fLayout.renderH, &pdfX, &pdfY);
                FORM_OnMouseMove(g_formHandle, g_pdfPage, 0, pdfX, pdfY);
            }
        }

        // 패닝 드래그 처리
        if (g_isPanning && g_pdfDoc)
        {
            int dx = g_panStart.x - x;
            int dy = g_panStart.y - y;

            RECT cr;
            GetClientRect(hWnd, &cr);
            const RECT vp = GetViewportRect(cr);
            const int vpW = vp.right - vp.left;
            const int vpH = vp.bottom - vp.top;

            g_scrollX = std::clamp(g_panStartX + dx, 0, std::max(0, g_maxPageWidth - vpW));
            g_scrollY = std::clamp(g_panStartY + dy, 0, std::max(0, g_totalDocHeight - vpH));
            g_scrollYf = (float)g_scrollY;
            g_scrollTargetY = g_scrollYf;
            UpdateActivePageFromScroll(hWnd);
            UpdateScrollBars(hWnd);
            g_pageBaseDirty = true;
            { RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT; InvalidateRect(hWnd, &ir, TRUE); }
            break;
        }

        // 툴바 hover 효과 (슬라이드쇼 중에는 차단)
        if (!g_slideshowActive)
        {
            RECT cr_hv; GetClientRect(hWnd, &cr_hv);
            RECT findBtn   = GetFindToolbarBtnRect(cr_hv);
            RECT darkBtn   = GetDarkModeToolbarBtnRect(cr_hv);
            RECT lockBtn   = GetScreenLockToolbarBtnRect(cr_hv);
            RECT spotBtn   = GetSpotlightToolbarBtnRect(cr_hv);
            RECT laserBtn2 = GetLaserPointerToolbarBtnRect(cr_hv);
            RECT qjBtn     = GetQuickJumpToolbarBtnRect(cr_hv);
            POINT pt_hv = { x, y };
            int hit = PtInRect(&findBtn, pt_hv) ? 9999
                    : PtInRect(&darkBtn, pt_hv) ? 9998
                    : PtInRect(&lockBtn, pt_hv) ? 9997
                    : PtInRect(&spotBtn, pt_hv) ? 9996
                    : PtInRect(&laserBtn2, pt_hv) ? 9995
                    : PtInRect(&qjBtn, pt_hv)   ? 9994
                    : HitTestToolbar(x, y);
            if (hit != g_hoverButton)
            {
                int prevHover = g_hoverButton;
                g_hoverButton = hit;
                auto invalidateBtn = [&](int idx) {
                    if (idx == 9999) {
                        RECT r = GetFindToolbarBtnRect(cr_hv);
                        InflateRect(&r, 2, 2);
                        InvalidateRect(hWnd, &r, FALSE);
                    } else if (idx == 9998) {
                        RECT r = GetDarkModeToolbarBtnRect(cr_hv);
                        InflateRect(&r, 2, 2);
                        InvalidateRect(hWnd, &r, FALSE);
                    } else if (idx == 9997) {
                        RECT r = GetScreenLockToolbarBtnRect(cr_hv);
                        InflateRect(&r, 2, 2);
                        InvalidateRect(hWnd, &r, FALSE);
                    } else if (idx == 9996) {
                        RECT r = GetSpotlightToolbarBtnRect(cr_hv);
                        InflateRect(&r, 2, 2);
                        InvalidateRect(hWnd, &r, FALSE);
                    } else if (idx == 9995) {
                        RECT r = GetLaserPointerToolbarBtnRect(cr_hv);
                        InflateRect(&r, 2, 2);
                        InvalidateRect(hWnd, &r, FALSE);
                    } else if (idx == 9994) {
                        RECT r = GetQuickJumpToolbarBtnRect(cr_hv);
                        InflateRect(&r, 2, 2);
                        InvalidateRect(hWnd, &r, FALSE);
                    } else if (idx >= 0 && idx < g_toolbarButtonCount) {
                        RECT r = GetToolbarButtonRect(idx);
                        InflateRect(&r, 2, 2);
                        InvalidateRect(hWnd, &r, FALSE);
                    }
                };
                invalidateBtn(prevHover);
                invalidateBtn(hit);
            }
        }

        // 상태바 네비게이션 버튼 hover
        {
            int sbHit = -1;
            if (g_pdfDoc && !g_slideshowActive)
            {
                RECT cr_sh; GetClientRect(hWnd, &cr_sh);
                POINT pt_sh = { x, y };
                RECT b0 = GetStatusFirstBtnRect(cr_sh), b1 = GetStatusPrevBtnRect(cr_sh);
                RECT b2 = GetStatusNextBtnRect(cr_sh),  b3 = GetStatusLastBtnRect(cr_sh);
                sbHit = PtInRect(&b0, pt_sh) ? 0 : PtInRect(&b1, pt_sh) ? 1
                      : PtInRect(&b2, pt_sh) ? 2 : PtInRect(&b3, pt_sh) ? 3 : -1;
            }
            if (sbHit != g_statusHoverBtn)
            {
                g_statusHoverBtn = sbHit;
                RECT cr_si; GetClientRect(hWnd, &cr_si);
                RECT sbInv = { 0, cr_si.bottom - STATUSBAR_HEIGHT, cr_si.right, cr_si.bottom };
                InvalidateRect(hWnd, &sbInv, FALSE);
            }
        }

        // 썸네일 스크롤바 드래그 중이면 스크롤 업데이트
        if (g_thumbSbDragging && g_sidebarVisible && g_sidebarTab == TAB_THUMBNAILS && g_pdfDoc)
        {
            RECT cr_sb; GetClientRect(hWnd, &cr_sb);
            int contentTop = GetBelowToolbarY() + SIDEBAR_HEADER_H;
            int contentH   = cr_sb.bottom - STATUSBAR_HEIGHT - contentTop;
            int totalH     = GetThumbnailTotalHeight(cr_sb);
            if (totalH > contentH && contentH > 0)
            {
                int thumbH    = std::max(20, contentH * contentH / totalH);
                int maxScroll = totalH - contentH;
                int trackH    = contentH;
                int denom     = trackH - thumbH;
                if (denom > 0)
                {
                    int dy        = y - g_thumbSbDragStartY;
                    int newScroll = g_thumbSbDragStartScrollY + dy * maxScroll / denom;
                    g_thumbScrollY = std::clamp(newScroll, 0, maxScroll);
                }
            }
            RECT sbInv = { 0, GetBelowToolbarY(), SIDEBAR_WIDTH, cr_sb.bottom - STATUSBAR_HEIGHT };
            InvalidateRect(hWnd, &sbInv, FALSE);
            break;
        }

        // 툴바 툴팁 처리 (슬라이드쇼 중에는 차단)
        if (!g_slideshowActive && y >= 0 && y < TOOLBAR_HEIGHT && g_hToolbarTooltip)
        {
            RECT cr_tt; GetClientRect(hWnd, &cr_tt);
            RECT findBtn  = GetFindToolbarBtnRect(cr_tt);
            RECT darkBtn  = GetDarkModeToolbarBtnRect(cr_tt);
            RECT lockBtn  = GetScreenLockToolbarBtnRect(cr_tt);
            RECT spotBtn  = GetSpotlightToolbarBtnRect(cr_tt);
            RECT laserBtn = GetLaserPointerToolbarBtnRect(cr_tt);
            RECT qjBtn    = GetQuickJumpToolbarBtnRect(cr_tt);
            POINT pt2 = { x, y };
            std::wstring newTip;
            if      (PtInRect(&findBtn,  pt2)) newTip = LS(STR_TIP_FIND);
            else if (PtInRect(&darkBtn,  pt2)) newTip = LS(STR_TIP_DARKMODE);
            else if (PtInRect(&lockBtn,  pt2)) newTip = LS(STR_TIP_LOCK);
            else if (PtInRect(&spotBtn,  pt2)) newTip = LS(STR_TIP_SPOTLIGHT);
            else if (PtInRect(&laserBtn, pt2)) newTip = LS(STR_TIP_LASER);
            else if (PtInRect(&qjBtn,    pt2)) newTip = LS(STR_TIP_QUICKJUMP);
            else
            {
                int idx = HitTestToolbar(x, y);
                const wchar_t* tip = GetToolbarTooltip(idx);
                if (tip) newTip = tip;
            }
            if (newTip != g_toolbarTipText)
            {
                g_toolbarTipText = newTip;
                ShowWindow(g_hToolbarTooltip, SW_HIDE);
                KillTimer(hWnd, 901);
                if (!newTip.empty())
                    SetTimer(hWnd, 901, 500, nullptr);
            }
        }
        else if (g_hToolbarTooltip)
        {
            if (!g_toolbarTipText.empty())
            {
                g_toolbarTipText.clear();
                KillTimer(hWnd, 901);
                ShowWindow(g_hToolbarTooltip, SW_HIDE);
            }
        }

        // 레이저 포인터 위치 업데이트
        if (g_laserMode && g_pdfDoc && y >= TOOLBAR_HEIGHT)
        {
            POINT newPos = { x, y };
            if (newPos.x != g_laserPos.x || newPos.y != g_laserPos.y)
            {
                // 이전 위치 무효화 (지우기)
                if (g_laserPos.x >= 0)
                {
                    RECT oldRc = { g_laserPos.x - 14, g_laserPos.y - 14,
                                   g_laserPos.x + 14, g_laserPos.y + 14 };
                    InvalidateRect(hWnd, &oldRc, FALSE);
                }
                g_laserPos = newPos;
                // 새 위치 무효화 (그리기)
                RECT newRc = { x - 14, y - 14, x + 14, y + 14 };
                InvalidateRect(hWnd, &newRc, FALSE);
            }
        }

        // 사이드바 hover
        if (g_sidebarVisible)
        {
            RECT cr4; GetClientRect(hWnd, &cr4);
            if (g_sidebarTab == TAB_THUMBNAILS)
            {
                int thHit = ThumbnailHitTest(x, y, cr4);
                if (thHit != g_thumbHoverIdx)
                {
                    g_thumbHoverIdx = thHit;
                    RECT sbInv = { 0, GetBelowToolbarY(), SIDEBAR_WIDTH, cr4.bottom - STATUSBAR_HEIGHT };
                    InvalidateRect(hWnd, &sbInv, FALSE);
                }
            }
            else
            {
                int sbHit = SidebarHitTest(x, y, cr4);
                if (sbHit != g_sidebarHoverIdx)
                {
                    g_sidebarHoverIdx = sbHit;
                    RECT sbInv = { 0, GetBelowToolbarY(), SIDEBAR_WIDTH, cr4.bottom - STATUSBAR_HEIGHT };
                    InvalidateRect(hWnd, &sbInv, FALSE);
                }
            }
        }
        // 마우스 추적 (leave 이벤트 감지)
        if (!g_trackingMouse)
        {
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hWnd;
            TrackMouseEvent(&tme);
            g_trackingMouse = true;
        }

        // 노트 아이콘 hover
        if (g_pdfDoc && !g_notes.empty() && g_hNoteTooltip)
        {
            RECT crn; GetClientRect(hWnd, &crn);
            const RECT vpn = GetViewportRect(crn);
            int prevHoverNote = g_hoverNote;
            g_hoverNote = HitTestNote(x, y, vpn);
            if (g_hoverNote != prevHoverNote)
                InvalidateRect(hWnd, nullptr, FALSE);

            TOOLINFOW ti = {};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
            ti.hwnd   = hWnd;
            ti.uId    = 1;
            if (g_hoverNote >= 0)
            {
                const std::wstring& noteText = g_notes[g_hoverNote].text;
                ti.lpszText = const_cast<LPWSTR>(noteText.empty() ? LS(STR_NOTE_EMPTY) : noteText.c_str());
                SendMessageW(g_hNoteTooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
                POINT pt = { x, y };
                ClientToScreen(hWnd, &pt);
                SendMessageW(g_hNoteTooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x + 16, pt.y + 16));
                SendMessageW(g_hNoteTooltip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
            }
            else if (prevHoverNote >= 0)
            {
                SendMessageW(g_hNoteTooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
            }
        }
        else if (g_hNoteTooltip && g_hoverNote >= 0)
        {
            TOOLINFOW ti = {};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
            ti.hwnd   = hWnd;
            ti.uId    = 1;
            SendMessageW(g_hNoteTooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
            g_hoverNote = -1;
        }
    }
    break;

    case WM_MOUSELEAVE:
        GroupPanel::OnMouseLeave();
        g_trackingMouse = false;
        if (g_statusHoverBtn != -1)
        {
            g_statusHoverBtn = -1;
            RECT cr_sl; GetClientRect(hWnd, &cr_sl);
            RECT sbInv = { 0, cr_sl.bottom - STATUSBAR_HEIGHT, cr_sl.right, cr_sl.bottom };
            InvalidateRect(hWnd, &sbInv, FALSE);
        }
        // 슬라이드쇼 화살표 hover 초기화
        if (g_ssArrowHoverLeft || g_ssArrowHoverRight || g_ssCloseHover)
        {
            g_ssArrowHoverLeft = g_ssArrowHoverRight = false;
            g_ssCloseHover = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (g_hoverNote != -1 && g_hNoteTooltip)
        {
            TOOLINFOW ti = {};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
            ti.hwnd   = hWnd;
            ti.uId    = 1;
            SendMessageW(g_hNoteTooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
            g_hoverNote = -1;
        }
        // 탭 툴팁 숨기기
        if (g_hTabTooltip && g_tabTooltipIdx >= 0)
        {
            g_tabTooltipIdx = -1;
            g_tabTipText.clear();
            ShowWindow(g_hTabTooltip, SW_HIDE);
        }
        if (g_hoverButton != -1)
        {
            RECT r;
            if (g_hoverButton == 9999) {
                RECT cr; GetClientRect(hWnd, &cr);
                r = GetFindToolbarBtnRect(cr);
            } else if (g_hoverButton == 9998) {
                RECT cr; GetClientRect(hWnd, &cr);
                r = GetDarkModeToolbarBtnRect(cr);
            } else if (g_hoverButton == 9997) {
                RECT cr; GetClientRect(hWnd, &cr);
                r = GetScreenLockToolbarBtnRect(cr);
            } else if (g_hoverButton == 9996) {
                RECT cr; GetClientRect(hWnd, &cr);
                r = GetSpotlightToolbarBtnRect(cr);
            } else if (g_hoverButton == 9995) {
                RECT cr; GetClientRect(hWnd, &cr);
                r = GetLaserPointerToolbarBtnRect(cr);
            } else if (g_hoverButton == 9994) {
                RECT cr; GetClientRect(hWnd, &cr);
                r = GetQuickJumpToolbarBtnRect(cr);
            } else if (g_hoverButton >= 0 && g_hoverButton < g_toolbarButtonCount) {
                r = GetToolbarButtonRect(g_hoverButton);
            } else {
                g_hoverButton = -1;
                break;
            }
            InflateRect(&r, 2, 2);
            g_hoverButton = -1;
            InvalidateRect(hWnd, &r, FALSE);
        }
        // 레이저 포인터 위치 초기화
        if (g_laserMode && g_laserPos.x >= 0)
        {
            RECT inv = { g_laserPos.x - 12, g_laserPos.y - 12, g_laserPos.x + 12, g_laserPos.y + 12 };
            g_laserPos = { -1, -1 };
            InvalidateRect(hWnd, &inv, FALSE);
        }
        // 툴바 툴팁 닫기
        if (g_hToolbarTooltip)
        {
            g_toolbarTipText.clear();
            KillTimer(hWnd, 901);
            ShowWindow(g_hToolbarTooltip, SW_HIDE);
        }
    break;

    case WM_SETCURSOR:
    {
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt);

            // 슬라이드쇼 화살표 영역: 손 커서, 나머지 뷰포트: 기본 화살표
            if (g_slideshowActive)
            {
                RECT cr; GetClientRect(hWnd, &cr);
                RECT lr = GetSSArrowRect(cr, true);
                RECT rr = GetSSArrowRect(cr, false);
                RECT xr = GetSSCloseRect(cr);
                if (PtInRect(&lr, pt) || PtInRect(&rr, pt) || PtInRect(&xr, pt))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                }
                else
                {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                }
                return TRUE;
            }

            // 그룹 패널 영역: OnMouseMove에서 이미 커서 설정했으므로 그대로 유지
            if (GroupPanel::IsVisible()) {
                RECT cr; GetClientRect(hWnd, &cr);
                RECT gpr = GroupPanel::GetPanelRect(cr);
                if (pt.x >= gpr.left && pt.x < gpr.right &&
                    pt.y >= gpr.top  && pt.y < gpr.bottom) {
                    return TRUE; // DefWindowProc 호출 안 함 → 커서 유지
                }
            }

            // 탭바 X 닫기 버튼 위: 손 커서
            if (!g_tabs.empty() && pt.y >= TOOLBAR_HEIGHT && pt.y < TOOLBAR_HEIGHT + TAB_BAR_HEIGHT)
            {
                bool closeBtn = false;
                HitTestTabBar(pt.x, pt.y, &closeBtn);
                if (closeBtn)
                {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                return TRUE;
            }

            // 사이드바 북마크 × 삭제 버튼 위: 손 커서, 나머지 사이드바: 기본 커서
            if (g_laserMode && g_pdfDoc && pt.y >= TOOLBAR_HEIGHT)
            {
                SetCursor(nullptr);
                return TRUE;
            }

            // 사이드바 북마크 × 삭제 버튼 위: 손 커서, 나머지 사이드바: 기본 커서
            if (g_sidebarVisible && pt.x >= 0 && pt.x < SIDEBAR_WIDTH &&
                pt.y >= TOOLBAR_HEIGHT)
            {
                RECT cr; GetClientRect(hWnd, &cr);
                if (g_sidebarTab == TAB_BOOKMARKS)
                {
                    int sbHit = SidebarHitTest(pt.x, pt.y, cr);
                    if (sbHit >= 0 && sbHit < (int)g_bookmarks.size() &&
                        g_bookmarks[sbHit].isUserAdded && pt.x >= SIDEBAR_WIDTH - 22)
                    {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                    }
                    else
                    {
                        SetCursor(LoadCursor(nullptr, IDC_ARROW));
                    }
                }
                else
                {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }
                return TRUE;
            }

            // Redact 모드: 십자 커서
            if (g_redactMode && g_pdfDoc && pt.y >= GetBelowToolbarY())
            {
                RECT cr; GetClientRect(hWnd, &cr);
                if (pt.y < cr.bottom - STATUSBAR_HEIGHT)
                {
                    SetCursor(LoadCursor(nullptr, IDC_CROSS));
                    return TRUE;
                }
            }

            // 스포트라이트 모드: 뷰포트 영역에서 십자 커서
            if (g_spotlightMode && g_pdfDoc && pt.y >= TOOLBAR_HEIGHT)
            {
                RECT cr; GetClientRect(hWnd, &cr);
                if (pt.y < cr.bottom - STATUSBAR_HEIGHT)
                {
                    SetCursor(LoadCursor(nullptr, IDC_CROSS));
                    return TRUE;
                }
            }
            if (g_pdfDoc && g_selectedTextBox >= 0 && !g_isDraggingTextBox)
            {
                RECT tbRc = TextBoxScreenRect(g_textBoxes[g_selectedTextBox]);
                int hh = HitTestTextBoxHandle(tbRc, pt.x, pt.y);
                if (hh >= 0)
                {
                    SetCursor(LoadCursor(nullptr, HandleCursor(hh)));
                    return TRUE;
                }
            }
            // 서명 크기 조절 핸들 위 커서
            if (g_pdfDoc && g_selectedSig >= 0 && g_selectedSig < (int)g_signatures.size()
                && !g_isDraggingSig && !g_isResizingSig)
            {
                RECT sigRc = SigScreenRect(g_signatures[g_selectedSig]);
                int hh = HitTestSigHandle(sigRc, pt.x, pt.y);
                if (hh >= 0)
                {
                    SetCursor(LoadCursor(nullptr, HandleCursor(hh)));
                    return TRUE;
                }
            }
            // 서명 본체 위 이동 커서
            if (g_pdfDoc && !g_signatures.empty())
            {
                RECT cr_sc; GetClientRect(hWnd, &cr_sc);
                RECT vp_sc = GetViewportRect(cr_sc);
                int si = HitTestSignature(pt.x, pt.y, vp_sc);
                if (si >= 0)
                {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                    return TRUE;
                }
            }
            // 텍스트 박스 본체 위 이동 커서
            if (g_pdfDoc && !g_textBoxes.empty())
            {
                RECT crtmp; GetClientRect(hWnd, &crtmp);
                const RECT vptmp = GetViewportRect(crtmp);
                int ti = HitTestTextBox(pt.x, pt.y, vptmp);
                if (ti >= 0)
                {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                    return TRUE;
                }
            }
            // 잉크 모드: 기본 화살표 커서
            if (g_inkMode && g_pdfDoc && pt.y >= GetBelowToolbarY())
            {
                RECT cr; GetClientRect(hWnd, &cr);
                if (pt.y < cr.bottom - STATUSBAR_HEIGHT &&
                    !(g_sidebarVisible && pt.x < SIDEBAR_WIDTH))
                {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                    return TRUE;
                }
            }
            // 도형 그리기 모드 (사각형/원): 십자 커서 — handMode보다 우선
            if (g_shapeMode != 0 && g_pdfDoc && pt.y >= GetBelowToolbarY())
            {
                RECT cr; GetClientRect(hWnd, &cr);
                if (pt.y < cr.bottom - STATUSBAR_HEIGHT &&
                    !(g_sidebarVisible && pt.x < SIDEBAR_WIDTH))
                {
                    SetCursor(LoadCursor(nullptr, IDC_CROSS));
                    return TRUE;
                }
            }

        }

        // 텍스트 선택 모드: I-빔 커서 (사이드바 영역 제외, 도형/잉크 모드 제외)
        if (!g_handMode && !g_shapeMode && !g_inkMode && g_pdfDoc && LOWORD(lParam) == HTCLIENT)
        {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt);
            RECT cr; GetClientRect(hWnd, &cr);
            const RECT vp = GetViewportRect(cr);
            if (pt.y >= vp.top && pt.y < vp.bottom &&
                !(g_sidebarVisible && pt.x < SIDEBAR_WIDTH))
            {
                SetCursor(LoadCursor(nullptr, IDC_IBEAM));
                return TRUE;
            }
        }

        if (LOWORD(lParam) == HTCLIENT && g_handMode && !g_shapeMode && !g_inkMode && g_pdfDoc)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hWnd, &pt);
            RECT cr;
            GetClientRect(hWnd, &cr);
            RECT vp = GetViewportRect(cr);
            // 뷰포트 영역 안에서, 사이드바 영역 제외
            if (pt.y >= vp.top && pt.y < vp.bottom &&
                !(g_sidebarVisible && pt.x < SIDEBAR_WIDTH))
            {
                SetCursor(g_isPanning ? g_curGrab : g_curHand);
                return TRUE;
            }
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        // 그룹 패널
        if (GroupPanel::IsVisible()) {
            RECT cr; GetClientRect(hWnd, &cr);
            GroupPanel::OnLButtonDown(hWnd, x, y, cr);
        }

        // QuickJump 팝업이 열려 있을 때 처리
        if (g_hQJPopup)
        {
            RECT qjWr; GetWindowRect(g_hQJPopup, &qjWr);
            POINT ptScr = { x, y };
            ClientToScreen(hWnd, &ptScr);
            if (!PtInRect(&qjWr, ptScr))
                QJPopupClose();
            // QJ 툴바 버튼 위 클릭이면 닫고 끝 (토글 OFF)
            if (y >= 0 && y < TOOLBAR_HEIGHT)
            {
                RECT cr_tb2; GetClientRect(hWnd, &cr_tb2);
                RECT qjBtn2 = GetQuickJumpToolbarBtnRect(cr_tb2);
                POINT pt2 = { x, y };
                if (PtInRect(&qjBtn2, pt2)) break;
            }
        }

        // 화면 잠금: 툴바는 허용, 드래그 펨인 방지만 (WM_MOUSEMOVE에서 SetCapture 허용)

        // 툴바 클릭: DOWN에서는 눌림 상태만 기록, UP에서 실행 (가장 먼저 처리)
        if (y >= 0 && y < TOOLBAR_HEIGHT)
        {
            RECT cr_tb; GetClientRect(hWnd, &cr_tb);
            RECT findBtn  = GetFindToolbarBtnRect(cr_tb);
            RECT darkBtn  = GetDarkModeToolbarBtnRect(cr_tb);
            RECT lockBtn  = GetScreenLockToolbarBtnRect(cr_tb);
            RECT spotBtn  = GetSpotlightToolbarBtnRect(cr_tb);
            RECT laserBtn = GetLaserPointerToolbarBtnRect(cr_tb);
            RECT qjBtn    = GetQuickJumpToolbarBtnRect(cr_tb);
            POINT pt_tb = { x, y };
            if (PtInRect(&findBtn,  pt_tb)) { g_toolbarPressedCmd = IDM_FIND;          SetCapture(hWnd); break; }
            if (PtInRect(&darkBtn,  pt_tb)) { g_toolbarPressedCmd = IDM_DARK_MODE;     SetCapture(hWnd); break; }
            if (PtInRect(&lockBtn,  pt_tb)) { g_toolbarPressedCmd = IDM_SCREEN_LOCK;   SetCapture(hWnd); break; }
            if (PtInRect(&spotBtn,  pt_tb)) { g_toolbarPressedCmd = IDM_SPOTLIGHT;     SetCapture(hWnd); break; }
            if (PtInRect(&laserBtn, pt_tb)) { g_toolbarPressedCmd = IDM_LASER_POINTER; SetCapture(hWnd); break; }
            if (PtInRect(&qjBtn,    pt_tb)) { g_toolbarPressedCmd = IDM_QUICKJUMP;     SetCapture(hWnd); break; }
            int hit = HitTestToolbar(x, y);
            if (hit >= 0 && g_toolbarButtons[hit].commandId != 0)
            {
                bool enabled = !g_toolbarButtons[hit].needsDoc || g_pdfDoc != nullptr;
                if (enabled)
                {
                    g_toolbarPressedBtn = hit;
                    g_toolbarPressedCmd = g_toolbarButtons[hit].commandId;
                    SetCapture(hWnd);
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
            break; // 툴바 영역이면 이후 처리 불필요
        }

        // 폼 필드 클릭: 폼이 활성화되어 있고 현재 페이지 위를 클릭했을 때 최우선 처리
        if (g_formHandle && g_pdfDoc && g_pdfPage && !g_redactMode && !g_inkMode)
        {
            RECT fCr; GetClientRect(hWnd, &fCr);
            const RECT fVp = GetViewportRect(fCr);
            bool inPage = false;
            if (y >= fVp.top && y < fVp.bottom && x >= fVp.left && x < fVp.right)
            {
                int pgL, pgT;
                GetPageScreenPos(g_currentPage, fVp, &pgL, &pgT);
                const auto& fLayout = g_pageLayouts[g_currentPage];
                int pgX = x - pgL;
                int pgY = y - pgT;
                if (pgX >= 0 && pgX < fLayout.renderW && pgY >= 0 && pgY < fLayout.renderH)
                {
                    inPage = true;
                    SetFocus(hWnd);
                    double pdfX = 0, pdfY = 0;
                    FormDeviceToPage(pgX, pgY, fLayout.renderW, fLayout.renderH, &pdfX, &pdfY);
                    // 반환값: 폼 필드에 포커스가 잡혔으면 TRUE
                    g_formFieldFocused = (FORM_OnLButtonDown(g_formHandle, g_pdfPage, 0, pdfX, pdfY) == TRUE);
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
            if (!inPage)
                g_formFieldFocused = false;
        }

        // Redact 모드: 드래그 시작
        if (g_redactMode && g_pdfDoc)
        {
            RECT cr0; GetClientRect(hWnd, &cr0);
            if (y >= GetBelowToolbarY())
            {
                // 기존 redact 박스 선택 확인
                const RECT vp0 = GetViewportRect(cr0);
                g_selectedRedact = -1;
                for (int ri = (int)g_redacts.size() - 1; ri >= 0; ri--)
                {
                    const auto& rd = g_redacts[ri];
                    int pgL, pgT;
                    GetPageScreenPos(rd.pageIndex, vp0, &pgL, &pgT);
                    double pageW = g_pageLayouts[rd.pageIndex].pdfW;
                    double pageH = g_pageLayouts[rd.pageIndex].pdfH;
                    int sx1, sy1, sx2, sy2;
                    PdfToScreen(rd.pdfX,           rd.pdfY,           pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(rd.pageIndex), &sx1, &sy1);
                    PdfToScreen(rd.pdfX + rd.pdfW, rd.pdfY - rd.pdfH, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(rd.pageIndex), &sx2, &sy2);
                    // 클릭 감지 영역을 4픽셀 넉넉하게 확장
                    RECT rr = { std::min(sx1,sx2) - 4, std::min(sy1,sy2) - 4,
                                std::max(sx1,sx2) + 4, std::max(sy1,sy2) + 4 };
                    POINT pt0 = { x, y };
                    if (PtInRect(&rr, pt0)) { g_selectedRedact = ri; SetFocus(hWnd); break; }
                }
                if (g_selectedRedact < 0)
                {
                    // 새 영역 드래그 시작
                    g_isDraggingRedact  = true;
                    g_redactDragStartX  = x;
                    g_redactDragStartY  = y;
                    g_redactDragCurX    = x;
                    g_redactDragCurY    = y;
                    SetCapture(hWnd);
                }
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
        }

        // 도형 선택 (모드 무관 — 항상 클릭으로 선택 가능)
        if (g_pdfDoc && !g_shapes.empty())
        {
            RECT cr0; GetClientRect(hWnd, &cr0);
            if (y >= GetBelowToolbarY())
            {
                const RECT vp0 = GetViewportRect(cr0);
                bool hitShape = false;
                for (int si2 = (int)g_shapes.size() - 1; si2 >= 0; si2--)
                {
                    const auto& sh = g_shapes[si2];
                    if (sh.pageIndex < 0 || sh.pageIndex >= (int)g_pageLayouts.size()) continue;
                    int pgL, pgT;
                    GetPageScreenPos(sh.pageIndex, vp0, &pgL, &pgT);
                    double pageW = g_pageLayouts[sh.pageIndex].pdfW;
                    double pageH = g_pageLayouts[sh.pageIndex].pdfH;
                    int sx1, sy1, sx2, sy2;
                    PdfToScreen(sh.pdfX,           sh.pdfY,           pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sh.pageIndex), &sx1, &sy1);
                    PdfToScreen(sh.pdfX + sh.pdfW, sh.pdfY - sh.pdfH, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sh.pageIndex), &sx2, &sy2);
                    RECT rr = { std::min(sx1,sx2) - 4, std::min(sy1,sy2) - 4,
                                std::max(sx1,sx2) + 4, std::max(sy1,sy2) + 4 };
                    POINT pt0 = { x, y };
                    if (PtInRect(&rr, pt0))
                    {
                        g_selectedShape = si2;
                        hitShape = true;
                        SetFocus(hWnd);
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    }
                }
                // 빈 곳 클릭 시 선택 해제
                if (!hitShape && g_selectedShape >= 0)
                {
                    g_selectedShape = -1;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
        }
        // 잉크 스트로크 선택 (잉크 모드가 아닐 때 — 선분과의 거리로 판정)
        if (!g_inkMode && g_pdfDoc && !g_inkStrokes.empty())
        {
            RECT cr0; GetClientRect(hWnd, &cr0);
            if (y >= GetBelowToolbarY())
            {
                const RECT vp0 = GetViewportRect(cr0);
                constexpr float kHitThresh = 6.0f; // 픽셀 거리 임계값
                int hitIdx = -1;
                for (int si2 = (int)g_inkStrokes.size() - 1; si2 >= 0; si2--)
                {
                    const auto& sk = g_inkStrokes[si2];
                    if (sk.pageIndex < 0 || sk.pageIndex >= (int)g_pageLayouts.size()) continue;
                    int pgL, pgT;
                    GetPageScreenPos(sk.pageIndex, vp0, &pgL, &pgT);
                    double pageW = g_pageLayouts[sk.pageIndex].pdfW;
                    double pageH = g_pageLayouts[sk.pageIndex].pdfH;
                    for (int pi2 = 1; pi2 < (int)sk.pdfPts.size(); ++pi2)
                    {
                        int ax, ay, bx, by;
                        PdfToScreen(sk.pdfPts[pi2-1].first, sk.pdfPts[pi2-1].second,
                                    pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sk.pageIndex), &ax, &ay);
                        PdfToScreen(sk.pdfPts[pi2].first,   sk.pdfPts[pi2].second,
                                    pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sk.pageIndex), &bx, &by);
                        // 점 (x,y)와 선분 (ax,ay)-(bx,by) 거리
                        float dx = (float)(bx - ax), dy = (float)(by - ay);
                        float len2 = dx*dx + dy*dy;
                        float dist;
                        if (len2 < 1.0f) {
                            float ex = (float)(x - ax), ey = (float)(y - ay);
                            dist = sqrtf(ex*ex + ey*ey);
                        } else {
                            float t = ((float)(x - ax)*dx + (float)(y - ay)*dy) / len2;
                            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                            float px2 = ax + t*dx - (float)x;
                            float py2 = ay + t*dy - (float)y;
                            dist = sqrtf(px2*px2 + py2*py2);
                        }
                        if (dist <= kHitThresh) { hitIdx = si2; break; }
                    }
                    if (hitIdx >= 0) break;
                }
                if (hitIdx >= 0)
                {
                    g_selectedInkStroke = hitIdx;
                    g_selectedShape = -1;
                    SetFocus(hWnd);
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
                else if (g_selectedInkStroke >= 0)
                {
                    g_selectedInkStroke = -1;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
        }
        // 잉크 모드: 드래그로 자유 곡선 그리기 시작
        if (g_inkMode && g_pdfDoc)
        {
            RECT cr0; GetClientRect(hWnd, &cr0);
            if (y >= GetBelowToolbarY())
            {
                g_isDrawingInk = true;
                g_inkStroke.clear();
                g_inkStroke.push_back({ x, y });
                SetCapture(hWnd);
                break;
            }
        }
        // 도형 모드: 드래그로 새 도형 생성
        if (g_shapeMode != 0 && g_pdfDoc)
        {
            RECT cr0; GetClientRect(hWnd, &cr0);
            if (y >= GetBelowToolbarY())
            {
                const RECT vp0 = GetViewportRect(cr0);
                // 기존 도형을 클릭한 경우에는 드래그 생성 생략
                bool hitExisting = false;
                for (int si2 = (int)g_shapes.size() - 1; si2 >= 0; si2--)
                {
                    const auto& sh = g_shapes[si2];
                    if (sh.pageIndex < 0 || sh.pageIndex >= (int)g_pageLayouts.size()) continue;
                    int pgL, pgT;
                    GetPageScreenPos(sh.pageIndex, vp0, &pgL, &pgT);
                    double pageW = g_pageLayouts[sh.pageIndex].pdfW;
                    double pageH = g_pageLayouts[sh.pageIndex].pdfH;
                    int sx1, sy1, sx2, sy2;
                    PdfToScreen(sh.pdfX,           sh.pdfY,           pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sh.pageIndex), &sx1, &sy1);
                    PdfToScreen(sh.pdfX + sh.pdfW, sh.pdfY - sh.pdfH, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sh.pageIndex), &sx2, &sy2);
                    RECT rr = { std::min(sx1,sx2) - 4, std::min(sy1,sy2) - 4,
                                std::max(sx1,sx2) + 4, std::max(sy1,sy2) + 4 };
                    POINT pt0 = { x, y };
                    if (PtInRect(&rr, pt0)) { hitExisting = true; break; }
                }
                if (!hitExisting)
                {
                    g_selectedShape    = -1;
                    g_isDraggingShape  = true;
                    g_shapeDragStartX  = x;
                    g_shapeDragStartY  = y;
                    g_shapeDragCurX    = x;
                    g_shapeDragCurY    = y;
                    SetCapture(hWnd);
                }
                else if (g_selectedShape < 0)
                {
                    // 기존 도형을 클릭했지만 아직 선택 안 된 경우 → 아무것도 안 함
                    // (선택은 위의 hitExisting 루프에서 처리)
                }
                else
                {
                    // 도형이 아닌 빈 곳 클릭 → 선택 해제
                    g_selectedShape = -1;
                }                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
        }
        {
            RECT cr0; GetClientRect(hWnd, &cr0);
            const RECT vp0 = GetViewportRect(cr0);
            int ni = HitTestNote(x, y, vp0);
            if (ni >= 0)
            {
                // 아이콘 화면 좌표 계산
                const auto& note = g_notes[ni];
                int pgLeft, pgTop;
                GetPageScreenPos(note.pageIndex, vp0, &pgLeft, &pgTop);
                const double pageW = g_pageLayouts[note.pageIndex].pdfW;
                const double pageH = g_pageLayouts[note.pageIndex].pdfH;
                int sx, sy;
                PdfToScreen(note.pdfX, note.pdfY, pageW, pageH, pgLeft, pgTop, g_zoom, GetEffectiveRotation(note.pageIndex), &sx, &sy);

                g_isDraggingNote  = true;
                g_draggingNoteIdx = ni;
                g_noteDragOffsetX = x - sx;
                g_noteDragOffsetY = y - sy;
                g_selectedNote    = ni;
                SetCapture(hWnd);
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
        }

        // 텍스트 박스 클릭/드래그 시작
        if (g_pdfDoc && !g_textBoxes.empty())
        {
            RECT crtb; GetClientRect(hWnd, &crtb);
            const RECT vptb = GetViewportRect(crtb);

            // 먼저 선택된 박스의 핸들 히트테스트
            if (g_selectedTextBox >= 0)
            {
                RECT tbRc = TextBoxScreenRect(g_textBoxes[g_selectedTextBox]);
                int hh = HitTestTextBoxHandle(tbRc, x, y);
                if (hh >= 0)
                {
                    // 리사이즈 시작
                    const auto& tb = g_textBoxes[g_selectedTextBox];
                    g_isResizingTextBox  = true;
                    g_resizeHandle       = hh;
                    g_tbResizeInitX      = tb.pdfX;
                    g_tbResizeInitY      = tb.pdfY;
                    g_tbResizeInitW      = tb.pdfW;
                    g_tbResizeInitH      = tb.pdfH;
                    g_tbResizeInitMouseX = x;
                    g_tbResizeInitMouseY = y;
                    // 리사이즈 중 Edit 컨트롤이 불투명하게 깜빡이지 않도록 숨김
                    if (g_hTextBoxEdit) ShowWindow(g_hTextBoxEdit, SW_HIDE);
                    SetCapture(hWnd);
                    SetCursor(LoadCursor(nullptr, HandleCursor(hh)));
                    RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT;
                    InvalidateRect(hWnd, &ir, FALSE);
                    break;
                }
            }

            int ti = HitTestTextBox(x, y, vptb);
            if (ti >= 0)
            {
                g_selectedTextBox   = ti;
                g_isDraggingTextBox = true;
                RECT tbRc = TextBoxScreenRect(g_textBoxes[ti]);
                g_tbDragOffsetX = x - tbRc.left;
                g_tbDragOffsetY = y - tbRc.top;
                SetCapture(hWnd);
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT;
                InvalidateRect(hWnd, &ir, FALSE);
                break;
            }
            else if (g_selectedTextBox >= 0)
            {
                g_selectedTextBox = -1;
                RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT;
                InvalidateRect(hWnd, &ir, FALSE);
            }
        }

        // ── 서명 히트 테스트 (텍스트박스와 독립적으로 항상 처리) ──
        if (g_pdfDoc && !g_signatures.empty())
        {
            // 선택된 서명의 핸들 먼저 체크
            if (g_selectedSig >= 0 && g_selectedSig < (int)g_signatures.size()) {
                RECT sr2 = SigScreenRect(g_signatures[g_selectedSig]);
                int hh = HitTestSigHandle(sr2, x, y);
                if (hh >= 0) {
                    const auto& sig = g_signatures[g_selectedSig];
                    g_isResizingSig    = true;
                    g_sigResizeHandle  = hh;
                    g_sigResizeInitX   = sig.pdfX;
                    g_sigResizeInitY   = sig.pdfY;
                    g_sigResizeInitW   = sig.pdfW;
                    g_sigResizeInitH   = sig.pdfH;
                    g_sigResizeMouseX  = x;
                    g_sigResizeMouseY  = y;
                    g_sigResizeInitStrokes = sig.strokes; // 원본 획 좌표 스냅샷
                    SetCapture(hWnd);
                    SetCursor(LoadCursor(nullptr, HandleCursor(hh)));
                    InvalidateRect(hWnd, nullptr, FALSE);
                    break;
                }
            }
            RECT cr_sig; GetClientRect(hWnd, &cr_sig);
            RECT vp_sig = GetViewportRect(cr_sig);
            int si = HitTestSignature(x, y, vp_sig);
            if (si >= 0) {
                g_selectedSig    = si;
                g_isDraggingSig  = true;
                RECT sr2 = SigScreenRect(g_signatures[si]);
                g_sigDragOffsetX = x - sr2.left;
                g_sigDragOffsetY = y - sr2.top;
                SetCapture(hWnd);
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            } else if (g_selectedSig >= 0) {
                g_selectedSig = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }

        // 상태바 이전/다음 페이지 버튼 클릭
        {
            RECT cr_sb; GetClientRect(hWnd, &cr_sb);
            POINT pt_sb = { x, y };
            if (g_pdfDoc)
            {
                RECT prevSbBtn  = GetStatusPrevBtnRect(cr_sb);
                RECT nextSbBtn  = GetStatusNextBtnRect(cr_sb);
                RECT firstSbBtn = GetStatusFirstBtnRect(cr_sb);
                RECT lastSbBtn  = GetStatusLastBtnRect(cr_sb);
                if (PtInRect(&firstSbBtn, pt_sb)) { if (!g_screenLock) ScrollToPage(hWnd, 0);               break; }
                if (PtInRect(&lastSbBtn,  pt_sb)) { if (!g_screenLock) ScrollToPage(hWnd, g_pageCount - 1); break; }
                if (PtInRect(&prevSbBtn,  pt_sb)) { if (!g_screenLock) NavigatePage(hWnd, g_twoPageView ? -2 : -1); break; }
                if (PtInRect(&nextSbBtn,  pt_sb)) { if (!g_screenLock) NavigatePage(hWnd, g_twoPageView ? +2 : +1); break; }
            }
        }

        // 탭 바 클릭
        if (!g_tabs.empty())
        {
            bool closeBtn = false;
            int tabHit = HitTestTabBar(x, y, &closeBtn);
            if (tabHit == -2) { AddNewTab(hWnd); break; }
            if (tabHit >= 0)
            {
                if (closeBtn)
                    CloseTab(hWnd, tabHit);
                else
                    SwitchToTab(hWnd, tabHit);
                break;
            }
        }

        // 사이드바 클릭
        if (g_sidebarVisible && x < SIDEBAR_WIDTH && y >= GetBelowToolbarY())
        {
            // 헤더 클릭은 무시 (탭 전환은 툴바 버튼으로만)
            if (y < GetBelowToolbarY() + SIDEBAR_HEADER_H)
                break;

            // 썸네일 탭 클릭
            if (g_sidebarTab == TAB_THUMBNAILS)
            {
                RECT cr3; GetClientRect(hWnd, &cr3);

                // 스크롤바 클릭 여부 먼저 확인
                int contentTop = GetBelowToolbarY() + SIDEBAR_HEADER_H;
                int contentH   = cr3.bottom - STATUSBAR_HEIGHT - contentTop;
                int totalH     = GetThumbnailTotalHeight(cr3);
                if (totalH > contentH && contentH > 0)
                {
                    int trackX = SIDEBAR_WIDTH - THUMB_SB_W - 2;
                    if (x >= trackX && x <= SIDEBAR_WIDTH && y >= contentTop && y < cr3.bottom - STATUSBAR_HEIGHT)
                    {
                        int thumbH    = std::max(20, contentH * contentH / totalH);
                        int maxScroll = totalH - contentH;
                        int denom     = contentH - thumbH;
                        int thumbY    = contentTop + (denom > 0 ? (g_thumbScrollY * denom / maxScroll) : 0);

                        if (y >= thumbY && y < thumbY + thumbH)
                        {
                            // 썸 드래그 시작
                            g_thumbSbDragging = true;
                            g_thumbSbDragStartY = y;
                            g_thumbSbDragStartScrollY = g_thumbScrollY;
                            SetCapture(hWnd);
                        }
                        else if (denom > 0)
                        {
                            // 트랙 클릭: 해당 위치로 점프
                            int newScroll = (y - contentTop - thumbH / 2) * maxScroll / denom;
                            g_thumbScrollY = std::clamp(newScroll, 0, maxScroll);
                            RECT sbInv = { 0, GetBelowToolbarY(), SIDEBAR_WIDTH, cr3.bottom - STATUSBAR_HEIGHT };
                            InvalidateRect(hWnd, &sbInv, FALSE);
                        }
                        break;
                    }
                }

                int thumbIdx = ThumbnailHitTest(x, y, cr3);
                if (thumbIdx >= 0 && thumbIdx < g_pageCount)
                {
                    ScrollToPage(hWnd, thumbIdx);
                    InvalidateRect(hWnd, nullptr, TRUE);
                }
                break;
            }

            // 북마크 탭 클릭
            RECT cr3; GetClientRect(hWnd, &cr3);
            int hitIdx = SidebarHitTest(x, y, cr3);
            if (hitIdx == -2)
            {
                // "+ 현재 페이지 북마크" 버튼
                AddUserBookmark(hWnd);
                break;
            }
            if (hitIdx >= 0)
            {
                auto& bm = g_bookmarks[hitIdx];

                // 사용자 북마크 × 삭제 버튼 클릭 여부
                if (bm.isUserAdded && x >= SIDEBAR_WIDTH - 22)
                {
                    DeleteUserBookmark(hWnd, hitIdx);
                    break;
                }

                // 펼침/접힘 토글 (자식이 있는 항목)
                if (bm.hasChildren)
                {
                    int arrowLeft = 8 + bm.level * SIDEBAR_INDENT - 14;
                    if (x < arrowLeft + 16)
                    {
                        bm.isOpen = !bm.isOpen;
                        InvalidateRect(hWnd, nullptr, TRUE);
                        break;
                    }
                }
                // 페이지 이동
                if (bm.pageIndex >= 0 && bm.pageIndex < g_pageCount)
                {
                    ScrollToPage(hWnd, bm.pageIndex);
                    InvalidateRect(hWnd, nullptr, TRUE);
                }
                else if (bm.hasChildren)
                {
                    bm.isOpen = !bm.isOpen;
                    InvalidateRect(hWnd, nullptr, TRUE);
                }
            }
            break;
        }

        // 툴바 클릭: DOWN에서는 눌림 상태만 기록, UP에서 실행
        if (y >= 0 && y < TOOLBAR_HEIGHT)
        {
            RECT cr_tb; GetClientRect(hWnd, &cr_tb);
            RECT findBtn  = GetFindToolbarBtnRect(cr_tb);
            RECT darkBtn  = GetDarkModeToolbarBtnRect(cr_tb);
            RECT lockBtn  = GetScreenLockToolbarBtnRect(cr_tb);
            RECT spotBtn  = GetSpotlightToolbarBtnRect(cr_tb);
            RECT laserBtn = GetLaserPointerToolbarBtnRect(cr_tb);
            RECT qjBtn    = GetQuickJumpToolbarBtnRect(cr_tb);
            POINT pt_tb = { x, y };
            if (PtInRect(&findBtn,  pt_tb)) { g_toolbarPressedCmd = IDM_FIND;          SetCapture(hWnd); break; }
            if (PtInRect(&darkBtn,  pt_tb)) { g_toolbarPressedCmd = IDM_DARK_MODE;     SetCapture(hWnd); break; }
            if (PtInRect(&lockBtn,  pt_tb)) { g_toolbarPressedCmd = IDM_SCREEN_LOCK;   SetCapture(hWnd); break; }
            if (PtInRect(&spotBtn,  pt_tb)) { g_toolbarPressedCmd = IDM_SPOTLIGHT;     SetCapture(hWnd); break; }
            if (PtInRect(&laserBtn, pt_tb)) { g_toolbarPressedCmd = IDM_LASER_POINTER; SetCapture(hWnd); break; }
            if (PtInRect(&qjBtn,    pt_tb)) { g_toolbarPressedCmd = IDM_QUICKJUMP;     SetCapture(hWnd); break; }
        }
        {
            int hit = HitTestToolbar(x, y);
            if (hit >= 0 && g_toolbarButtons[hit].commandId != 0)
            {
                bool enabled = !g_toolbarButtons[hit].needsDoc || g_pdfDoc != nullptr;
                if (enabled)
                {
                    g_toolbarPressedBtn = hit;
                    g_toolbarPressedCmd = g_toolbarButtons[hit].commandId;
                    SetCapture(hWnd);
                }
                break;
            }
        }

        // 스포트라이트 드래그 시작
        if (g_spotlightMode && g_pdfDoc && y >= TOOLBAR_HEIGHT)
        {
            g_isDrawingSpotlight = true;
            g_spotlightStart = { x, y };
            g_spotlightRect  = { x, y, x, y };
            SetCapture(hWnd);
            break;
        }

        // 손 도구 패닝 시작 (잠금 시 패닝 비활성화)
        if (!g_screenLock && g_handMode && g_pdfDoc && y >= TOOLBAR_HEIGHT)
        {
            g_isPanning = true;
            g_panStart = { x, y };
            g_panStartX = g_scrollX;
            g_panStartY = g_scrollY;
            SetCapture(hWnd);
            SetCursor(g_curGrab);
        }

        // ★ 텍스트 선택 시작 (손 도구가 아닐 때)
        else if (!g_handMode && g_pdfDoc && !g_pageLayouts.empty())
        {
            RECT cr; GetClientRect(hWnd, &cr);
            const RECT vp = GetViewportRect(cr);
            if (y >= vp.top && y < vp.bottom)
            {
                double pdfX, pdfY;
                int hitPage = HitTestPageAtPoint(x, y, vp, &pdfX, &pdfY);
                if (hitPage >= 0)
                {
                    if (hitPage != g_currentPage)
                        LoadPage(hWnd, hitPage);

                    // 하이라이트 선택 체크
                    int prevSel = g_selectedHighlight;
                    int prevSelUl = g_selectedUnderline;
                    int prevSelSo = g_selectedStrikeout;
                    int prevSelSq = g_selectedSquiggly;
                    g_selectedHighlight = -1;
                    g_selectedUnderline = -1;
                    g_selectedStrikeout = -1;
                    g_selectedSquiggly = -1;
                    if (g_textPage)
                    {
                        double tol = 10.0 / g_zoom;
                        int ci = FPDFText_GetCharIndexAtPos(g_textPage, pdfX, pdfY, tol, tol);
                        if (ci >= 0)
                        {
                            if (!g_highlights.empty())
                                g_selectedHighlight = HitTestHighlight(hitPage, ci);
                            if (g_selectedHighlight < 0 && !g_underlines.empty())
                                g_selectedUnderline = HitTestUnderline(hitPage, ci);
                            if (g_selectedHighlight < 0 && g_selectedUnderline < 0 && !g_strikeouts.empty())
                                g_selectedStrikeout = HitTestStrikeout(hitPage, ci);
                            if (g_selectedHighlight < 0 && g_selectedUnderline < 0 && g_selectedStrikeout < 0 && !g_squigglies.empty())
                                g_selectedSquiggly = HitTestSquiggly(hitPage, ci);
                        }
                    }

                    if (g_selectedHighlight >= 0 || g_selectedUnderline >= 0 || g_selectedStrikeout >= 0 || g_selectedSquiggly >= 0)
                    {
                        // 하이라이트/밑줄을 클릭 → 선택만 하고 텍스트 선택 시작 안 함
                        g_hasSelection = false;
                        g_selCharStart = g_selCharEnd = -1;
                        g_selPage = -1;
                        SetFocus(hWnd);
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    }

                    // 선택 해제됐으면 다시 그리기
                    if (prevSel >= 0 || prevSelUl >= 0 || prevSelSo >= 0 || prevSelSq >= 0)
                        InvalidateRect(hWnd, nullptr, FALSE);

                    if (g_textPage)
                    {
                        double tol = 10.0 / g_zoom;
                        int charIdx = FPDFText_GetCharIndexAtPos(g_textPage, pdfX, pdfY, tol, tol);
                        if (charIdx < 0) {
                            double tol2 = 20.0 / g_zoom;
                            charIdx = FPDFText_GetCharIndexAtPos(g_textPage, pdfX, pdfY, tol2, tol2);
                        }
                        g_selCharStart = g_selCharEnd = (charIdx >= 0) ? charIdx : -1;
                    }
                    else
                    {
                        g_selCharStart = g_selCharEnd = -1;
                    }
                    g_isSelecting = true;
                    g_hasSelection = false;
                    g_selPage = g_currentPage;
                    g_selCaptured = false;
                    g_selDragStart = { x, y };
                    g_selLastMouse = { -1, -1 };
                }
            }
        }
    }
    break;

    case WM_LBUTTONUP:
    {
        int xUp = GET_X_LPARAM(lParam);
        int yUp = GET_Y_LPARAM(lParam);

        // 슬라이드쇼 화살표 클릭 처리
        if (g_slideshowActive)
        {
            RECT cr0; GetClientRect(hWnd, &cr0);
            POINT pt = { xUp, yUp };
            RECT ssLr = GetSSArrowRect(cr0, true);
            RECT ssRr = GetSSArrowRect(cr0, false);
            RECT ssXr = GetSSCloseRect(cr0);
            if (PtInRect(&ssXr, pt))
            {
                SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDM_SLIDESHOW, 0), 0);
                break;
            }
            if (g_currentPage > 0 && PtInRect(&ssLr, pt))
            {
                NavigatePage(hWnd, -1);
                break;
            }
            if (g_currentPage < g_pageCount - 1 && PtInRect(&ssRr, pt))
            {
                NavigatePage(hWnd, +1);
                break;
            }
        }

        if (GroupPanel::IsVisible()) {
            RECT cr; GetClientRect(hWnd, &cr);
            GroupPanel::OnLButtonUp(hWnd, xUp, yUp, cr);
        }

        // 폼 필드 마우스 UP 전달
        if (g_formHandle && g_pdfDoc && g_pdfPage && !g_redactMode && !g_inkMode)
        {
            RECT fCr; GetClientRect(hWnd, &fCr);
            const RECT fVp = GetViewportRect(fCr);
            int pgL, pgT;
            GetPageScreenPos(g_currentPage, fVp, &pgL, &pgT);
            const auto& fLayout = g_pageLayouts[g_currentPage];
            int pgX = xUp - pgL;
            int pgY = yUp - pgT;
            if (pgX >= 0 && pgX < fLayout.renderW && pgY >= 0 && pgY < fLayout.renderH)
            {
                double pdfX = 0, pdfY = 0;
                FormDeviceToPage(pgX, pgY, fLayout.renderW, fLayout.renderH, &pdfX, &pdfY);
                FORM_OnLButtonUp(g_formHandle, g_pdfPage, 0, pdfX, pdfY);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }

        // 잉크 드로잉 완료 → g_inkStrokes에 보관 (PDF 저장은 SaveDocument에서 일괄 처리)
        if (g_isDrawingInk)
        {
            g_isDrawingInk = false;
            ReleaseCapture();
            if (g_inkStroke.size() >= 2 && g_pdfDoc)
            {
                RECT cr0; GetClientRect(hWnd, &cr0);
                const RECT vp0 = GetViewportRect(cr0);
                int pgL, pgT;
                GetPageScreenPos(g_currentPage, vp0, &pgL, &pgT);
                double pageW = g_pageLayouts[g_currentPage].pdfW;
                double pageH = g_pageLayouts[g_currentPage].pdfH;

                InkStrokeItem overlay;
                overlay.pageIndex = g_currentPage;
                overlay.color     = g_inkColor;
                overlay.lineWidth = 2.0f;
                for (auto& sp : g_inkStroke)
                {
                    double px, py;
                    ScreenToPdf(sp.x, sp.y, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(g_currentPage), &px, &py);
                    overlay.pdfPts.emplace_back(px, py);
                }
                g_inkStrokes.push_back(std::move(overlay));
                g_dirty = true;
                g_pageBaseDirty = true;
            }
            g_inkStroke.clear();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // Redact 드래그 완료 → RedactItem 추가
        if (g_isDraggingRedact)
        {
            g_isDraggingRedact = false;
            ReleaseCapture();
            int x1 = std::min(g_redactDragStartX, xUp);
            int y1 = std::min(g_redactDragStartY, yUp);
            int x2 = std::max(g_redactDragStartX, xUp);
            int y2 = std::max(g_redactDragStartY, yUp);
            if (x2 - x1 > 4 && y2 - y1 > 4 && g_pdfPage)
            {
                RECT cr0; GetClientRect(hWnd, &cr0);
                const RECT vp0 = GetViewportRect(cr0);
                int pgL, pgT;
                GetPageScreenPos(g_currentPage, vp0, &pgL, &pgT);
                double pageW = g_pageLayouts[g_currentPage].pdfW;
                double pageH = g_pageLayouts[g_currentPage].pdfH;
                double pdfX1, pdfY1, pdfX2, pdfY2;
                ScreenToPdf(x1, y1, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(g_currentPage), &pdfX1, &pdfY1);
                ScreenToPdf(x2, y2, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(g_currentPage), &pdfX2, &pdfY2);
                RedactItem rd;
                rd.pageIndex = g_currentPage;
                rd.pdfX = std::min(pdfX1, pdfX2);
                rd.pdfY = std::max(pdfY1, pdfY2);
                rd.pdfW = std::abs(pdfX2 - pdfX1);
                rd.pdfH = std::abs(pdfY1 - pdfY2);
                if (rd.pdfW > 0 && rd.pdfH > 0)
                    g_redacts.push_back(rd);
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // 도형 드래그 완료 → ShapeItem 추가
        if (g_isDraggingShape)
        {
            g_isDraggingShape = false;
            ReleaseCapture();
            int x1 = std::min(g_shapeDragStartX, xUp);
            int y1 = std::min(g_shapeDragStartY, yUp);
            int x2 = std::max(g_shapeDragStartX, xUp);
            int y2 = std::max(g_shapeDragStartY, yUp);
            if (x2 - x1 > 4 && y2 - y1 > 4)
            {
                // 드래그한 화면 영역이 어느 페이지에 속하는지 판단
                int midX = (x1 + x2) / 2, midY = (y1 + y2) / 2;
                RECT cr0; GetClientRect(hWnd, &cr0);
                const RECT vp0 = GetViewportRect(cr0);
                int targetPage = g_currentPage;
                for (int pi = 0; pi < g_pageCount; ++pi)
                {
                    int pgL, pgT;
                    GetPageScreenPos(pi, vp0, &pgL, &pgT);
                    int pgR = pgL + g_pageLayouts[pi].renderW;
                    int pgB = pgT + g_pageLayouts[pi].renderH;
                    if (midX >= pgL && midX <= pgR && midY >= pgT && midY <= pgB)
                    { targetPage = pi; break; }
                }
                int pgL, pgT;
                GetPageScreenPos(targetPage, vp0, &pgL, &pgT);
                double pageW = g_pageLayouts[targetPage].pdfW;
                double pageH = g_pageLayouts[targetPage].pdfH;
                double pdfX1, pdfY1, pdfX2, pdfY2;
                ScreenToPdf(x1, y1, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(targetPage), &pdfX1, &pdfY1);
                ScreenToPdf(x2, y2, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(targetPage), &pdfX2, &pdfY2);
                ShapeItem sh;
                sh.pageIndex = targetPage;
                sh.isCircle  = (g_shapeMode == 2);
                sh.color     = g_shapeColor;
                sh.lineWidth = g_shapeLineWidth;
                sh.pdfX = std::min(pdfX1, pdfX2);
                sh.pdfY = std::max(pdfY1, pdfY2);  // 상단 (하단원점)
                sh.pdfW = std::abs(pdfX2 - pdfX1);
                sh.pdfH = std::abs(pdfY1 - pdfY2);
                if (sh.pdfW > 0 && sh.pdfH > 0)
                {
                    g_shapes.push_back(sh);
                    g_dirty = true;
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
    }
        if (g_thumbSbDragging)
        {
            g_thumbSbDragging = false;
            ReleaseCapture();
            break;
        }

        if (g_isDrawingSpotlight)
        {
            g_isDrawingSpotlight = false;
            ReleaseCapture();
            // 너무 작은 드래그는 무시
            if (abs(g_spotlightRect.right - g_spotlightRect.left) > 5 &&
                abs(g_spotlightRect.bottom - g_spotlightRect.top) > 5)
                g_hasSpotlight = true;
            else
                g_hasSpotlight = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        if (g_isDraggingNote)
        {
            g_isDraggingNote  = false;
            g_draggingNoteIdx = -1;
            ReleaseCapture();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        if (g_isDraggingTextBox)
        {
            g_isDraggingTextBox = false;
            ReleaseCapture();
            RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT;
            InvalidateRect(hWnd, &ir, FALSE);
            break;
        }

        if (g_isResizingTextBox)
        {
            g_isResizingTextBox = false;
            g_resizeHandle = -1;
            ReleaseCapture();
            // 숨겼던 Edit 컨트롤을 다시 표시하고 크기/위치 재동기화
            if (g_hTextBoxEdit && g_editingTextBox == g_selectedTextBox)
            {
                const int B = 2;
                RECT nr = TextBoxScreenRect(g_textBoxes[g_selectedTextBox]);
                SetWindowPos(g_hTextBoxEdit, nullptr, nr.left + B, nr.top + B,
                    (nr.right - nr.left) - B * 2, (nr.bottom - nr.top) - B * 2,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                ShowWindow(g_hTextBoxEdit, SW_SHOW);
            }
            RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT;
            InvalidateRect(hWnd, &ir, FALSE);
            break;
        }

        if (g_isDraggingSig)
        {
            g_isDraggingSig = false;
            ReleaseCapture();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        if (g_isResizingSig)
        {
            g_isResizingSig    = false;
            g_sigResizeHandle  = -1;
            g_sigResizeInitStrokes.clear();
            ReleaseCapture();
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        if (g_isPanning)
        {
            g_isPanning = false;
            ReleaseCapture();
            SetCursor(g_curHand);
        }

        if (g_isSelecting)
        {
            g_isSelecting = false;
            g_hasSelection = (g_selCharStart >= 0 && g_selCharEnd >= 0
                && g_selCharStart != g_selCharEnd);
            if (g_selCaptured) { ReleaseCapture(); g_selCaptured = false; }
            // 하이라이트 모드: 선택 완료 즉시 하이라이트 적용
            if (g_hasSelection && g_highlightMode)
                AddHighlightFromSelection(g_highlightColor);
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        // 툴바 버튼: UP에서 명령 실행
        if (g_toolbarPressedCmd != 0)
        {
            UINT cmd = g_toolbarPressedCmd;
            g_toolbarPressedCmd = 0;
            g_toolbarPressedBtn = -1;
            ReleaseCapture();
            SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
        }
    break;

    case WM_LBUTTONDBLCLK:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        if (g_pdfDoc && !g_textBoxes.empty())
        {
            RECT crtb; GetClientRect(hWnd, &crtb);
            const RECT vptb = GetViewportRect(crtb);
            int ti = HitTestTextBox(mx, my, vptb);
            if (ti >= 0)
            {
                StartTextBoxEdit(hWnd, ti);
                break;
            }
        }

        if (g_pdfDoc && !g_notes.empty())
        {
            RECT crn; GetClientRect(hWnd, &crn);
            const RECT vpn = GetViewportRect(crn);
            int ni = HitTestNote(mx, my, vpn);
            if (ni >= 0)
            {
                std::wstring txt = g_notes[ni].text;
                if (CleanPopup::ShowNoteInput(hWnd, LS(STR_NOTE_EDIT), txt) && !txt.empty())
                {
                    g_notes[ni].text = txt;
                    g_dirty = true;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
                break;
            }
        }

        // 단어 더블클릭 → 단어 선택
        if (!g_handMode && g_pdfDoc && !g_pageLayouts.empty())
        {
            RECT cr; GetClientRect(hWnd, &cr);
            const RECT vp = GetViewportRect(cr);
            if (my >= vp.top && my < vp.bottom)
            {
                double pdfX, pdfY;
                int hitPage = HitTestPageAtPoint(mx, my, vp, &pdfX, &pdfY);
                if (hitPage >= 0)
                {
                    if (hitPage != g_currentPage)
                        LoadPage(hWnd, hitPage);
                    if (g_textPage)
                    {
                        double tol = 5.0 / g_zoom;
                        int charIdx = FPDFText_GetCharIndexAtPos(g_textPage, pdfX, pdfY, tol, tol);
                        const int totalChars = FPDFText_CountChars(g_textPage);
                        if (charIdx >= 0 && charIdx < totalChars)
                        {
                            auto isWordChar = [&](int idx) -> bool {
                                if (idx < 0 || idx >= totalChars) return false;
                                unsigned int ch = FPDFText_GetUnicode(g_textPage, idx);
                                if (ch >= L'\uAC00' && ch <= L'\uD7A3') return true;
                                if (ch >= 0x4E00 && ch <= 0x9FFF) return true;
                                if (ch >= 0x3400 && ch <= 0x4DBF) return true;
                                if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')) return true;
                                if (ch >= L'0' && ch <= L'9') return true;
                                if (ch == L'_') return true;
                                return false;
                            };
                            if (isWordChar(charIdx))
                            {
                                int wordStart = charIdx;
                                while (wordStart > 0 && isWordChar(wordStart - 1)) --wordStart;
                                int wordEnd = charIdx;
                                while (wordEnd < totalChars - 1 && isWordChar(wordEnd + 1)) ++wordEnd;
                                g_selCharStart = wordStart;
                                g_selCharEnd   = wordEnd;
                                g_hasSelection = (wordStart != wordEnd);
                                g_selPage      = g_currentPage;
                                g_isSelecting  = false;
                                InvalidateRect(hWnd, nullptr, FALSE);
                            }
                        }
                    }
                }
            }
        }
    }
    break;

    case WM_CAPTURECHANGED:
    {
        // 어떤 이유로든 캡처가 해제되면 모든 드래그 상태를 초기화한다.
        // 이로 인해 WM_MOUSEMOVE에서 SetCapture가 재호출되는 것을 막는다.
        g_isPanning         = false;
        g_isSelecting       = false;
        g_selCaptured       = false;
        g_isDraggingNote    = false;
        g_draggingNoteIdx   = -1;
        g_isDraggingTextBox = false;
        g_isResizingTextBox = false;
        g_isDraggingSig     = false;
        g_isResizingSig     = false;
        g_toolbarPressedBtn = -1;
        g_toolbarPressedCmd = 0;
    }
    break;

    case WM_MBUTTONUP:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        if (!g_tabs.empty())
        {
            bool closeBtn = false;
            int tabHit = HitTestTabBar(mx, my, &closeBtn);
            if (tabHit >= 0)
                CloseTab(hWnd, tabHit);
        }
    }
    break;

    case WM_RBUTTONDOWN:
    {
        // 잠금 시에도 우클릭 컨텍스트 메뉴(주석 삭제, 복사 등)는 허용
        // 드래그/캡처 상태를 즉시 정리
        if (GetCapture() == hWnd)
            ReleaseCapture();
        g_isPanning         = false;
        g_isSelecting       = false;
        g_selCaptured       = false;
        g_isDraggingNote    = false;
        g_draggingNoteIdx   = -1;
        g_isDraggingTextBox = false;
        g_isResizingTextBox = false;
        g_isDraggingSig     = false;
        g_isResizingSig     = false;

        // 메시지 큐를 거치지 않고 즉시 WM_CONTEXTMENU를 동기 호출한다.
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &pt);
        g_contextMenuPt = pt;
        SendMessage(hWnd, WM_CONTEXTMENU, (WPARAM)hWnd, MAKELPARAM((WORD)pt.x, (WORD)pt.y));
    }
    return 0;

    case WM_RBUTTONUP:
        // WM_RBUTTONDOWN에서 이미 처리했으므로 DefWindowProc 차단
        return 0;

    case WM_CONTEXTMENU:
    {
        POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (ptScreen.x == -1 && ptScreen.y == -1)
        {
            // 키보드(Shift+F10)로 열린 경우 → 윈도우 중앙
            RECT rc; GetClientRect(hWnd, &rc);
            ptScreen = { rc.right / 2, rc.bottom / 2 };
            ClientToScreen(hWnd, &ptScreen);
        }
        g_contextMenuPt = {};

        POINT ptClient = ptScreen;
        ScreenToClient(hWnd, &ptClient);
        int mx = ptClient.x;
        int my = ptClient.y;

        {
            RECT crTh; GetClientRect(hWnd, &crTh);
            const int thIdx = ThumbnailHitTest(mx, my, crTh);
            if (thIdx >= 0)
            {
                HMENU hTh = CreatePopupMenu();
                if (!hTh) return 0;
                AppendMenuW(hTh, MF_STRING, 11, LS(STR_CTX_PAGE_ROT_CW));
                AppendMenuW(hTh, MF_STRING, 12, LS(STR_CTX_PAGE_ROT_CCW));
                AppendMenuW(hTh, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hTh, MF_STRING, 13, LS(STR_CTX_PAGE_EXPORT));
                int thCmd = TrackPopupMenu(hTh, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                    ptScreen.x, ptScreen.y, 0, hWnd, nullptr);
                DestroyMenu(hTh);
                if (thCmd == 11 || thCmd == 12)
                    RotateSinglePage(hWnd, thIdx, thCmd == 11 ? 1 : 3);
                else if (thCmd == 13)
                    ExportPagesAsImages(hWnd, thIdx + 1);
                return 0;
            }
            if (g_sidebarVisible && mx >= 0 && mx < SIDEBAR_WIDTH && g_sidebarTab == TAB_THUMBNAILS)
                return 0;
        }

        HMENU hPopup = CreatePopupMenu();
        if (!hPopup) return 0;

        // 복사 (선택 영역이 있을 때만 활성화)
        AppendMenuW(hPopup, MF_STRING | (g_hasSelection ? 0 : MF_GRAYED),
            1, LS(STR_CTX_COPY));

        // 구글 검색 (선택 영역이 있을 때만 활성화)
        AppendMenuW(hPopup, MF_STRING | (g_hasSelection ? 0 : MF_GRAYED),
            10, LS(STR_CTX_GOOGLE));

        // 전체 선택
        AppendMenuW(hPopup, MF_STRING | (g_textPage ? 0 : MF_GRAYED),
            2, LS(STR_CTX_SELECT_ALL));

        AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);

        // ── 하이라이트 서브메뉴 ──
        g_colorMenuItems.clear();
        g_colorMenuItems.reserve(32); // 재할당으로 인한 포인터 무효화 방지
        {
            HMENU hHighlight = CreatePopupMenu();
            bool sel = g_hasSelection;
            AppendColorMenuItem(hHighlight, 20, ANNOT_COLOR_YELLOW,   LS(STR_COLOR_YELLOW),   sel);
            AppendColorMenuItem(hHighlight, 21, ANNOT_COLOR_GREEN,    LS(STR_COLOR_GREEN),    sel);
            AppendColorMenuItem(hHighlight, 22, ANNOT_COLOR_BLUE,     LS(STR_COLOR_BLUE),     sel);
            AppendColorMenuItem(hHighlight, 23, ANNOT_COLOR_ORANGE,   LS(STR_COLOR_ORANGE),   sel);
            AppendColorMenuItem(hHighlight, 24, ANNOT_COLOR_PINK,     LS(STR_COLOR_PINK),     sel);
            AppendColorMenuItem(hHighlight, 26, ANNOT_COLOR_LAVENDER, LS(STR_COLOR_LAVENDER), sel);
            AppendColorMenuItem(hHighlight, 27, ANNOT_COLOR_EMERALD,  LS(STR_COLOR_EMERALD),  sel);
            AppendColorMenuItem(hHighlight, 28, ANNOT_COLOR_SLATE,    LS(STR_COLOR_SLATE),    sel);
            AppendMenuW(hPopup, MF_POPUP | (g_hasSelection ? 0 : MF_GRAYED),
                reinterpret_cast<UINT_PTR>(hHighlight), LS(STR_CTX_HIGHLIGHT));

            // 우클릭 위치에 하이라이트가 있으면 삭제 옵션 표시
            int hitHlIdx = -1;
            if (g_textPage && g_pdfDoc)
            {
                RECT cr5; GetClientRect(hWnd, &cr5);
                const RECT vp5 = GetViewportRect(cr5);
                double pdfX5, pdfY5;
                int hitPage5 = HitTestPageAtPoint(mx, my, vp5, &pdfX5, &pdfY5);
                if (hitPage5 >= 0)
                {
                    FPDF_TEXTPAGE tp5 = (hitPage5 == g_currentPage) ? g_textPage : nullptr;
                    FPDF_PAGE pg5 = (hitPage5 == g_currentPage) ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, hitPage5);
                    if (!tp5 && pg5) tp5 = FPDFText_LoadPage(pg5);
                    if (tp5)
                    {
                        double tol5 = 5.0 / g_zoom;
                        int ci5 = FPDFText_GetCharIndexAtPos(tp5, pdfX5, pdfY5, tol5, tol5);
                        if (ci5 >= 0) hitHlIdx = HitTestHighlight(hitPage5, ci5);
                    }
                    if (tp5 && tp5 != g_textPage) FPDFText_ClosePage(tp5);
                    if (pg5 && pg5 != g_pdfPage)  FPDF_ClosePage(pg5);
                }
            }
            if (hitHlIdx >= 0)
            {
                AppendMenuW(hPopup, MF_STRING, 25, LS(STR_CTX_HL_DEL));
            }
        }

        // ── 밑줄 서브메뉴 ──
        {
            HMENU hUnderline = CreatePopupMenu();
            bool sel = g_hasSelection;
            AppendColorMenuItem(hUnderline, 30, ANNOT_COLOR_YELLOW,   LS(STR_COLOR_YELLOW),   sel);
            AppendColorMenuItem(hUnderline, 31, ANNOT_COLOR_GREEN,    LS(STR_COLOR_GREEN),    sel);
            AppendColorMenuItem(hUnderline, 32, ANNOT_COLOR_BLUE,     LS(STR_COLOR_BLUE),     sel);
            AppendColorMenuItem(hUnderline, 33, ANNOT_COLOR_ORANGE,   LS(STR_COLOR_ORANGE),   sel);
            AppendColorMenuItem(hUnderline, 34, ANNOT_COLOR_PINK,     LS(STR_COLOR_PINK),     sel);
            AppendColorMenuItem(hUnderline, 36, ANNOT_COLOR_LAVENDER, LS(STR_COLOR_LAVENDER), sel);
            AppendColorMenuItem(hUnderline, 37, ANNOT_COLOR_EMERALD,  LS(STR_COLOR_EMERALD),  sel);
            AppendColorMenuItem(hUnderline, 38, ANNOT_COLOR_SLATE,    LS(STR_COLOR_SLATE),    sel);
            AppendMenuW(hPopup, MF_POPUP | (g_hasSelection ? 0 : MF_GRAYED),
                reinterpret_cast<UINT_PTR>(hUnderline), LS(STR_CTX_UNDERLINE));

            // 우클릭 위치에 밑줄이 있으면 삭제 옵션 표시
            int hitUlIdx = -1;
            if (g_textPage && g_pdfDoc)
            {
                RECT cr5u; GetClientRect(hWnd, &cr5u);
                const RECT vp5u = GetViewportRect(cr5u);
                double pdfX5u, pdfY5u;
                int hitPage5u = HitTestPageAtPoint(mx, my, vp5u, &pdfX5u, &pdfY5u);
                if (hitPage5u >= 0)
                {
                    FPDF_TEXTPAGE tp5u = (hitPage5u == g_currentPage) ? g_textPage : nullptr;
                    FPDF_PAGE pg5u = (hitPage5u == g_currentPage) ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, hitPage5u);
                    if (!tp5u && pg5u) tp5u = FPDFText_LoadPage(pg5u);
                    if (tp5u)
                    {
                        double tol5u = 5.0 / g_zoom;
                        int ci5u = FPDFText_GetCharIndexAtPos(tp5u, pdfX5u, pdfY5u, tol5u, tol5u);
                        if (ci5u >= 0) hitUlIdx = HitTestUnderline(hitPage5u, ci5u);
                    }
                    if (tp5u && tp5u != g_textPage) FPDFText_ClosePage(tp5u);
                    if (pg5u && pg5u != g_pdfPage)  FPDF_ClosePage(pg5u);
                }
            }
            if (hitUlIdx >= 0)
            {
                AppendMenuW(hPopup, MF_STRING, 35, LS(STR_CTX_UL_DEL));
            }
        }

        // ── 취소선 서브메뉴 ──
        {
            HMENU hStrikeout = CreatePopupMenu();
            bool sel = g_hasSelection;
            AppendColorMenuItem(hStrikeout, 40, ANNOT_COLOR_YELLOW,   LS(STR_COLOR_YELLOW),   sel);
            AppendColorMenuItem(hStrikeout, 41, ANNOT_COLOR_GREEN,    LS(STR_COLOR_GREEN),    sel);
            AppendColorMenuItem(hStrikeout, 42, ANNOT_COLOR_BLUE,     LS(STR_COLOR_BLUE),     sel);
            AppendColorMenuItem(hStrikeout, 43, ANNOT_COLOR_ORANGE,   LS(STR_COLOR_ORANGE),   sel);
            AppendColorMenuItem(hStrikeout, 44, ANNOT_COLOR_PINK,     LS(STR_COLOR_PINK),     sel);
            AppendColorMenuItem(hStrikeout, 46, ANNOT_COLOR_LAVENDER, LS(STR_COLOR_LAVENDER), sel);
            AppendColorMenuItem(hStrikeout, 47, ANNOT_COLOR_EMERALD,  LS(STR_COLOR_EMERALD),  sel);
            AppendColorMenuItem(hStrikeout, 48, ANNOT_COLOR_SLATE,    LS(STR_COLOR_SLATE),    sel);
            AppendMenuW(hPopup, MF_POPUP | (g_hasSelection ? 0 : MF_GRAYED),
                reinterpret_cast<UINT_PTR>(hStrikeout), LS(STR_CTX_STRIKEOUT));

            // 우클릭 위치에 취소선이 있으면 삭제 옵션 표시
            int hitSoIdx = -1;
            if (g_textPage && g_pdfDoc)
            {
                RECT cr5s; GetClientRect(hWnd, &cr5s);
                const RECT vp5s = GetViewportRect(cr5s);
                double pdfX5s, pdfY5s;
                int hitPage5s = HitTestPageAtPoint(mx, my, vp5s, &pdfX5s, &pdfY5s);
                if (hitPage5s >= 0)
                {
                    FPDF_TEXTPAGE tp5s = (hitPage5s == g_currentPage) ? g_textPage : nullptr;
                    FPDF_PAGE pg5s = (hitPage5s == g_currentPage) ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, hitPage5s);
                    if (!tp5s && pg5s) tp5s = FPDFText_LoadPage(pg5s);
                    if (tp5s)
                    {
                        double tol5s = 5.0 / g_zoom;
                        int ci5s = FPDFText_GetCharIndexAtPos(tp5s, pdfX5s, pdfY5s, tol5s, tol5s);
                        if (ci5s >= 0) hitSoIdx = HitTestStrikeout(hitPage5s, ci5s);
                    }
                    if (tp5s && tp5s != g_textPage) FPDFText_ClosePage(tp5s);
                    if (pg5s && pg5s != g_pdfPage)  FPDF_ClosePage(pg5s);
                }
            }
            if (hitSoIdx >= 0)
            {
                AppendMenuW(hPopup, MF_STRING, 45, LS(STR_CTX_SO_DEL));
            }
        }

        // ── 물결 밑줄 서브메뉴 ──
        {
            HMENU hSquiggly = CreatePopupMenu();
            bool sel = g_hasSelection;
            AppendColorMenuItem(hSquiggly, 50, ANNOT_COLOR_YELLOW,   LS(STR_COLOR_YELLOW),   sel);
            AppendColorMenuItem(hSquiggly, 51, ANNOT_COLOR_GREEN,    LS(STR_COLOR_GREEN),    sel);
            AppendColorMenuItem(hSquiggly, 52, ANNOT_COLOR_BLUE,     LS(STR_COLOR_BLUE),     sel);
            AppendColorMenuItem(hSquiggly, 53, ANNOT_COLOR_ORANGE,   LS(STR_COLOR_ORANGE),   sel);
            AppendColorMenuItem(hSquiggly, 54, ANNOT_COLOR_PINK,     LS(STR_COLOR_PINK),     sel);
            AppendColorMenuItem(hSquiggly, 56, ANNOT_COLOR_LAVENDER, LS(STR_COLOR_LAVENDER), sel);
            AppendColorMenuItem(hSquiggly, 57, ANNOT_COLOR_EMERALD,  LS(STR_COLOR_EMERALD),  sel);
            AppendColorMenuItem(hSquiggly, 58, ANNOT_COLOR_SLATE,    LS(STR_COLOR_SLATE),    sel);
            AppendMenuW(hPopup, MF_POPUP | (g_hasSelection ? 0 : MF_GRAYED),
                reinterpret_cast<UINT_PTR>(hSquiggly), LS(STR_CTX_SQUIGGLY));

            // 우클릭 위치에 물결 밑줄이 있으면 삭제 옵션 표시
            int hitSqIdx = -1;
            if (g_textPage && g_pdfDoc)
            {
                RECT cr5q; GetClientRect(hWnd, &cr5q);
                const RECT vp5q = GetViewportRect(cr5q);
                double pdfX5q, pdfY5q;
                int hitPage5q = HitTestPageAtPoint(mx, my, vp5q, &pdfX5q, &pdfY5q);
                if (hitPage5q >= 0)
                {
                    FPDF_TEXTPAGE tp5q = (hitPage5q == g_currentPage) ? g_textPage : nullptr;
                    FPDF_PAGE pg5q = (hitPage5q == g_currentPage) ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, hitPage5q);
                    if (!tp5q && pg5q) tp5q = FPDFText_LoadPage(pg5q);
                    if (tp5q)
                    {
                        double tol5q = 5.0 / g_zoom;
                        int ci5q = FPDFText_GetCharIndexAtPos(tp5q, pdfX5q, pdfY5q, tol5q, tol5q);
                        if (ci5q >= 0) hitSqIdx = HitTestSquiggly(hitPage5q, ci5q);
                    }
                    if (tp5q && tp5q != g_textPage) FPDFText_ClosePage(tp5q);
                    if (pg5q && pg5q != g_pdfPage)  FPDF_ClosePage(pg5q);
                }
            }
            if (hitSqIdx >= 0)
            {
                AppendMenuW(hPopup, MF_STRING, 55, LS(STR_CTX_SQ_DEL));
            }
        }

        AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);

        // ── 메모 추가 / 삭제 ──
        {
            RECT crn; GetClientRect(hWnd, &crn);
            const RECT vpn = GetViewportRect(crn);
            AppendMenuW(hPopup, MF_STRING | (g_pdfDoc ? 0 : MF_GRAYED),
                    60, LS(STR_CTX_NOTE_ADD));

                int hitNoteIdx = HitTestNote(mx, my, vpn);
                if (hitNoteIdx >= 0)
                {
                    AppendMenuW(hPopup, MF_STRING, 61, LS(STR_CTX_NOTE_EDIT));
                    AppendMenuW(hPopup, MF_STRING, 62, LS(STR_CTX_NOTE_DEL));
                }
            }

            // ── 텍스트 박스 ──
            AppendMenuW(hPopup, MF_STRING | (g_pdfDoc ? 0 : MF_GRAYED),
                70, LS(STR_CTX_TEXTBOX_ADD));

            {
                RECT crtb; GetClientRect(hWnd, &crtb);
                const RECT vptb = GetViewportRect(crtb);
                int hitTb = HitTestTextBox(mx, my, vptb);
                if (hitTb >= 0)
                    AppendMenuW(hPopup, MF_STRING, 71, LS(STR_CTX_TEXTBOX_DEL));
            }

            // ── 도형 ──
            if (!g_shapes.empty())
            {
                RECT crsh; GetClientRect(hWnd, &crsh);
                const RECT vpsh = GetViewportRect(crsh);
                for (int si = (int)g_shapes.size() - 1; si >= 0; si--)
                {
                    const auto& sh = g_shapes[si];
                    if (sh.pageIndex < 0 || sh.pageIndex >= (int)g_pageLayouts.size()) continue;
                    int pgL, pgT;
                    GetPageScreenPos(sh.pageIndex, vpsh, &pgL, &pgT);
                    double pageW = g_pageLayouts[sh.pageIndex].pdfW;
                    double pageH = g_pageLayouts[sh.pageIndex].pdfH;
                    int sx1, sy1, sx2, sy2;
                    PdfToScreen(sh.pdfX,           sh.pdfY,           pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sh.pageIndex), &sx1, &sy1);
                    PdfToScreen(sh.pdfX + sh.pdfW, sh.pdfY - sh.pdfH, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sh.pageIndex), &sx2, &sy2);
                    RECT rsh = { std::min(sx1,sx2) - 4, std::min(sy1,sy2) - 4,
                                 std::max(sx1,sx2) + 4, std::max(sy1,sy2) + 4 };
                    POINT ptsh = { mx, my };
                    if (PtInRect(&rsh, ptsh))
                    {
                        AppendMenuW(hPopup, MF_STRING, 80, L"도형 삭제");
                        break;
                    }
                }
            }

        AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);

        // 확대/축소
        AppendMenuW(hPopup, MF_STRING | (g_pdfDoc ? 0 : MF_GRAYED),
            3, LS(STR_CTX_ZOOM_IN));
        AppendMenuW(hPopup, MF_STRING | (g_pdfDoc ? 0 : MF_GRAYED),
            4, LS(STR_CTX_ZOOM_OUT));
        AppendMenuW(hPopup, MF_STRING | (g_pdfDoc ? 0 : MF_GRAYED),
            5, LS(STR_CTX_ZOOM_100));

        AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);

        // 회전
        AppendMenuW(hPopup, MF_STRING | (g_pdfDoc ? 0 : MF_GRAYED),
            6, LS(STR_CTX_ROT_CW));
        AppendMenuW(hPopup, MF_STRING | (g_pdfDoc ? 0 : MF_GRAYED),
            7, LS(STR_CTX_ROT_CCW));
        {
            RECT crR; GetClientRect(hWnd, &crR);
            const RECT vpR = GetViewportRect(crR);
            const bool onPage = g_pdfDoc && HitTestPageAtPoint(mx, my, vpR, nullptr, nullptr) >= 0;
            AppendMenuW(hPopup, MF_STRING | (onPage ? 0 : MF_GRAYED), 11,
                LS(STR_CTX_PAGE_ROT_CW));
            AppendMenuW(hPopup, MF_STRING | (onPage ? 0 : MF_GRAYED), 12,
                LS(STR_CTX_PAGE_ROT_CCW));
        }

        AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);

        // 야간 모드
        AppendMenuW(hPopup, MF_STRING | (g_darkMode ? MF_CHECKED : 0),
            8, LS(STR_CTX_DARK_MODE));

        // 손 도구
        AppendMenuW(hPopup, MF_STRING | (g_handMode ? MF_CHECKED : 0) | (g_pdfDoc ? 0 : MF_GRAYED),
            9, LS(STR_CTX_HAND));

        POINT pt = ptScreen;
        int cmd = TrackPopupMenu(hPopup, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            pt.x, pt.y, 0, hWnd, nullptr);
        DestroyMenu(hPopup);
        switch (cmd)
        {
        case 1: CopySelectionToClipboard(hWnd); break;
        case 10: // 구글 검색
        {
            if (!g_hasSelection || !g_textPage) break;
            const int firstIdx = std::min(g_selCharStart, g_selCharEnd);
            const int lastIdx  = std::max(g_selCharStart, g_selCharEnd);
            const int count    = lastIdx - firstIdx + 1;
            if (count <= 0) break;

            // 선택 텍스트 추출
            const int selPage = (g_selPage >= 0) ? g_selPage : g_currentPage;
            FPDF_PAGE     qPage     = nullptr;
            FPDF_TEXTPAGE qTextPage = nullptr;
            bool qNeedClose = false;
            if (selPage == g_currentPage && g_pdfPage && g_textPage) {
                qPage = g_pdfPage; qTextPage = g_textPage;
            } else {
                qPage = FPDF_LoadPage(g_pdfDoc, selPage);
                if (qPage) { qTextPage = FPDFText_LoadPage(qPage); qNeedClose = true; }
            }
            if (!qTextPage) { if (qNeedClose && qPage) FPDF_ClosePage(qPage); break; }

            std::vector<WCHAR> buf(count + 2, 0);
            FPDFText_GetText(qTextPage, firstIdx, count,
                reinterpret_cast<unsigned short*>(buf.data()));
            if (qNeedClose) { FPDFText_ClosePage(qTextPage); FPDF_ClosePage(qPage); }

            // URL 인코딩 후 ShellExecute
            std::wstring query(buf.data());
            // 공백 → +, 특수문자 → %XX (기본 인코딩)
            std::wstring encoded;
            encoded.reserve(query.size() * 3);
            for (wchar_t ch : query) {
                if (ch == L' ') { encoded += L'+'; }
                else if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
                         (ch >= L'0' && ch <= L'9') ||
                         ch == L'-' || ch == L'_' || ch == L'.' || ch == L'~') {
                    encoded += ch;
                } else {
                    // UTF-8로 변환 후 % 인코딩
                    char mb[5] = {};
                    int mbLen = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
                    for (int i = 0; i < mbLen; ++i) {
                        WCHAR hex[4];
                        swprintf_s(hex, L"%%%02X", (unsigned char)mb[i]);
                        encoded += hex;
                    }
                }
            }
            std::wstring url = L"https://www.google.com/search?q=" + encoded;
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case 2: // 전체 선택
            if (g_textPage && g_pdfPage) {
                int charCount = FPDFText_CountChars(g_textPage);
                if (charCount > 0) {
                    g_selCharStart = 0;
                    g_selCharEnd = charCount - 1;
                    g_hasSelection = true;
                    g_isSelecting = false;
                    g_selPage = g_currentPage;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
            break;
        case 3: SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_ZOOM_IN, 0), 0); break;
        case 11:
        case 12:
        {
            RECT crR; GetClientRect(hWnd, &crR);
            const RECT vpR = GetViewportRect(crR);
            int pgR = HitTestPageAtPoint(mx, my, vpR, nullptr, nullptr);
            if (pgR >= 0) RotateSinglePage(hWnd, pgR, cmd == 11 ? 1 : 3);
            break;
        }
        case 60: // 메모 추가
        {
            RECT crn2; GetClientRect(hWnd, &crn2);
            const RECT vpn2 = GetViewportRect(crn2);
            double pdfXn, pdfYn;
            int hitPagen = HitTestPageAtPoint(mx, my, vpn2, &pdfXn, &pdfYn);
            if (hitPagen >= 0)
                AddNoteAtPdfPos(hWnd, hitPagen, pdfXn, pdfYn);
            break;
        }
        case 61: // 메모 편집
        {
            RECT crn3; GetClientRect(hWnd, &crn3);
            const RECT vpn3 = GetViewportRect(crn3);
            int ni = HitTestNote(mx, my, vpn3);
            if (ni >= 0)
            {
                std::wstring txt = g_notes[ni].text;
                if (CleanPopup::ShowNoteInput(hWnd, LS(STR_NOTE_EDIT), txt) && !txt.empty())
                {
                    g_notes[ni].text = txt;
                    g_dirty = true;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
            break;
        }
        case 62: // 메모 삭제
        {
            RECT crn4; GetClientRect(hWnd, &crn4);
            const RECT vpn4 = GetViewportRect(crn4);
            int ni = HitTestNote(mx, my, vpn4);
            if (ni >= 0)
            {
                g_notes.erase(g_notes.begin() + ni);
                if (g_selectedNote >= (int)g_notes.size()) g_selectedNote = -1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }
        case 70: // 텍스트 박스 추가
        {
            RECT crtb2; GetClientRect(hWnd, &crtb2);
            const RECT vptb2 = GetViewportRect(crtb2);
            double pdfXtb, pdfYtb;
            int hitPageTb = HitTestPageAtPoint(mx, my, vptb2, &pdfXtb, &pdfYtb);
            if (hitPageTb >= 0)
                AddTextBoxAtPdfPos(hWnd, hitPageTb, pdfXtb, pdfYtb);
            break;
        }
        case 71: // 텍스트 박스 삭제
        {
            RECT crtb3; GetClientRect(hWnd, &crtb3);
            const RECT vptb3 = GetViewportRect(crtb3);
            int ti = HitTestTextBox(mx, my, vptb3);
            if (ti >= 0)
            {
                if (g_editingTextBox == ti) CommitTextBoxEdit();
                g_textBoxes.erase(g_textBoxes.begin() + ti);
                if (g_selectedTextBox >= (int)g_textBoxes.size()) g_selectedTextBox = -1;
                g_dirty = true;
                RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT;
                InvalidateRect(hWnd, &ir, FALSE);
            }
            break;
        }
        case 80: // 도형 삭제 (우클릭)
        {
            RECT crsh2; GetClientRect(hWnd, &crsh2);
            const RECT vpsh2 = GetViewportRect(crsh2);
            for (int si = (int)g_shapes.size() - 1; si >= 0; si--)
            {
                const auto& sh = g_shapes[si];
                if (sh.pageIndex < 0 || sh.pageIndex >= (int)g_pageLayouts.size()) continue;
                int pgL, pgT;
                GetPageScreenPos(sh.pageIndex, vpsh2, &pgL, &pgT);
                double pageW = g_pageLayouts[sh.pageIndex].pdfW;
                double pageH = g_pageLayouts[sh.pageIndex].pdfH;
                int sx1, sy1, sx2, sy2;
                PdfToScreen(sh.pdfX,           sh.pdfY,           pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sh.pageIndex), &sx1, &sy1);
                PdfToScreen(sh.pdfX + sh.pdfW, sh.pdfY - sh.pdfH, pageW, pageH, pgL, pgT, g_zoom, GetEffectiveRotation(sh.pageIndex), &sx2, &sy2);
                RECT rsh2 = { std::min(sx1,sx2) - 4, std::min(sy1,sy2) - 4,
                              std::max(sx1,sx2) + 4, std::max(sy1,sy2) + 4 };
                POINT ptsh2 = { mx, my };
                if (PtInRect(&rsh2, ptsh2))
                {
                    if (g_selectedShape == si) g_selectedShape = -1;
                    g_shapes.erase(g_shapes.begin() + si);
                    g_dirty = true;
                    InvalidateRect(hWnd, nullptr, FALSE);
                    break;
                }
            }
            break;
        }
        case 4: SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_ZOOM_OUT, 0), 0); break;
        case 5: SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_ZOOM_RESET, 0), 0); break;
        case 6: SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_ROTATE_CW, 0), 0); break;
        case 7: SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_ROTATE_CCW, 0), 0); break;
        case 8: SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_DARK_MODE, 0), 0); break;
        case 9: SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_HAND_TOOL, 0), 0); break;
        case 20: AddHighlightFromSelection(ANNOT_COLOR_YELLOW);   InvalidateRect(hWnd, nullptr, FALSE); break;
        case 21: AddHighlightFromSelection(ANNOT_COLOR_GREEN);    InvalidateRect(hWnd, nullptr, FALSE); break;
        case 22: AddHighlightFromSelection(ANNOT_COLOR_BLUE);     InvalidateRect(hWnd, nullptr, FALSE); break;
        case 23: AddHighlightFromSelection(ANNOT_COLOR_ORANGE);   InvalidateRect(hWnd, nullptr, FALSE); break;
        case 24: AddHighlightFromSelection(ANNOT_COLOR_PINK);     InvalidateRect(hWnd, nullptr, FALSE); break;
        case 26: AddHighlightFromSelection(ANNOT_COLOR_LAVENDER); InvalidateRect(hWnd, nullptr, FALSE); break;
        case 27: AddHighlightFromSelection(ANNOT_COLOR_EMERALD);  InvalidateRect(hWnd, nullptr, FALSE); break;
        case 28: AddHighlightFromSelection(ANNOT_COLOR_SLATE);    InvalidateRect(hWnd, nullptr, FALSE); break;
        case 25: // 하이라이트 삭제
        {
            // hitHlIdx를 다시 계산
            if (g_textPage && g_pdfDoc)
            {
                RECT cr6; GetClientRect(hWnd, &cr6);
                const RECT vp6 = GetViewportRect(cr6);
                double pdfX6, pdfY6;
                int hitPage6 = HitTestPageAtPoint(mx, my, vp6, &pdfX6, &pdfY6);
                if (hitPage6 >= 0)
                {
                    FPDF_TEXTPAGE tp6 = (hitPage6 == g_currentPage) ? g_textPage : nullptr;
                    FPDF_PAGE pg6 = (hitPage6 == g_currentPage) ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, hitPage6);
                    if (!tp6 && pg6) tp6 = FPDFText_LoadPage(pg6);
                    if (tp6)
                    {
                        double tol6 = 5.0 / g_zoom;
                        int ci6 = FPDFText_GetCharIndexAtPos(tp6, pdfX6, pdfY6, tol6, tol6);
                        if (ci6 >= 0)
                        {
                            int idx = HitTestHighlight(hitPage6, ci6);
                            if (idx >= 0)
                            {
                                g_highlights.erase(g_highlights.begin() + idx);
                                g_dirty = true;
                            }
                        }
                    }
                    if (tp6 && tp6 != g_textPage) FPDFText_ClosePage(tp6);
                    if (pg6 && pg6 != g_pdfPage)  FPDF_ClosePage(pg6);
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case 30: AddUnderlineFromSelection(ANNOT_COLOR_YELLOW);   InvalidateRect(hWnd, nullptr, FALSE); break;
        case 31: AddUnderlineFromSelection(ANNOT_COLOR_GREEN);    InvalidateRect(hWnd, nullptr, FALSE); break;
        case 32: AddUnderlineFromSelection(ANNOT_COLOR_BLUE);     InvalidateRect(hWnd, nullptr, FALSE); break;
        case 33: AddUnderlineFromSelection(ANNOT_COLOR_ORANGE);   InvalidateRect(hWnd, nullptr, FALSE); break;
        case 34: AddUnderlineFromSelection(ANNOT_COLOR_PINK);     InvalidateRect(hWnd, nullptr, FALSE); break;
        case 36: AddUnderlineFromSelection(ANNOT_COLOR_LAVENDER); InvalidateRect(hWnd, nullptr, FALSE); break;
        case 37: AddUnderlineFromSelection(ANNOT_COLOR_EMERALD);  InvalidateRect(hWnd, nullptr, FALSE); break;
        case 38: AddUnderlineFromSelection(ANNOT_COLOR_SLATE);    InvalidateRect(hWnd, nullptr, FALSE); break;
        case 35: // 밑줄 삭제
        {
            if (g_textPage && g_pdfDoc)
            {
                RECT cr6u; GetClientRect(hWnd, &cr6u);
                const RECT vp6u = GetViewportRect(cr6u);
                double pdfX6u, pdfY6u;
                int hitPage6u = HitTestPageAtPoint(mx, my, vp6u, &pdfX6u, &pdfY6u);
                if (hitPage6u >= 0)
                {
                    FPDF_TEXTPAGE tp6u = (hitPage6u == g_currentPage) ? g_textPage : nullptr;
                    FPDF_PAGE pg6u = (hitPage6u == g_currentPage) ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, hitPage6u);
                    if (!tp6u && pg6u) tp6u = FPDFText_LoadPage(pg6u);
                    if (tp6u)
                    {
                        double tol6u = 5.0 / g_zoom;
                        int ci6u = FPDFText_GetCharIndexAtPos(tp6u, pdfX6u, pdfY6u, tol6u, tol6u);
                        if (ci6u >= 0)
                        {
                            int idx = HitTestUnderline(hitPage6u, ci6u);
                            if (idx >= 0)
                            {
                                g_underlines.erase(g_underlines.begin() + idx);
                                g_dirty = true;
                            }
                        }
                    }
                    if (tp6u && tp6u != g_textPage) FPDFText_ClosePage(tp6u);
                    if (pg6u && pg6u != g_pdfPage)  FPDF_ClosePage(pg6u);
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case 40: AddStrikeoutFromSelection(ANNOT_COLOR_YELLOW);   InvalidateRect(hWnd, nullptr, FALSE); break;
        case 41: AddStrikeoutFromSelection(ANNOT_COLOR_GREEN);    InvalidateRect(hWnd, nullptr, FALSE); break;
        case 42: AddStrikeoutFromSelection(ANNOT_COLOR_BLUE);     InvalidateRect(hWnd, nullptr, FALSE); break;
        case 43: AddStrikeoutFromSelection(ANNOT_COLOR_ORANGE);   InvalidateRect(hWnd, nullptr, FALSE); break;
        case 44: AddStrikeoutFromSelection(ANNOT_COLOR_PINK);     InvalidateRect(hWnd, nullptr, FALSE); break;
        case 46: AddStrikeoutFromSelection(ANNOT_COLOR_LAVENDER); InvalidateRect(hWnd, nullptr, FALSE); break;
        case 47: AddStrikeoutFromSelection(ANNOT_COLOR_EMERALD);  InvalidateRect(hWnd, nullptr, FALSE); break;
        case 48: AddStrikeoutFromSelection(ANNOT_COLOR_SLATE);    InvalidateRect(hWnd, nullptr, FALSE); break;
        case 45: // 취소선 삭제
        {
            if (g_textPage && g_pdfDoc)
            {
                RECT cr6s; GetClientRect(hWnd, &cr6s);
                const RECT vp6s = GetViewportRect(cr6s);
                double pdfX6s, pdfY6s;
                int hitPage6s = HitTestPageAtPoint(mx, my, vp6s, &pdfX6s, &pdfY6s);
                if (hitPage6s >= 0)
                {
                    FPDF_TEXTPAGE tp6s = (hitPage6s == g_currentPage) ? g_textPage : nullptr;
                    FPDF_PAGE pg6s = (hitPage6s == g_currentPage) ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, hitPage6s);
                    if (!tp6s && pg6s) tp6s = FPDFText_LoadPage(pg6s);
                    if (tp6s)
                    {
                        double tol6s = 5.0 / g_zoom;
                        int ci6s = FPDFText_GetCharIndexAtPos(tp6s, pdfX6s, pdfY6s, tol6s, tol6s);
                        if (ci6s >= 0)
                        {
                            int idx = HitTestStrikeout(hitPage6s, ci6s);
                            if (idx >= 0)
                            {
                                g_strikeouts.erase(g_strikeouts.begin() + idx);
                                g_dirty = true;
                            }
                        }
                    }
                    if (tp6s && tp6s != g_textPage) FPDFText_ClosePage(tp6s);
                    if (pg6s && pg6s != g_pdfPage)  FPDF_ClosePage(pg6s);
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case 50: AddSquigglyFromSelection(ANNOT_COLOR_YELLOW);   InvalidateRect(hWnd, nullptr, FALSE); break;
        case 51: AddSquigglyFromSelection(ANNOT_COLOR_GREEN);    InvalidateRect(hWnd, nullptr, FALSE); break;
        case 52: AddSquigglyFromSelection(ANNOT_COLOR_BLUE);     InvalidateRect(hWnd, nullptr, FALSE); break;
        case 53: AddSquigglyFromSelection(ANNOT_COLOR_ORANGE);   InvalidateRect(hWnd, nullptr, FALSE); break;
        case 54: AddSquigglyFromSelection(ANNOT_COLOR_PINK);     InvalidateRect(hWnd, nullptr, FALSE); break;
        case 56: AddSquigglyFromSelection(ANNOT_COLOR_LAVENDER); InvalidateRect(hWnd, nullptr, FALSE); break;
        case 57: AddSquigglyFromSelection(ANNOT_COLOR_EMERALD);  InvalidateRect(hWnd, nullptr, FALSE); break;
        case 58: AddSquigglyFromSelection(ANNOT_COLOR_SLATE);    InvalidateRect(hWnd, nullptr, FALSE); break;
        case 55: // 물결 밑줄 삭제
        {
            if (g_textPage && g_pdfDoc)
            {
                RECT cr6q; GetClientRect(hWnd, &cr6q);
                const RECT vp6q = GetViewportRect(cr6q);
                double pdfX6q, pdfY6q;
                int hitPage6q = HitTestPageAtPoint(mx, my, vp6q, &pdfX6q, &pdfY6q);
                if (hitPage6q >= 0)
                {
                    FPDF_TEXTPAGE tp6q = (hitPage6q == g_currentPage) ? g_textPage : nullptr;
                    FPDF_PAGE pg6q = (hitPage6q == g_currentPage) ? g_pdfPage : FPDF_LoadPage(g_pdfDoc, hitPage6q);
                    if (!tp6q && pg6q) tp6q = FPDFText_LoadPage(pg6q);
                    if (tp6q)
                    {
                        double tol6q = 5.0 / g_zoom;
                        int ci6q = FPDFText_GetCharIndexAtPos(tp6q, pdfX6q, pdfY6q, tol6q, tol6q);
                        if (ci6q >= 0)
                        {
                            int idx = HitTestSquiggly(hitPage6q, ci6q);
                            if (idx >= 0)
                            {
                                g_squigglies.erase(g_squigglies.begin() + idx);
                                g_dirty = true;
                            }
                        }
                    }
                    if (tp6q && tp6q != g_textPage) FPDFText_ClosePage(tp6q);
                    if (pg6q && pg6q != g_pdfPage)  FPDF_ClosePage(pg6q);
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        } // switch(cmd)
    }
    break;

    case WM_MOUSEWHEEL:
    {
        if (g_screenLock) break;
        const int  delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const WORD keys = GET_KEYSTATE_WPARAM(wParam);

        // 그룹 패널 위에 있으면 패널 스크롤
        if (GroupPanel::IsVisible()) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            RECT cr; GetClientRect(hWnd, &cr);
            RECT pr = GroupPanel::GetPanelRect(cr);
            if (PtInRect(&pr, pt)) {
                GroupPanel::OnMouseWheel(delta);
                RECT inv = { pr.left, pr.top, pr.right, pr.bottom };
                InvalidateRect(hWnd, &inv, FALSE);
                return 0;
            }
        }

        // 마우스 휠 시 자동 스크롤/슬라이드쇼 중단
        if (g_autoScrolling)
        {
            g_autoScrolling = false;
            KillTimer(hWnd, AUTO_SCROLL_TIMER_ID);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (g_slideshowActive)
        {
            // 슬라이드쇼 중 휠: 페이지 이동 (위로 = 이전, 아래로 = 다음)
            NavigatePage(hWnd, delta > 0 ? -1 : +1);
            return 0;
        }

        // 사이드바 스크롤
        if (g_sidebarVisible)
        {
            POINT mpt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &mpt);
            if (mpt.x < SIDEBAR_WIDTH && mpt.y >= TOOLBAR_HEIGHT)
            {
                RECT cr5; GetClientRect(hWnd, &cr5);
                int contentH = cr5.bottom - STATUSBAR_HEIGHT - GetBelowToolbarY() - SIDEBAR_HEADER_H;

                if (g_sidebarTab == TAB_THUMBNAILS && g_pdfDoc)
                {
                    int totalH = GetThumbnailTotalHeight(cr5);
                    int scrollAmt = delta > 0 ? -60 : 60;
                    g_thumbScrollY = std::clamp(g_thumbScrollY + scrollAmt, 0, std::max(0, totalH - contentH));
                    InvalidateRect(hWnd, nullptr, FALSE);
                    break;
                }
                else if (g_sidebarTab == TAB_BOOKMARKS && !g_bookmarks.empty())
                {
                    int visCount = 0;
                    for (int i = 0; i < (int)g_bookmarks.size(); ++i)
                        if (IsBookmarkVisible(i)) visCount++;
                    int totalH = visCount * SIDEBAR_ITEM_H;
                    int scrollAmt = delta > 0 ? -SIDEBAR_ITEM_H * 3 : SIDEBAR_ITEM_H * 3;
                    g_sidebarScrollY = std::clamp(g_sidebarScrollY + scrollAmt, 0, std::max(0, totalH - contentH));
                    InvalidateRect(hWnd, nullptr, FALSE);
                    break;
                }
            }
        }

        if (keys & MK_CONTROL)
        {
            if (!g_pdfDoc || g_pageLayouts.empty()) break;

            // 마우스 화면 좌표 → 클라이언트 좌표
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);

            RECT cr; GetClientRect(hWnd, &cr);
            const RECT vp = GetViewportRect(cr);
            const int vpW = vp.right - vp.left;
            const int vpH = vp.bottom - vp.top;

            // 현재 마우스 위치의 문서 내 상대 위치 기억 (X, Y 모두)
            float relY = (g_totalDocHeight > 0)
                ? (float)(g_scrollY + (pt.y - vp.top)) / g_totalDocHeight : 0.0f;
            int totalDocW = g_maxPageWidth + SIDE_MARGIN * 2;
            float relX = (totalDocW > 0)
                ? (float)(g_scrollX + (pt.x - vp.left)) / totalDocW : 0.5f;

            // 새 줌 계산
            float newZoom = (delta > 0)
                ? std::min(g_zoom * ZOOM_STEP, ZOOM_MAX)
                : std::max(g_zoom / ZOOM_STEP, ZOOM_MIN);
            if (newZoom == g_zoom) break;

            g_zoom = newZoom;
            g_hasSelection = g_isSelecting = false;
            g_selPage = -1;
            RecalcPageLayouts();
            ClearPageCache();
            InitPageCache(g_pageCount);
            ClearThumbCache();
            InitThumbCache(g_pageCount);

            // annotation rect 캐시 무효화 (줌 변경 시 PDF→화면 변환 달라짐)
            for (auto& h : g_highlights)  h.cachedRects.clear();
            for (auto& u : g_underlines)  u.cachedRects.clear();
            for (auto& s : g_strikeouts)  s.cachedRects.clear();
            for (auto& q : g_squigglies)  q.cachedRects.clear();

            // 새 스크롤 위치: 커서 아래 동일 지점 유지 (Y축)
            g_scrollY = (int)(relY * g_totalDocHeight) - (pt.y - vp.top);
            g_scrollY = std::clamp(g_scrollY, 0, std::max(0, g_totalDocHeight - vpH));
            g_scrollYf = (float)g_scrollY;

            // 새 스크롤 위치: 커서 아래 동일 지점 유지 (X축)
            int newTotalDocW = g_maxPageWidth + SIDE_MARGIN * 2;
            g_scrollX = (int)(relX * newTotalDocW) - (pt.x - vp.left);
            g_scrollX = std::clamp(g_scrollX, 0, std::max(0, newTotalDocW - vpW));

            UpdateActivePageFromScroll(hWnd);
            UpdateScrollBars(hWnd);
            UpdateTitle(hWnd);
            g_pageBaseDirty = true;
            { RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT; InvalidateRect(hWnd, &ir, FALSE); }
        }
        else if (g_pdfDoc)
        {
            UINT sysLines = 3;
            SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &sysLines, 0);
            if (sysLines == WHEEL_PAGESCROLL) sysLines = 10;

            float scrollAmt = -(float)delta / WHEEL_DELTA * (float)sysLines * SCROLL_LINE;

            RECT cr6; GetClientRect(hWnd, &cr6);
            const RECT vp6 = GetViewportRect(cr6);
            const int vpH = vp6.bottom - vp6.top;
            const float maxY = (float)std::max(0, g_totalDocHeight - vpH);

            // g_scrollTargetY가 현재 실제 스크롤과 다를 수 있으므로 먼저 동기화
            if (std::abs(g_scrollTargetY - g_scrollYf) > 1.0f)
                g_scrollTargetY = g_scrollYf;

            g_scrollTargetY = std::clamp(g_scrollTargetY + scrollAmt, 0.0f, maxY);
            g_scrollYf = g_scrollTargetY;
            g_scrollY  = (int)g_scrollYf;
            UpdateActivePageFromScroll(hWnd);
            UpdateScrollBars(hWnd);
            g_pageBaseDirty = true;
            { RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT; InvalidateRect(hWnd, &ir, FALSE); }
            SchedulePrefetch(hWnd);
        }
    }
    break;

    case WM_CHAR:
        // 폼 텍스트 필드에 포커스가 있을 때만 문자 입력 전달 (단축키와 충돌 방지)
        if (g_formHandle && g_pdfPage && g_formFieldFocused && !g_screenLock)
        {
            FORM_OnChar(g_formHandle, g_pdfPage, (int)wParam, 0);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        break;

    case WM_KEYUP:
        if (g_formHandle && g_pdfPage && g_formFieldFocused && !g_screenLock)
        {
            int modifier = 0;
            if (GetKeyState(VK_SHIFT)   & 0x8000) modifier |= FWL_EVENTFLAG_ShiftKey;
            if (GetKeyState(VK_CONTROL) & 0x8000) modifier |= FWL_EVENTFLAG_ControlKey;
            FORM_OnKeyUp(g_formHandle, g_pdfPage, (int)wParam, modifier);
        }
        break;

    case WM_KEYDOWN:
        // 폼 필드에 포커스가 있으면 모든 키를 폼에 전달 (단축키 충돌 방지)
        // ESC는 예외: 포커스 해제 후 기본 처리로 넘김
        if (g_formHandle && g_pdfPage && g_formFieldFocused && !g_screenLock)
        {
            if (wParam == VK_ESCAPE)
            {
                // ESC로 폼 포커스 해제
                g_formFieldFocused = false;
                int modifier = 0;
                if (GetKeyState(VK_SHIFT)   & 0x8000) modifier |= FWL_EVENTFLAG_ShiftKey;
                if (GetKeyState(VK_CONTROL) & 0x8000) modifier |= FWL_EVENTFLAG_ControlKey;
                FORM_OnKeyDown(g_formHandle, g_pdfPage, (int)wParam, modifier);
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
            int modifier = 0;
            if (GetKeyState(VK_SHIFT)   & 0x8000) modifier |= FWL_EVENTFLAG_ShiftKey;
            if (GetKeyState(VK_CONTROL) & 0x8000) modifier |= FWL_EVENTFLAG_ControlKey;
            FORM_OnKeyDown(g_formHandle, g_pdfPage, (int)wParam, modifier);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        if (g_screenLock)
        {
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            // 허용: 잠금 해제(L), 복사(Ctrl+C), 전체선택(Ctrl+A),
            //         하이라이트(Ctrl+H), 밑줄(Ctrl+U), 취소선(Ctrl+U+Shift)
            //         검색(Ctrl+F), ESC, Delete/Backspace
            if (wParam == 'L')
                SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_SCREEN_LOCK, 0), 0);
            else if (ctrl && wParam == 'C') CopySelectionToClipboard(hWnd);
            else if (ctrl && wParam == 'A') SendMessage(hWnd, WM_KEYDOWN, 'A', lParam); // 아래에서 처리
            else if (ctrl && wParam == 'H' && g_hasSelection)
            { AddHighlightFromSelection(ANNOT_COLOR_YELLOW); InvalidateRect(hWnd,nullptr,FALSE); }
            else if (ctrl && wParam == 'U' && g_hasSelection)
            { if(shift) AddStrikeoutFromSelection(RGB(255,100,100));
              else       AddUnderlineFromSelection(RGB(255,100,100));
              InvalidateRect(hWnd,nullptr,FALSE); }
            else if (ctrl && wParam == 'F')
                SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_FIND, 0), 0);
            else if (wParam == VK_ESCAPE || wParam == VK_DELETE || wParam == VK_BACK)
            {
                // 선택 해제 / 주석 삭제는 아래 switch와 동일하게 처리
                goto keydown_common;
            }
            break;
        keydown_common:;
        }
        switch (wParam)
        {
        case 'H':
            if ((GetKeyState(VK_CONTROL) & 0x8000) && g_hasSelection)
            {
                AddHighlightFromSelection(ANNOT_COLOR_YELLOW);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            else if (g_pdfDoc)
                SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_HAND_TOOL, 0), 0);
            break;
        case 'U':
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && g_hasSelection)
            {
                AddStrikeoutFromSelection(RGB(255, 100, 100));
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            else if ((GetKeyState(VK_CONTROL) & 0x8000) && g_hasSelection)
            {
                AddUnderlineFromSelection(RGB(255, 100, 100));
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        case 'B':
            if (g_pdfDoc)
                SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_TOGGLE_SIDEBAR, 0), 0);
            break;
        case 'R':
            if (g_pdfDoc)
            {
                if (GetKeyState(VK_SHIFT) & 0x8000)
                    SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_ROTATE_CCW, 0), 0);
                else
                    SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_ROTATE_CW, 0), 0);
            }
            break;
        case 'D':
            SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_DARK_MODE, 0), 0);
            break;
        case 'F':
            if (g_pdfDoc)
                FitToPage(hWnd);
            break;
        case 'C':
            if (GetKeyState(VK_CONTROL) & 0x8000)
                CopySelectionToClipboard(hWnd);
            break;
        case 'A':
            if ((GetKeyState(VK_CONTROL) & 0x8000) && g_textPage && g_pdfPage)
            {
                int charCount = FPDFText_CountChars(g_textPage);
                if (charCount > 0)
                {
                    g_selCharStart = 0;
                    g_selCharEnd = charCount - 1;  // ← -1 추가 (inclusive)
                    g_hasSelection = true;
                    g_isSelecting = false;
                    g_selPage = g_currentPage;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
            break;
        case 'G':
            if (GetKeyState(VK_CONTROL) & 0x8000)
                ShowGotoPageDialog(hWnd);
            break;
        case 'T':
            if (GetKeyState(VK_CONTROL) & 0x8000)
                SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_NEW_TAB, 0), 0);
            break;
        case VK_ESCAPE:
            if (g_slideshowActive)
            {
                ToggleSlideshow(hWnd);
                break;
            }
            if (g_shapeMode != 0)
            {
                g_shapeMode = 0;
                g_isDraggingShape = false;
                if (g_selectedShape >= 0) g_selectedShape = -1;
                if (GetCapture() == hWnd) ReleaseCapture();
                HMENU hMenu = GetMenu(hWnd);
                if (hMenu)
                {
                    CheckMenuItem(hMenu, IDM_SHAPE_RECT,   MF_BYCOMMAND | MF_UNCHECKED);
                    CheckMenuItem(hMenu, IDM_SHAPE_CIRCLE, MF_BYCOMMAND | MF_UNCHECKED);
                }
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                InvalidateToolbarBaseCache();
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
            if (g_selectedShape >= 0)
            {
                g_selectedShape = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
            if (g_selectedInkStroke >= 0)
            {
                g_selectedInkStroke = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
            if (g_redactMode)
            {
                g_redactMode = false;
                g_isDraggingRedact = false;
                g_selectedRedact = -1;
                if (g_isDraggingRedact) ReleaseCapture();
                CheckMenuItem(GetMenu(hWnd), IDM_REDACT,
                    MF_BYCOMMAND | MF_UNCHECKED);
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
            if (g_selectedHighlight >= 0 || g_selectedUnderline >= 0 || g_selectedStrikeout >= 0)
            {
                g_selectedHighlight = -1;
                g_selectedUnderline = -1;
                g_selectedStrikeout = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        case VK_DELETE:
        case VK_BACK:
            // 잉크 스트로크 선택 중 삭제
            if (g_selectedInkStroke >= 0 && g_selectedInkStroke < (int)g_inkStrokes.size())
            {
                g_inkStrokes.erase(g_inkStrokes.begin() + g_selectedInkStroke);
                g_selectedInkStroke = -1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
            // 도형 선택 중 삭제
            if (g_selectedShape >= 0 && g_selectedShape < (int)g_shapes.size())
            {
                g_shapes.erase(g_shapes.begin() + g_selectedShape);
                g_selectedShape = -1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
            // Redact 박스 선택 중 삭제
            if (g_redactMode && g_selectedRedact >= 0 && g_selectedRedact < (int)g_redacts.size())
            {
                g_redacts.erase(g_redacts.begin() + g_selectedRedact);
                g_selectedRedact = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
            // 서명 선택 중
            if (g_selectedSig >= 0 && g_selectedSig < (int)g_signatures.size())
            {
                delete g_signatures[g_selectedSig].bitmap;
                g_signatures.erase(g_signatures.begin() + g_selectedSig);
                g_selectedSig = -1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
            // 텍스트 박스 선택 중 (편집 모드가 아닐 때만)
            if (g_selectedTextBox >= 0 && g_editingTextBox < 0 &&
                g_selectedTextBox < static_cast<int>(g_textBoxes.size()))
            {
                g_textBoxes.erase(g_textBoxes.begin() + g_selectedTextBox);
                g_selectedTextBox = -1;
                g_dirty = true;
                RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT;
                InvalidateRect(hWnd, &ir, FALSE);
                break;
            }
            // 노트(스티키 메모) 선택 중
            if (g_selectedNote >= 0 && g_selectedNote < static_cast<int>(g_notes.size()))
            {
                g_notes.erase(g_notes.begin() + g_selectedNote);
                g_selectedNote = -1;
                g_hoverNote = -1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }
            if (g_selectedHighlight >= 0 && g_selectedHighlight < static_cast<int>(g_highlights.size()))
            {
                g_highlights.erase(g_highlights.begin() + g_selectedHighlight);
                g_selectedHighlight = -1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            else if (g_selectedUnderline >= 0 && g_selectedUnderline < static_cast<int>(g_underlines.size()))
            {
                g_underlines.erase(g_underlines.begin() + g_selectedUnderline);
                g_selectedUnderline = -1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            else if (g_selectedStrikeout >= 0 && g_selectedStrikeout < static_cast<int>(g_strikeouts.size()))
            {
                g_strikeouts.erase(g_strikeouts.begin() + g_selectedStrikeout);
                g_selectedStrikeout = -1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            else if (g_selectedSquiggly >= 0 && g_selectedSquiggly < static_cast<int>(g_squigglies.size()))
            {
                g_squigglies.erase(g_squigglies.begin() + g_selectedSquiggly);
                g_selectedSquiggly = -1;
                g_dirty = true;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        case VK_F4:
            if (GetKeyState(VK_CONTROL) & 0x8000)
                SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_CLOSE_TAB, 0), 0);
            break;
        case VK_TAB:
            if (GetKeyState(VK_CONTROL) & 0x8000)
            {
                if (GetKeyState(VK_SHIFT) & 0x8000)
                    SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_PREV_TAB, 0), 0);
                else
                    SendMessage(hWnd, WM_COMMAND, MAKEWPARAM(IDM_NEXT_TAB, 0), 0);
            }
            break;
        case VK_UP:    HandleScroll(hWnd, SB_VERT, MAKEWPARAM(SB_LINEUP, 0)); break;
        case VK_DOWN:  HandleScroll(hWnd, SB_VERT, MAKEWPARAM(SB_LINEDOWN, 0)); break;
        case VK_LEFT:
        {
            if (g_slideshowActive)
            {
                NavigatePage(hWnd, -1);
                break;
            }
            SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE };
            GetScrollInfo(hWnd, SB_HORZ, &si);
            if (si.nMax > static_cast<int>(si.nPage))
                HandleScroll(hWnd, SB_HORZ, MAKEWPARAM(SB_LINEUP, 0));
            else
                NavigatePage(hWnd, -1);
            break;
        }
        case VK_RIGHT:
        {
            if (g_slideshowActive)
            {
                NavigatePage(hWnd, +1);
                break;
            }
            SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE };
            GetScrollInfo(hWnd, SB_HORZ, &si);
            if (si.nMax > static_cast<int>(si.nPage))
                HandleScroll(hWnd, SB_HORZ, MAKEWPARAM(SB_LINEDOWN, 0));
            else
                NavigatePage(hWnd, +1);
            break;
        }
        case VK_HOME:
            if (g_pdfDoc)
                ScrollToPage(hWnd, 0);
            break;
        case VK_END:
            if (g_pdfDoc)
                ScrollToPage(hWnd, g_pageCount - 1);
            break;
        case VK_PRIOR:
            if (g_slideshowActive) NavigatePage(hWnd, -1);
            else NavigatePage(hWnd, -1);
            break;
        case VK_NEXT:
            if (g_slideshowActive) NavigatePage(hWnd, +1);
            else NavigatePage(hWnd, +1);
            break;
        case VK_SPACE:
            if (g_slideshowActive) NavigatePage(hWnd, +1);
            break;
        case VK_F5:
            ToggleSlideshow(hWnd);
            break;
        }
        break;

    case WM_TIMER:
        if (wParam == SB_ANIM_TIMER)
        {
            SidebarAnimStep(hWnd);
            break;
        }
        // 로딩 스피너 애니메이션 타이머
        if (wParam == LOADING_ANIM_TIMER_ID)
        {
            ++g_loadingFrame;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        // PDFium 폼 타이머 (커서 깜빡임 등): FORM_TIMER_BASE 이상 ID
        if (wParam >= FORM_TIMER_BASE)
        {
            int pdfId = (int)(wParam - FORM_TIMER_BASE);
            auto it = g_formTimers.find(pdfId);
            if (it != g_formTimers.end())
                it->second.callback(pdfId); // PDFium 콜백 호출 → FFI_Invalidate_CB 경유 화면 갱신
            break;
        }
        if (wParam == 901) // 툴바 툴팁 딜레이
        {
            KillTimer(hWnd, 901);
            if (g_hToolbarTooltip && !g_toolbarTipText.empty())
            {
                // GDI+로 텍스트 크기 측정
                HDC hdc = GetDC(g_hToolbarTooltip);
                Gdiplus::Graphics gfx(hdc);
                Gdiplus::FontFamily fontFamily(L"Segoe UI");
                Gdiplus::Font font(&fontFamily, 9.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                Gdiplus::RectF layoutRect(0, 0, 400, 100);
                Gdiplus::RectF boundRect;
                gfx.MeasureString(g_toolbarTipText.c_str(), -1, &font, layoutRect, &boundRect);
                ReleaseDC(g_hToolbarTooltip, hdc);

                int padX = 16, padY = 8;
                int tipW = (int)boundRect.Width  + padX * 2;
                int tipH = (int)boundRect.Height + padY * 2;

                POINT pt; GetCursorPos(&pt);
                int tx = pt.x + 12;
                int ty = pt.y + 20;

                SetWindowPos(g_hToolbarTooltip, HWND_TOPMOST, tx, ty, tipW, tipH,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
                // 윈도우 모양을 둥근 사각형으로 클리핑
                HRGN hRgn = CreateRoundRectRgn(0, 0, tipW + 1, tipH + 1, 12, 12);
                SetWindowRgn(g_hToolbarTooltip, hRgn, FALSE);
                InvalidateRect(g_hToolbarTooltip, nullptr, TRUE);
            }
            break;
        }
        if (wParam == AUTO_SCROLL_TIMER_ID)
        {
            if (!g_pdfDoc || g_pageLayouts.empty())
            {
                g_autoScrolling = false;
                KillTimer(hWnd, AUTO_SCROLL_TIMER_ID);
                InvalidateRect(hWnd, nullptr, FALSE);
                break;
            }

            RECT cr; GetClientRect(hWnd, &cr);
            const RECT vp = GetViewportRect(cr);
            const int vpH = vp.bottom - vp.top;
            const int maxY = std::max(0, g_totalDocHeight - vpH);

            g_scrollYf += g_prefs.scrollSpeed;
            if (g_scrollYf >= (float)maxY)
            {
                g_scrollYf = (float)maxY;
                    g_scrollTargetY = g_scrollYf;
                    g_autoScrolling = false;
                KillTimer(hWnd, AUTO_SCROLL_TIMER_ID);
            }
            g_scrollY = (int)g_scrollYf;

            UpdateActivePageFromScroll(hWnd);
            UpdateScrollBars(hWnd);
            { RECT ir; GetClientRect(hWnd, &ir); ir.top = TOOLBAR_HEIGHT; InvalidateRect(hWnd, &ir, FALSE); }
        }
        break;

    case WM_PDF_LOADED:
    {
        LoadingResult* res = reinterpret_cast<LoadingResult*>(lParam);

        if (!res->doc)
        {
            std::vector<char> pwBuf;
            FPDF_DOCUMENT pwDoc = LoadPDFWithPasswordPrompt(hWnd, res->path, &pwBuf);
            if (pwDoc)
            {
                res->doc = pwDoc;
                res->pdfBuffer = std::make_shared<std::vector<char>>(std::move(pwBuf));
            }
        }
        if (!res->doc)
        {
            StopLoadingGif(hWnd);
            if (!g_pwPromptCancelled)
            {
                WCHAR msg[256];
                swprintf_s(msg, 256, LS(STR_PDF_OPEN_FAIL_FMT), FPDF_GetLastError());
                CleanPopup::Show(hWnd, LS(STR_ERROR), msg);
            }
            if (g_pwPromptCancelled)
            {
                g_pwPromptCancelled = false;
                CloseTab(hWnd, g_activeTab);
            }
            delete res;
            break;
        }

        // 전역 변수만 설정하고, 무거운 초기화는 메시지 루프가
        // 한 번 돌아 스피너를 그린 뒤에 처리되도록 PostMessage로 넘긴다.
        g_pdfDoc    = res->doc;
        g_filePath  = res->path;
        g_pageCount = FPDF_GetPageCount(g_pdfDoc);

        if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
        {
            g_tabs[g_activeTab].filePath = res->path;
            g_tabs[g_activeTab].pdfBuffer = std::move(res->pdfBuffer);  // shared_ptr 이동
        }

        // 주석 결과를 WM_PDF_INIT → WM_PDF_INIT2까지 wParam으로 전달
        AnnotLoadResult* pendingAnnots = res->annots;
        res->annots = nullptr;

        // 스피너 한 프레임 그리고 WM_PDF_INIT 에서 초기화
        InvalidateRect(hWnd, nullptr, FALSE);
        UpdateWindow(hWnd);
        PostMessageW(hWnd, WM_PDF_INIT, reinterpret_cast<WPARAM>(pendingAnnots), 0);

        delete res;
        break;
    }

    case WM_PDF_INIT:
    {
        // 1단계: 레이아웃·스크롤·타이틀 등 가벼운 초기화
        WPARAM savedAnnots = wParam;
        FlushRenderQueue();   // 이전 문서의 렌더 요청 폐기
        RecalcPageLayouts();
        InitPageCache(g_pageCount);
        InitThumbCache(g_pageCount);
        ExtractBookmarks();
        LoadUserBookmarks();
        FitToWidth(hWnd);
        ScrollToPage(hWnd, 0);
        UpdateTitle(hWnd);
        UpdateScrollBars(hWnd);
        AddToRecentFiles(g_filePath);
        UpdateRecentFilesMenu(hWnd);
        // 메시지 루프에 제어를 돌려 스피너가 한 프레임 더 돌도록 PostMessage
        PostMessageW(hWnd, WM_PDF_INIT2, savedAnnots, 0);
        break;
    }

    case WM_PDF_INIT2:
    {
        // 2단계: 주석 초기화 후 가시 페이지부터 지연 로딩 시작
        AnnotLoadResult* annots = reinterpret_cast<AnnotLoadResult*>(wParam);
        if (annots) delete annots;
        InvalidateToolbarBaseCache();
        InitFormFill(hWnd);
        // InitFormFill 이전에 이미 페이지가 로드되어 있으므로
        // 현재 페이지에 대해 AfterLoadPage/PageAAction을 여기서 다시 호출
        if (g_formHandle && g_pdfPage)
        {
            FORM_OnAfterLoadPage(g_pdfPage, g_formHandle);
            FORM_DoPageAAction(g_pdfPage, g_formHandle, FPDFPAGE_AACTION_OPEN);
        }
        RunAnnotLoad(hWnd);
        // 스피너는 바로 종료 — 첫 페이지가 이미 보이는 상태로 시작
        StopLoadingGif(hWnd);
        InvalidateStatusBarCache();
        InvalidateRect(hWnd, nullptr, TRUE);
        SchedulePrefetch(hWnd);
        break;
    }

    case WM_ANNOT_PAGE:
    {
        if (!g_pdfDoc) break;
        const int pageIndex = (int)wParam;
        if (pageIndex < 0 || pageIndex >= g_pageCount) break;

        // 이미 로드된 페이지는 건너뜀 (중복 메시지 방어)
        if (pageIndex < (int)g_annotPageLoaded.size() && g_annotPageLoaded[pageIndex]) break;

        // 단일 페이지 처리
        RunAnnotLoadBatch(hWnd, pageIndex, pageIndex + 1);

        // 해당 페이지가 현재 화면에 보이면 즉시 갱신
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }

    case WM_RENDER_DONE:
    {
        // worker가 heap 할당해 전달한 RenderResult*를 받아 HBITMAP 생성 후 캐시에 저장.
        // HBITMAP 생성/캐시 쓰기는 UI 스레드에서만 수행 → 경쟁 조건 없음.
        RenderResult* res = reinterpret_cast<RenderResult*>(lParam);
        if (!res) break;

        const int idx = res->pageIndex;
        {
            std::shared_ptr<std::vector<char>> curBuf;
            if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
                curBuf = g_tabs[g_activeTab].pdfBuffer;
            if (!g_pdfDoc || !curBuf || res->pdfBuf != curBuf)
            {
                delete res;
                break;
            }
        }
        // zoom이 바뀌었거나 캐시가 이미 최신이면 버림
        if (res->zoom != g_zoom
            || idx < 0 || idx >= g_pageCount
            || idx >= (int)g_pageCache.size()
            || res->pixels.empty()
            || res->rotation != GetEffectiveRotation(idx))
        {
            delete res;
            break;
        }

        // 렌더 크기가 현재 레이아웃과 다르면 버림 (스크롤 중 레이아웃이 변한 경우)
        if (idx < (int)g_pageLayouts.size()
            && (res->width  != g_pageLayouts[idx].renderW
             || res->height != g_pageLayouts[idx].renderH))
        {
            if (idx < (int)g_prefetchPending.size())
                g_prefetchPending[idx] = false;
            delete res;
            break;
        }

        // pending 플래그 해제
        if (idx < (int)g_prefetchPending.size())
            g_prefetchPending[idx] = false;

        // 이미 같은 zoom의 캐시가 있으면 버림
        if (g_pageCache[idx].hBmp && g_pageCache[idx].zoom == g_zoom)
        {
            delete res;
            break;
        }

        // 픽셀 버퍼 → DIB section
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = res->width;
        bmi.bmiHeader.biHeight      = -res->height;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dibBits = nullptr;
        HDC   tmpDC   = CreateCompatibleDC(nullptr);
        HBITMAP hDib  = CreateDIBSection(tmpDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        DeleteDC(tmpDC);

        if (hDib && dibBits)
        {
            memcpy(dibBits, res->pixels.data(), res->pixels.size());
            if (g_pageCache[idx].hBmp) DeleteObject(g_pageCache[idx].hBmp);
            g_pageCache[idx].hBmp   = hDib;
            g_pageCache[idx].width  = res->width;
            g_pageCache[idx].height = res->height;
            g_pageCache[idx].zoom   = res->zoom;
            g_pageCache[idx].dark   = false;

            // ── 프리뷰 생성 (방금 만든 풀해상도를 축소해 보관) ──
            if ((int)g_previewCache.size() != g_pageCount)
                g_previewCache.assign(g_pageCount, PreviewEntry{});
            {
                const int PREV_W = 160;
                const int prevW = std::min(PREV_W, res->width);
                const int prevH = std::max(1, (int)((double)res->height / res->width * prevW));
                if (!(g_previewCache[idx].hBmp
                    && g_previewCache[idx].w == prevW
                    && g_previewCache[idx].h == prevH))
                {
                    BITMAPINFO pbi = {};
                    pbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    pbi.bmiHeader.biWidth = prevW;
                    pbi.bmiHeader.biHeight = -prevH;
                    pbi.bmiHeader.biPlanes = 1;
                    pbi.bmiHeader.biBitCount = 32;
                    pbi.bmiHeader.biCompression = BI_RGB;
                    void* pbits = nullptr;
                    HDC sdc = CreateCompatibleDC(nullptr);
                    HDC ddc = CreateCompatibleDC(nullptr);
                    HBITMAP prevBmp = CreateDIBSection(ddc, &pbi, DIB_RGB_COLORS, &pbits, nullptr, 0);
                    if (prevBmp)
                    {
                        HBITMAP so = (HBITMAP)SelectObject(sdc, hDib);
                        HBITMAP dOld = (HBITMAP)SelectObject(ddc, prevBmp);
                        SetStretchBltMode(ddc, HALFTONE);
                        SetBrushOrgEx(ddc, 0, 0, nullptr);
                        StretchBlt(ddc, 0, 0, prevW, prevH,
                            sdc, 0, 0, res->width, res->height, SRCCOPY);
                        SelectObject(sdc, so);
                        SelectObject(ddc, dOld);
                        if (g_previewCache[idx].hBmp) DeleteObject(g_previewCache[idx].hBmp);
                        g_previewCache[idx].hBmp = prevBmp;
                        g_previewCache[idx].w = prevW;
                        g_previewCache[idx].h = prevH;
                    }
                    DeleteDC(sdc);
                    DeleteDC(ddc);
                }
            }

            g_pageBaseDirty = true;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        else if (hDib)
        {
            DeleteObject(hDib);
        }

        delete res;
        break;
    }

    case WM_CLOSE:
    {
        // 현재 탭 상태 저장
        if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
            SaveTabState(g_tabs[g_activeTab]);
        // 변경사항이 있는 탭이 있는지 확인
        bool anyDirty = false;
        for (const auto& ts : g_tabs)
            if (ts.dirty) { anyDirty = true; break; }
        if (anyDirty)
        {
            bool ok = CleanPopup::Show(hWnd, LS(STR_SAVE_CHANGES_TITLE), LS(STR_SAVE_CHANGES_CONFIRM), true);
                if (!ok) return 0;
                }
                SaveWindowPlacement(hWnd);
                DestroyWindow(hWnd);
                return 0;
            }

    case WM_DESTROY:
        ShowWindow(hWnd, SW_HIDE);
        HideFindBar(hWnd);
        // 워커를 가장 먼저 종료
        StopRenderWorker();
        {
            MSG drain;
            while (PeekMessageW(&drain, hWnd, WM_RENDER_DONE, WM_RENDER_DONE, PM_REMOVE))
            {
                RenderResult* leftover = reinterpret_cast<RenderResult*>(drain.lParam);
                delete leftover;
            }
        }

        if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size())
            SaveTabState(g_tabs[g_activeTab]);
        for (auto& ts : g_tabs)
        {
            if (ts.findHandle) { FPDFText_FindClose(ts.findHandle); ts.findHandle = nullptr; }
            if (ts.textPage) { FPDFText_ClosePage(ts.textPage); ts.textPage = nullptr; }
            if (ts.pdfPage)
            {
                if (ts.formHandle)
                {
                    FORM_DoPageAAction(ts.pdfPage, ts.formHandle, FPDFPAGE_AACTION_CLOSE);
                    FORM_OnBeforeClosePage(ts.pdfPage, ts.formHandle);
                }
                FPDF_ClosePage(ts.pdfPage); ts.pdfPage = nullptr;
            }
            if (ts.formHandle) { FPDFDOC_ExitFormFillEnvironment(ts.formHandle); ts.formHandle = nullptr; }
            if (ts.pdfDoc) { FPDF_CloseDocument(ts.pdfDoc); ts.pdfDoc = nullptr; }
            for (auto& e : ts.pageCache)
                if (e.hBmp) { DeleteObject(e.hBmp); e.hBmp = nullptr; }
        }
        g_tabs.clear();
        g_activeTab = -1;
        // 전역 정리
        g_pdfDoc = nullptr; g_pdfPage = nullptr; g_textPage = nullptr;
        g_pageCache.clear();
        CleanPopup::Shutdown();
        GroupPanel::SaveGroups();
        FPDF_DestroyLibrary();
        // 영구 백버퍼 정리
        if (g_backDC)    { DeleteDC(g_backDC);    DeleteObject(g_backBmp);    g_backDC    = nullptr; g_backBmp    = nullptr; }
        // 페이지 베이스 캐시 DC 정리
        if (g_pageBaseDC) { DeleteDC(g_pageBaseDC); DeleteObject(g_pageBaseBmp); g_pageBaseDC = nullptr; g_pageBaseBmp = nullptr; }
        // 선택 AlphaBlend DC 정리
        if (g_selBlendDC) { DeleteDC(g_selBlendDC); DeleteObject(g_selBlendBmp); g_selBlendDC = nullptr; g_selBlendBmp = nullptr; }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// ─── 정보 대화 상자 ────────────────────────────────────────────────────────────

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
    {
        HWND hParent = GetParent(hDlg);
        if (!hParent) hParent = GetDesktopWindow();

        RECT rcParent, rcDlg;
        GetWindowRect(hParent, &rcParent);
        GetWindowRect(hDlg, &rcDlg);

        const int dlgW = rcDlg.right - rcDlg.left;
        const int dlgH = rcDlg.bottom - rcDlg.top;
        int x = rcParent.left + (rcParent.right - rcParent.left - dlgW) / 2;
        int y = rcParent.top + (rcParent.bottom - rcParent.top - dlgH) / 2;

        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfo(MonitorFromWindow(hParent, MONITOR_DEFAULTTONEAREST), &mi))
        {
            x = std::clamp(x, (int)mi.rcWork.left, (int)mi.rcWork.right - dlgW);
            y = std::clamp(y, (int)mi.rcWork.top, (int)mi.rcWork.bottom - dlgH);
        }

        SetWindowPos(hDlg, nullptr, x, y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return (INT_PTR)TRUE;
    }
    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlID != IDC_ABOUT_ICON) break;

        HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCE(IDB_PLAIN_PNG), RT_RCDATA);
        if (!hRes) break;
        DWORD sz = SizeofResource(hInst, hRes);
        HGLOBAL hMem = LoadResource(hInst, hRes);
        if (!hMem) break;
        void* pData = LockResource(hMem);
        if (!pData) break;

        HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, sz);
        if (!hBuf) break;
        void* pBuf = GlobalLock(hBuf);
        if (!pBuf) { GlobalFree(hBuf); break; }
        memcpy(pBuf, pData, sz);
        GlobalUnlock(hBuf);

        IStream* pStream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(hBuf, TRUE, &pStream)) || !pStream)
        {
            int w = dis->rcItem.right - dis->rcItem.left;
            int h = dis->rcItem.bottom - dis->rcItem.top;
            Gdiplus::Graphics gfx(dis->hDC);
            gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            Gdiplus::Image img(pStream);
            gfx.DrawImage(&img, dis->rcItem.left, dis->rcItem.top, w, h);
            pStream->Release();
        }
        return (INT_PTR)TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
