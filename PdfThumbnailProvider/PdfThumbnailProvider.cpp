// PdfThumbnailProvider.cpp
// PDF 파일의 첫 페이지를 썸네일로 제공하고 우측 하단에 file.png 오버레이를 그리는
// Shell Thumbnail Provider COM DLL
//
// CLSID: {AC686425-5B5C-4EEB-9169-E7A6D11999E1}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <shlwapi.h>
#include <objbase.h>
#include <new>
#include <string>

// GDI+
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

// PDFium - 동적 로드 (DLL이 같은 폴더에 있음)
#include "..\third_party\pdfium\include\fpdfview.h"

// -----------------------------------------------------------------------
// CLSID
// {AC686425-5B5C-4EEB-9169-E7A6D11999E1}
// -----------------------------------------------------------------------
static const CLSID CLSID_PdfThumbnailProvider =
{ 0xAC686425, 0x5B5C, 0x4EEB,
  { 0x91, 0x69, 0xE7, 0xA6, 0xD1, 0x19, 0x99, 0xE1 } };

static HMODULE  g_hModule   = nullptr;
static LONG     g_refCount  = 0;

// -----------------------------------------------------------------------
// PDFium 함수 포인터 (동적 로드)
// -----------------------------------------------------------------------
typedef void        (*PFN_FPDF_InitLibrary)();
typedef void        (*PFN_FPDF_DestroyLibrary)();
typedef FPDF_DOCUMENT (*PFN_FPDF_LoadMemDocument)(const void*, int, FPDF_BYTESTRING);
typedef void        (*PFN_FPDF_CloseDocument)(FPDF_DOCUMENT);
typedef int         (*PFN_FPDF_GetPageCount)(FPDF_DOCUMENT);
typedef FPDF_PAGE   (*PFN_FPDF_LoadPage)(FPDF_DOCUMENT, int);
typedef void        (*PFN_FPDF_ClosePage)(FPDF_PAGE);
typedef double      (*PFN_FPDF_GetPageWidthF)(FPDF_PAGE);
typedef double      (*PFN_FPDF_GetPageHeightF)(FPDF_PAGE);
typedef FPDF_BITMAP (*PFN_FPDFBitmap_Create)(int, int, int);
typedef void        (*PFN_FPDFBitmap_FillRect)(FPDF_BITMAP, int, int, int, int, FPDF_DWORD);
typedef void        (*PFN_FPDF_RenderPageBitmap)(FPDF_BITMAP, FPDF_PAGE, int, int, int, int, int, int);
typedef void*       (*PFN_FPDFBitmap_GetBuffer)(FPDF_BITMAP);
typedef int         (*PFN_FPDFBitmap_GetStride)(FPDF_BITMAP);
typedef void        (*PFN_FPDFBitmap_Destroy)(FPDF_BITMAP);

struct PdfiumFuncs
{
	HMODULE                     hDll;
	PFN_FPDF_InitLibrary        Init;
	PFN_FPDF_DestroyLibrary     Destroy;
	PFN_FPDF_LoadMemDocument    LoadMemDoc;
	PFN_FPDF_CloseDocument      CloseDoc;
	PFN_FPDF_GetPageCount       GetPageCount;
	PFN_FPDF_LoadPage           LoadPage;
	PFN_FPDF_ClosePage          ClosePage;
	PFN_FPDF_GetPageWidthF      GetPageW;
	PFN_FPDF_GetPageHeightF     GetPageH;
	PFN_FPDFBitmap_Create       BmpCreate;
	PFN_FPDFBitmap_FillRect     BmpFill;
	PFN_FPDF_RenderPageBitmap   RenderPage;
	PFN_FPDFBitmap_GetBuffer    BmpGetBuf;
	PFN_FPDFBitmap_GetStride    BmpGetStride;
	PFN_FPDFBitmap_Destroy      BmpDestroy;
};

static PdfiumFuncs g_pdf = {};

static bool LoadPdfium()
{
	if (g_pdf.hDll) return true;

	// 이 DLL과 같은 디렉터리에서 pdfium.dll 로드
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(g_hModule, path, MAX_PATH);
	PathRemoveFileSpecW(path);
	PathAppendW(path, L"pdfium.dll");

	g_pdf.hDll = LoadLibraryW(path);
	if (!g_pdf.hDll) return false;

#define LOAD(name, type) g_pdf.name = (type)GetProcAddress(g_pdf.hDll, #name); \
	if (!g_pdf.name) { FreeLibrary(g_pdf.hDll); g_pdf.hDll = nullptr; return false; }

	LOAD(FPDF_InitLibrary,       PFN_FPDF_InitLibrary)
	LOAD(FPDF_DestroyLibrary,    PFN_FPDF_DestroyLibrary)
	LOAD(FPDF_LoadMemDocument,   PFN_FPDF_LoadMemDocument)
	LOAD(FPDF_CloseDocument,     PFN_FPDF_CloseDocument)
	LOAD(FPDF_GetPageCount,      PFN_FPDF_GetPageCount)
	LOAD(FPDF_LoadPage,          PFN_FPDF_LoadPage)
	LOAD(FPDF_ClosePage,         PFN_FPDF_ClosePage)
	LOAD(FPDF_GetPageWidthF,     PFN_FPDF_GetPageWidthF)
	LOAD(FPDF_GetPageHeightF,    PFN_FPDF_GetPageHeightF)
	LOAD(FPDFBitmap_Create,      PFN_FPDFBitmap_Create)
	LOAD(FPDFBitmap_FillRect,    PFN_FPDFBitmap_FillRect)
	LOAD(FPDF_RenderPageBitmap,  PFN_FPDF_RenderPageBitmap)
	LOAD(FPDFBitmap_GetBuffer,   PFN_FPDFBitmap_GetBuffer)
	LOAD(FPDFBitmap_GetStride,   PFN_FPDFBitmap_GetStride)
	LOAD(FPDFBitmap_Destroy,     PFN_FPDFBitmap_Destroy)
#undef LOAD

	g_pdf.Init();
	return true;
}

// -----------------------------------------------------------------------
// file.png 리소스 로드 (RT_RCDATA "FILE_ICON_PNG")
// -----------------------------------------------------------------------
static Gdiplus::Bitmap* LoadFilePngOverlay()
{
	HRSRC   hRes  = FindResourceW(g_hModule, L"FILE_ICON_PNG", RT_RCDATA);
	if (!hRes) return nullptr;
	HGLOBAL hGlob = LoadResource(g_hModule, hRes);
	if (!hGlob) return nullptr;
	void*  pData  = LockResource(hGlob);
	DWORD  size   = SizeofResource(g_hModule, hRes);
	if (!pData || size == 0) return nullptr;

	HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, size);
	if (!hBuf) return nullptr;
	void* pBuf = GlobalLock(hBuf);
	memcpy(pBuf, pData, size);
	GlobalUnlock(hBuf);

	IStream* pStream = nullptr;
	if (FAILED(CreateStreamOnHGlobal(hBuf, TRUE, &pStream))) return nullptr;

	Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(pStream);
	pStream->Release();
	return bmp;
}

// -----------------------------------------------------------------------
// HBITMAP 생성: PDFium으로 첫 페이지 렌더 + file.png 우측 하단 오버레이
// -----------------------------------------------------------------------
static HBITMAP RenderPdfThumbnail(const BYTE* pdfData, DWORD dataSize, UINT cx)
{
	if (!LoadPdfium()) return nullptr;

	FPDF_DOCUMENT doc = g_pdf.LoadMemDoc(pdfData, (int)dataSize, nullptr);
	if (!doc) return nullptr;

	FPDF_PAGE page = g_pdf.LoadPage(doc, 0);
	if (!page) { g_pdf.CloseDoc(doc); return nullptr; }

	double pageW = g_pdf.GetPageW(page);
	double pageH = g_pdf.GetPageH(page);
	if (pageW <= 0 || pageH <= 0) { g_pdf.ClosePage(page); g_pdf.CloseDoc(doc); return nullptr; }

	// 썸네일 크기 계산 (비율 유지)
	int thumbW, thumbH;
	if (pageW >= pageH)
	{
		thumbW = (int)cx;
		thumbH = (int)(cx * pageH / pageW);
	}
	else
	{
		thumbH = (int)cx;
		thumbW = (int)(cx * pageW / pageH);
	}
	if (thumbW < 1) thumbW = 1;
	if (thumbH < 1) thumbH = 1;

	// PDFium BGRA 비트맵
	FPDF_BITMAP fpdfBmp = g_pdf.BmpCreate(thumbW, thumbH, 1);
	g_pdf.BmpFill(fpdfBmp, 0, 0, thumbW, thumbH, 0xFFFFFFFF);
	g_pdf.RenderPage(fpdfBmp, page, 0, 0, thumbW, thumbH, 0, FPDF_LCD_TEXT | FPDF_ANNOT);

	void* pBuf   = g_pdf.BmpGetBuf(fpdfBmp);
	int   stride = g_pdf.BmpGetStride(fpdfBmp);

	// GDI+ 비트맵에 복사
	Gdiplus::Bitmap gdiBmp(thumbW, thumbH, stride,
		PixelFormat32bppARGB, (BYTE*)pBuf);

	// file.png 오버레이 (우측 하단, 썸네일 크기의 30%)
	Gdiplus::Bitmap* overlay = LoadFilePngOverlay();
	if (overlay && overlay->GetLastStatus() == Gdiplus::Ok)
	{
		Gdiplus::Graphics g(&gdiBmp);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		int ovSize = thumbW * 30 / 100;
		if (ovSize < 16) ovSize = 16;
		int ovX = thumbW - ovSize - 2;
		int ovY = thumbH - ovSize - 2;
		g.DrawImage(overlay, ovX, ovY, ovSize, ovSize);
		delete overlay;
	}

	// GDI+ Bitmap → HBITMAP (32bpp PARGB)
	HBITMAP hBmp = nullptr;
	gdiBmp.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hBmp);

	g_pdf.BmpDestroy(fpdfBmp);
	g_pdf.ClosePage(page);
	g_pdf.CloseDoc(doc);

	return hBmp;
}

// -----------------------------------------------------------------------
// CPdfThumbnailProvider 클래스
// -----------------------------------------------------------------------
class CPdfThumbnailProvider : public IThumbnailProvider,
							  public IInitializeWithStream
{
public:
	CPdfThumbnailProvider() : m_ref(1), m_pStream(nullptr)
	{
		InterlockedIncrement(&g_refCount);
	}

	~CPdfThumbnailProvider()
	{
		if (m_pStream) { m_pStream->Release(); m_pStream = nullptr; }
		InterlockedDecrement(&g_refCount);
	}

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (!ppv) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IThumbnailProvider)
			*ppv = static_cast<IThumbnailProvider*>(this);
		else if (riid == IID_IInitializeWithStream)
			*ppv = static_cast<IInitializeWithStream*>(this);
		else { *ppv = nullptr; return E_NOINTERFACE; }
		AddRef();
		return S_OK;
	}
	STDMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&m_ref); }
	STDMETHODIMP_(ULONG) Release() override
	{
		ULONG r = InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}

	// IInitializeWithStream
	STDMETHODIMP Initialize(IStream* pStream, DWORD /*grfMode*/) override
	{
		if (m_pStream) { m_pStream->Release(); m_pStream = nullptr; }
		m_pStream = pStream;
		if (m_pStream) m_pStream->AddRef();
		return S_OK;
	}

	// IThumbnailProvider
	STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override
	{
		if (!phbmp || !pdwAlpha) return E_POINTER;
		*phbmp    = nullptr;
		*pdwAlpha = WTSAT_ARGB;

		if (!m_pStream) return E_FAIL;

		// 스트림을 메모리로 읽기
		STATSTG stat = {};
		if (FAILED(m_pStream->Stat(&stat, STATFLAG_NONAME))) return E_FAIL;
		DWORD size = (DWORD)stat.cbSize.QuadPart;
		if (size == 0) return E_FAIL;

		std::vector<BYTE> buf(size);
		ULONG read = 0;
		LARGE_INTEGER li = {};
		m_pStream->Seek(li, STREAM_SEEK_SET, nullptr);
		if (FAILED(m_pStream->Read(buf.data(), size, &read)) || read != size)
			return E_FAIL;

		*phbmp = RenderPdfThumbnail(buf.data(), size, cx);
		return *phbmp ? S_OK : E_FAIL;
	}

private:
	LONG    m_ref;
	IStream* m_pStream;
	std::vector<BYTE> dummy; // vector 헤더 포함 보장용
};

// vector 사용을 위한 include (위에서 이미 포함되지만 명시)
#include <vector>

// -----------------------------------------------------------------------
// Class Factory
// -----------------------------------------------------------------------
class CPdfThumbnailProviderFactory : public IClassFactory
{
public:
	CPdfThumbnailProviderFactory() : m_ref(1) {}

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (!ppv) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IClassFactory)
			{ *ppv = this; AddRef(); return S_OK; }
		*ppv = nullptr; return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&m_ref); }
	STDMETHODIMP_(ULONG) Release() override
	{
		ULONG r = InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}
	STDMETHODIMP CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) override
	{
		if (!ppv) return E_POINTER;
		if (pOuter) return CLASS_E_NOAGGREGATION;
		CPdfThumbnailProvider* p = new (std::nothrow) CPdfThumbnailProvider();
		if (!p) return E_OUTOFMEMORY;
		HRESULT hr = p->QueryInterface(riid, ppv);
		p->Release();
		return hr;
	}
	STDMETHODIMP LockServer(BOOL fLock) override
	{
		if (fLock) InterlockedIncrement(&g_refCount);
		else       InterlockedDecrement(&g_refCount);
		return S_OK;
	}

private:
	LONG m_ref;
};

// -----------------------------------------------------------------------
// GDI+ 수명 관리
// -----------------------------------------------------------------------
static ULONG_PTR g_gdiplusToken = 0;

// -----------------------------------------------------------------------
// DLL 진입점
// -----------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		g_hModule = hModule;
		DisableThreadLibraryCalls(hModule);
		{
			Gdiplus::GdiplusStartupInput si;
			Gdiplus::GdiplusStartup(&g_gdiplusToken, &si, nullptr);
		}
		break;

	case DLL_PROCESS_DETACH:
		if (g_pdf.hDll)
		{
			g_pdf.Destroy();
			FreeLibrary(g_pdf.hDll);
			g_pdf = {};
		}
		if (g_gdiplusToken)
		{
			Gdiplus::GdiplusShutdown(g_gdiplusToken);
			g_gdiplusToken = 0;
		}
		break;
	}
	return TRUE;
}

// -----------------------------------------------------------------------
// COM 익스포트
// -----------------------------------------------------------------------
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
	if (!ppv) return E_POINTER;
	if (rclsid != CLSID_PdfThumbnailProvider) return CLASS_E_CLASSNOTAVAILABLE;
	CPdfThumbnailProviderFactory* factory = new (std::nothrow) CPdfThumbnailProviderFactory();
	if (!factory) return E_OUTOFMEMORY;
	HRESULT hr = factory->QueryInterface(riid, ppv);
	factory->Release();
	return hr;
}

STDAPI DllCanUnloadNow()
{
	return g_refCount == 0 ? S_OK : S_FALSE;
}
