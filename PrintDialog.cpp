// ---------------------------------------------------------------
// PrintDialog.cpp - 미리보기가 포함된 인쇄 대화상자
// ---------------------------------------------------------------
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <winspool.h>
#include <algorithm>
#include <string>
#include <vector>
#include <mutex>
#include "PrintDialog.h"
#include "Lang.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winspool.lib")

namespace
{
	const wchar_t* PD_CLASS = L"PlainPrintDlgClass";
	const wchar_t* PV_CLASS = L"PlainPrintPreviewClass";

	enum
	{
		ID_COMBO = 101, ID_PROPS, ID_COPIES, ID_RAD_ALL, ID_RAD_CUR, ID_RAD_RNG,
		ID_ED_FROM, ID_ED_TO, ID_RAD_AUTO, ID_RAD_PORT, ID_RAD_LAND,
		ID_RAD_FIT, ID_RAD_ACT, ID_PREV, ID_NEXT, ID_RAD_POSTER, ID_ED_COLS, ID_ED_ROWS,
		ID_RAD_TWOUP, ID_COPY_DEC, ID_COPY_INC
	};

	// 미리보기용 렌더 캐시 한 칸
	struct PvCache { FPDF_BITMAP bmp = nullptr; int page = -1, w = 0, h = 0, rot = -1; };

	constexpr int DLG_CW = 900;
	constexpr int DLG_CH = 590;
	const RECT NAV_LABEL_RC = { 586, 530, 690, 562 };

	// Windows 11 풍 색
	const COLORREF kWinBg   = RGB(243, 243, 246);   // 창 바탕
	const COLORREF kEditOff = RGB(245, 245, 247);   // 꺼진 입력 상자 바탕
	const Gdiplus::Color kAccent(255, 0, 95, 184);
	const Gdiplus::Color kAccentText(255, 0, 84, 166);
	Gdiplus::Color ToColor(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

	struct Paper
	{
		int physW = 4961, physH = 7016;
		int printW = 4961, printH = 7016;
		int offX = 0, offY = 0;
		int dpiX = 600, dpiY = 600;
	};

	struct State
	{
		HWND hDlg = nullptr, hPreview = nullptr, hCombo = nullptr, hProps = nullptr;
		HWND hEdCopies = nullptr, hEdFrom = nullptr, hEdTo = nullptr;
		HWND hRadAll = nullptr, hRadCur = nullptr, hRadRng = nullptr;
		HWND hRadAuto = nullptr, hRadPort = nullptr, hRadLand = nullptr;
		HWND hRadFit = nullptr, hRadAct = nullptr;
		HWND hRadPoster = nullptr, hEdCols = nullptr, hEdRows = nullptr;
		HWND hRadTwoUp = nullptr;
		HWND hCopyDec = nullptr, hCopyInc = nullptr;
		PvCache two[2];               // 2쪽 모아찍기 미리보기의 두 페이지
		HWND hPrev = nullptr, hNext = nullptr, hPrint = nullptr, hCancel = nullptr;
		FPDF_DOCUMENT doc = nullptr;
		int  pageCount = 0, curPage = 0;
		std::vector<int> rotations;   // 페이지별 실효 회전(0~3)
		std::mutex* mtx = nullptr;

		std::vector<std::wstring> printers;
		int sel = -1;
		std::vector<BYTE> devmode;
		Paper paper;
		int previewPage = 0;

		FPDF_BITMAP cache = nullptr;
		int cPage = -1, cW = 0, cH = 0, cRot = -1;

		int  hotId = 0;
		bool ready = false, done = false, ok = false;
		bool kbdFocus = false;        // 키보드로 이동했을 때만 포커스 링 표시
		HFONT  font = nullptr;
		HBRUSH brWhite = nullptr, brDisabled = nullptr;
		PrintDialogResult* out = nullptr;
	};

	State* g = nullptr;

	const wchar_t* T(const wchar_t* ko, const wchar_t* en)
	{
		if (g_appLang == AppLang::Korean) return ko;
		if (g_appLang == AppLang::English) return en;
		static const wchar_t* const tbl[][8] = {
#include "PrintDialogStr.inc"
		};
		int idx = (int)g_appLang - 2;
		if (idx < 0 || idx > 6) return en;
		for (auto& row : tbl)
			if (wcscmp(row[0], en) == 0) return row[idx + 1];
		return en;
	}

	// 선택지(라디오)는 소유자 그리기 버튼이라 선택 상태를 창 사용자 데이터에 둔다
	bool Checked(HWND h) { return h && GetWindowLongPtrW(h, GWLP_USERDATA) != 0; }

	void SetCheck(HWND h, bool on)
	{
		if (!h) return;
		SetWindowLongPtrW(h, GWLP_USERDATA, on ? 1 : 0);
		InvalidateRect(h, nullptr, FALSE);
	}

	bool IsRadioId(int id)
	{
		return (id >= ID_RAD_ALL && id <= ID_RAD_RNG) || (id >= ID_RAD_AUTO && id <= ID_RAD_ACT)
			|| id == ID_RAD_POSTER || id == ID_RAD_TWOUP;
	}

	// 같은 묶음 안에서 하나만 켠다
	void SelectRadio(int id)
	{
		State* s = g;
		const std::vector<HWND> groups[] = {
			{ s->hRadAll, s->hRadCur, s->hRadRng },
			{ s->hRadAuto, s->hRadPort, s->hRadLand },
			{ s->hRadFit, s->hRadAct, s->hRadPoster, s->hRadTwoUp } };
		HWND target = GetDlgItem(s->hDlg, id);
		for (const auto& grp : groups)
			if (std::find(grp.begin(), grp.end(), target) != grp.end())
				for (HWND b : grp) SetCheck(b, b == target);
	}

	int ReadInt(HWND h, int def)
	{
		wchar_t b[16] = {};
		GetWindowTextW(h, b, 16);
		return b[0] ? _wtoi(b) : def;
	}

	void GetRange(int& f, int& t)
	{
		State* s = g;
		const int pc = s->pageCount;
		if (Checked(s->hRadCur))
		{
			f = t = std::clamp(s->curPage, 0, pc - 1);
			return;
		}
		if (Checked(s->hRadRng))
		{
			int a = std::clamp(ReadInt(s->hEdFrom, 1), 1, pc);
			int b = std::clamp(ReadInt(s->hEdTo, pc), 1, pc);
			if (b < a) std::swap(a, b);
			f = a - 1; t = b - 1;
			return;
		}
		f = 0; t = pc - 1;
	}

	bool TwoUp() { return Checked(g->hRadTwoUp); }

	// 2쪽 모아찍기에서 page가 들어 있는 장의 첫 페이지 (범위 시작 f부터 두 쪽씩 묶는다)
	int SheetStart(int page, int f) { return f + ((page - f) / 2) * 2; }

	int Rotation(int page)
	{
		const auto& r = g->rotations;
		return (page >= 0 && page < (int)r.size()) ? r[page] : 0;
	}

	bool PageSize(int idx, double& w, double& h)
	{
		State* s = g;
		{
			std::lock_guard<std::mutex> lk(*s->mtx);
			if (FPDF_GetPageSizeByIndex(s->doc, idx, &w, &h) == 0) return false;
		}
		const int rot = Rotation(idx);
		if (rot == 1 || rot == 3) std::swap(w, h);
		return w > 0.0 && h > 0.0;
	}

	bool LoadDevMode(const std::wstring& name, std::vector<BYTE>& dm)
	{
		HANDLE hp = nullptr;
		if (!OpenPrinterW(const_cast<LPWSTR>(name.c_str()), &hp, nullptr)) return false;
		LONG sz = DocumentPropertiesW(nullptr, hp, const_cast<LPWSTR>(name.c_str()), nullptr, nullptr, 0);
		bool ok = false;
		if (sz > 0)
		{
			dm.assign((size_t)sz, 0);
			ok = DocumentPropertiesW(nullptr, hp, const_cast<LPWSTR>(name.c_str()),
					 reinterpret_cast<DEVMODEW*>(dm.data()), nullptr, DM_OUT_BUFFER) == IDOK;
		}
		ClosePrinter(hp);
		if (!ok) dm.clear();
		return ok;
	}

	std::vector<BYTE> BuildDevMode()
	{
		State* s = g;
		std::vector<BYTE> dm = s->devmode;
		if (dm.size() < sizeof(DEVMODEW)) return {};
		DEVMODEW* d = reinterpret_cast<DEVMODEW*>(dm.data());

		short o;
		if (Checked(s->hRadPort))      o = DMORIENT_PORTRAIT;
		else if (Checked(s->hRadLand)) o = DMORIENT_LANDSCAPE;
		else
		{
			int f, t; GetRange(f, t);
			double w = 0, h = 0;
			const bool landscapePage = PageSize(f, w, h) && w > h;
			// 2쪽 모아찍기: 세로 페이지는 가로 용지에 나란히, 가로 페이지는 세로 용지에 위아래로
			if (TwoUp()) o = landscapePage ? DMORIENT_PORTRAIT : DMORIENT_LANDSCAPE;
			else         o = landscapePage ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
		}
		d->dmFields |= DM_ORIENTATION | DM_COPIES;
		d->dmOrientation = o;
		d->dmCopies = 1;
		return dm;
	}

	void UpdatePaper()
	{
		State* s = g;
		Paper p;
		if (s->sel >= 0 && s->sel < (int)s->printers.size())
		{
			std::vector<BYTE> dm = BuildDevMode();
			HDC ic = CreateICW(L"WINSPOOL", s->printers[s->sel].c_str(), nullptr,
							   dm.empty() ? nullptr : reinterpret_cast<DEVMODEW*>(dm.data()));
			if (ic)
			{
				int pw = GetDeviceCaps(ic, PHYSICALWIDTH),  ph = GetDeviceCaps(ic, PHYSICALHEIGHT);
				int rw = GetDeviceCaps(ic, HORZRES),        rh = GetDeviceCaps(ic, VERTRES);
				int dx = GetDeviceCaps(ic, LOGPIXELSX),     dy = GetDeviceCaps(ic, LOGPIXELSY);
				if (pw > 0 && ph > 0 && rw > 0 && rh > 0 && dx > 0 && dy > 0)
				{
					p.physW = pw; p.physH = ph; p.printW = rw; p.printH = rh;
					p.offX = GetDeviceCaps(ic, PHYSICALOFFSETX);
					p.offY = GetDeviceCaps(ic, PHYSICALOFFSETY);
					p.dpiX = dx; p.dpiY = dy;
				}
				DeleteDC(ic);
			}
		}
		s->paper = p;
	}

	void ComputeRender(double pdfW, double pdfH, const Paper& p, bool fit, int& rw, int& rh)
	{
		double wpx = pdfW * p.dpiX / 72.0;
		double hpx = pdfH * p.dpiY / 72.0;
		double sc = 1.0;
		if (fit) sc = std::min((double)p.printW / wpx, (double)p.printH / hpx);
		rw = (int)(wpx * sc);
		rh = (int)(hpx * sc);
	}

	void FreeCache()
	{
		if (g->cache) { FPDFBitmap_Destroy(g->cache); g->cache = nullptr; }
		g->cPage = -1;
		for (auto& c : g->two)
		{
			if (c.bmp) FPDFBitmap_Destroy(c.bmp);
			c = PvCache{};
		}
	}

	// 캐시 칸 c에 page를 w×h로 렌더해 둔다 (같으면 다시 그리지 않음)
	FPDF_BITMAP CachedRender(PvCache& c, int page, int w, int h)
	{
		State* s = g;
		if (c.bmp && c.page == page && c.w == w && c.h == h && c.rot == Rotation(page)) return c.bmp;
		if (c.bmp) { FPDFBitmap_Destroy(c.bmp); c = PvCache{}; }

		std::lock_guard<std::mutex> lk(*s->mtx);
		FPDF_PAGE pg = FPDF_LoadPage(s->doc, page);
		if (!pg) return nullptr;
		FPDF_BITMAP b = FPDFBitmap_Create(w, h, 0);
		if (b)
		{
			FPDFBitmap_FillRect(b, 0, 0, w, h, 0xFFFFFFFF);
			FPDF_RenderPageBitmap(b, pg, 0, 0, w, h, Rotation(page), FPDF_ANNOT | FPDF_LCD_TEXT);
			c.bmp = b; c.page = page; c.w = w; c.h = h; c.rot = Rotation(page);
		}
		FPDF_ClosePage(pg);
		return b;
	}

	bool EnsureCache(int page, int w, int h)
	{
		State* s = g;
		if (s->cache && s->cPage == page && s->cW == w && s->cH == h && s->cRot == Rotation(page)) return true;
		FreeCache();

		std::lock_guard<std::mutex> lk(*s->mtx);
		FPDF_PAGE pg = FPDF_LoadPage(s->doc, page);
		if (!pg) return false;
		FPDF_BITMAP b = FPDFBitmap_Create(w, h, 0);
		if (b)
		{
			FPDFBitmap_FillRect(b, 0, 0, w, h, 0xFFFFFFFF);
			FPDF_RenderPageBitmap(b, pg, 0, 0, w, h, Rotation(page), FPDF_ANNOT | FPDF_LCD_TEXT);
		}
		FPDF_ClosePage(pg);
		if (!b) return false;
		s->cache = b; s->cPage = page; s->cW = w; s->cH = h; s->cRot = Rotation(page);
		return true;
	}

	void RoundPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r)
	{
		p.AddArc(x, y, r * 2, r * 2, 180.f, 90.f);
		p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270.f, 90.f);
		p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0.f, 90.f);
		p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90.f, 90.f);
		p.CloseFigure();
	}

	// 미리보기 영역: 창 바탕 위에 둥근 회색 패널
	void PreviewBackground(HDC dc, const RECT& rc)
	{
		HBRUSH b = CreateSolidBrush(kWinBg); FillRect(dc, &rc, b); DeleteObject(b);
		Gdiplus::Graphics gfx(dc);
		gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		Gdiplus::GraphicsPath path;
		RoundPath(path, 0.5f, 0.5f, rc.right - 1.f, rc.bottom - 1.f, 8.f);
		Gdiplus::SolidBrush fill(Gdiplus::Color(255, 232, 233, 238));
		gfx.FillPath(&fill, &path);
		Gdiplus::Pen pen(Gdiplus::Color(255, 222, 223, 229), 1.f);
		gfx.DrawPath(&pen, &path);
	}

	// 용지 그림자: 아래쪽으로 살짝 퍼지는 부드러운 그림자 (반투명 여러 겹)
	void PaperShadow(HDC dc, const RECT& r)
	{
		Gdiplus::Graphics gfx(dc);
		gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		for (int k = 4; k >= 1; --k)
		{
			Gdiplus::GraphicsPath path;
			RoundPath(path, (float)r.left - k, (float)r.top - k + 2, (float)(r.right - r.left) + 2.f * k,
					  (float)(r.bottom - r.top) + 2.f * k, (float)k + 1.f);
			Gdiplus::SolidBrush b(Gdiplus::Color((BYTE)(10 + (4 - k) * 6), 0, 0, 0));
			gfx.FillPath(&b, &path);
		}
	}

	// 2쪽 모아찍기 미리보기: 용지 한 장에 지금 장의 두 페이지를 실제 인쇄와 같은 배치로 그린다
	void PaintTwoUpPreview(HDC dc, const RECT& rc, double aw, double ah)
	{
		State* s = g;
		const Paper& p = s->paper;
		const double k = std::min(aw / p.physW, ah / p.physH);
		const int pw = std::max(1, (int)(p.physW * k)), ph = std::max(1, (int)(p.physH * k));
		const int px = (rc.right - pw) / 2, py = (rc.bottom - ph) / 2;

		RECT paper = { px, py, px + pw, py + ph };
		PaperShadow(dc, paper);
		HBRUSH bw = CreateSolidBrush(RGB(255, 255, 255)); FillRect(dc, &paper, bw); DeleteObject(bw);
		HBRUSH bb = CreateSolidBrush(RGB(214, 215, 221)); FrameRect(dc, &paper, bb);

		int f, t; GetRange(f, t);
		const int first = SheetStart(s->previewPage, f);
		int saved = SaveDC(dc);
		IntersectClipRect(dc, px + 1, py + 1, px + pw - 1, py + ph - 1);
		for (int slot = 0; slot < 2; ++slot)
		{
			const int page = first + slot;
			double pdfW = 0, pdfH = 0;
			if (page > t || !PageSize(page, pdfW, pdfH)) continue;
			double x, y, w, h;
			TwoUpCell(slot, p.printW, p.printH, pdfW * p.dpiX / 72.0, pdfH * p.dpiY / 72.0, x, y, w, h);
			const int dx = px + (int)((p.offX + x) * k), dy = py + (int)((p.offY + y) * k);
			const int dw = std::max(1, (int)(w * k)), dh = std::max(1, (int)(h * k));
			if (dw > 6000 || dh > 6000) continue;
			FPDF_BITMAP bmp = CachedRender(s->two[slot], page, dw, dh);
			if (!bmp) continue;
			BITMAPINFO bmi = {};
			bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth       = FPDFBitmap_GetStride(bmp) / 4;
			bmi.bmiHeader.biHeight      = -dh;
			bmi.bmiHeader.biPlanes      = 1;
			bmi.bmiHeader.biBitCount    = 32;
			bmi.bmiHeader.biCompression = BI_RGB;
			StretchDIBits(dc, dx, dy, dw, dh, 0, 0, dw, dh, FPDFBitmap_GetBuffer(bmp), &bmi, DIB_RGB_COLORS, SRCCOPY);
			// 흰 용지 위에서 페이지 경계가 보이도록 미리보기에만 옅은 테두리
			RECT pr = { dx, dy, dx + dw, dy + dh };
			FrameRect(dc, &pr, bb);
		}
		RestoreDC(dc, saved);
		DeleteObject(bb);
	}

	void PaintPreview(HDC dc, const RECT& rc)
	{
		State* s = g;
		PreviewBackground(dc, rc);

		const Paper& p = s->paper;
		const int M = 18;
		double aw = rc.right - 2.0 * M, ah = rc.bottom - 2.0 * M;
		if (aw < 10 || ah < 10) return;

		if (TwoUp())
		{
			PaintTwoUpPreview(dc, rc, aw, ah);
			return;
		}

		if (Checked(s->hRadPoster))
		{
			const int cols = std::clamp(ReadInt(s->hEdCols, 2), 1, 10);
			const int rows = std::clamp(ReadInt(s->hEdRows, 2), 1, 10);
			const double ovX = POSTER_OVERLAP_MM / 25.4 * p.dpiX, ovY = POSTER_OVERLAP_MM / 25.4 * p.dpiY;
			const double stepX = std::max(1.0, p.printW - ovX), stepY = std::max(1.0, p.printH - ovY);
			const double totW = stepX * (cols - 1) + p.printW, totH = stepY * (rows - 1) + p.printH;
			const double kk = std::min(aw / totW, ah / totH);
			const int tw = std::max(1, (int)(totW * kk)), th = std::max(1, (int)(totH * kk));
			const int tx = (rc.right - tw) / 2, ty = (rc.bottom - th) / 2;
			RECT area = { tx, ty, tx + tw, ty + th };
			PaperShadow(dc, area);
			HBRUSH bw2 = CreateSolidBrush(RGB(255, 255, 255)); FillRect(dc, &area, bw2); DeleteObject(bw2);

			double pw0 = 0, ph0 = 0;
			if (PageSize(s->previewPage, pw0, ph0))
			{
				const double sc = std::min(totW / (pw0 * p.dpiX / 72.0), totH / (ph0 * p.dpiY / 72.0));
				const int dw = std::max(1, (int)(pw0 * p.dpiX / 72.0 * sc * kk));
				const int dh = std::max(1, (int)(ph0 * p.dpiY / 72.0 * sc * kk));
				if (dw <= 6000 && dh <= 6000 && EnsureCache(s->previewPage, dw, dh))
				{
					BITMAPINFO bmi = {};
					bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
					bmi.bmiHeader.biWidth       = FPDFBitmap_GetStride(s->cache) / 4;
					bmi.bmiHeader.biHeight      = -dh;
					bmi.bmiHeader.biPlanes      = 1;
					bmi.bmiHeader.biBitCount    = 32;
					bmi.bmiHeader.biCompression = BI_RGB;
					int saved = SaveDC(dc);
					IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
					StretchDIBits(dc, tx + (tw - dw) / 2, ty + (th - dh) / 2, dw, dh, 0, 0, dw, dh,
								  FPDFBitmap_GetBuffer(s->cache), &bmi, DIB_RGB_COLORS, SRCCOPY);
					RestoreDC(dc, saved);
				}
			}

			HPEN pen = CreatePen(PS_DOT, 1, RGB(255, 99, 71));
			HGDIOBJ oldPen = SelectObject(dc, pen);
			HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
			int oldBk = SetBkMode(dc, TRANSPARENT);
			COLORREF oldTc = SetTextColor(dc, RGB(255, 99, 71));
			HGDIOBJ oldFont = SelectObject(dc, s->font);
			for (int r = 0; r < rows; ++r)
				for (int c = 0; c < cols; ++c)
				{
					int x0 = tx + (int)(c * stepX * kk), y0 = ty + (int)(r * stepY * kk);
					int x1 = x0 + (int)(p.printW * kk), y1 = y0 + (int)(p.printH * kk);
					Rectangle(dc, x0, y0, x1, y1);
					wchar_t nb[16]; swprintf_s(nb, L"%d", r * cols + c + 1);
					RECT tr = { x0 + 4, y0 + 2, x1, y1 };
					DrawTextW(dc, nb, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE);
				}
			SelectObject(dc, oldFont);
			SetTextColor(dc, oldTc); SetBkMode(dc, oldBk);
			SelectObject(dc, oldBr); SelectObject(dc, oldPen); DeleteObject(pen);
			return;
		}
		double k = std::min(aw / p.physW, ah / p.physH);
		int pw = std::max(1, (int)(p.physW * k)), ph = std::max(1, (int)(p.physH * k));
		int px = (rc.right - pw) / 2, py = (rc.bottom - ph) / 2;

		RECT paper = { px, py, px + pw, py + ph };
		PaperShadow(dc, paper);
		HBRUSH bw = CreateSolidBrush(RGB(255, 255, 255)); FillRect(dc, &paper, bw); DeleteObject(bw);
		HBRUSH bb = CreateSolidBrush(RGB(214, 215, 221)); FrameRect(dc, &paper, bb); DeleteObject(bb);

		double pdfW = 0, pdfH = 0;
		if (!PageSize(s->previewPage, pdfW, pdfH)) return;

		int rw, rh;
		ComputeRender(pdfW, pdfH, p, Checked(s->hRadFit), rw, rh);
		int dw = std::max(1, (int)(rw * k)), dh = std::max(1, (int)(rh * k));
		int dx = px + (int)((p.offX + (p.printW - rw) / 2.0) * k);
		int dy = py + (int)((p.offY + (p.printH - rh) / 2.0) * k);
		if (dw > 6000 || dh > 6000) return;
		if (!EnsureCache(s->previewPage, dw, dh)) return;

		int stride = FPDFBitmap_GetStride(s->cache);
		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth       = stride / 4;
		bmi.bmiHeader.biHeight      = -dh;
		bmi.bmiHeader.biPlanes      = 1;
		bmi.bmiHeader.biBitCount    = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		int saved = SaveDC(dc);
		IntersectClipRect(dc, px + 1, py + 1, px + pw - 1, py + ph - 1);
		StretchDIBits(dc, dx, dy, dw, dh, 0, 0, dw, dh,
					  FPDFBitmap_GetBuffer(s->cache), &bmi, DIB_RGB_COLORS, SRCCOPY);
		RestoreDC(dc, saved);
	}

	void UpdateNav()
	{
		State* s = g;
		int f, t; GetRange(f, t);
		const int step = TwoUp() ? 2 : 1;
		EnableWindow(s->hPrev, s->previewPage > f);
		EnableWindow(s->hNext, s->previewPage + step <= t);
		InvalidateRect(s->hDlg, &NAV_LABEL_RC, FALSE);
	}

	void OnChanged()
	{
		State* s = g;
		if (!s->ready) return;
		int f, t; GetRange(f, t);
		s->previewPage = std::clamp(s->previewPage, f, t);
		if (TwoUp()) s->previewPage = SheetStart(s->previewPage, f); // 장의 첫 페이지로 맞춘다
		UpdatePaper();
		UpdateNav();
		InvalidateRect(s->hPreview, nullptr, FALSE);
	}

	// Segoe 아이콘 글꼴 (MDL2 우선, 없으면 Fluent Icons)
	const wchar_t* IconFontName()
	{
		static const wchar_t* name = nullptr;
		if (!name)
		{
			Gdiplus::FontFamily f(L"Segoe MDL2 Assets");
			name = f.IsAvailable() ? L"Segoe MDL2 Assets" : L"Segoe Fluent Icons";
		}
		return name;
	}

	enum class BtnKind { Primary, Secondary, Nav, Segment, Stepper, Select };

	BtnKind KindOf(int id)
	{
		if (id == IDOK) return BtnKind::Primary;
		if (id == ID_PREV || id == ID_NEXT) return BtnKind::Nav;
		if (id == ID_COPY_DEC || id == ID_COPY_INC) return BtnKind::Stepper;
		if (id == ID_COMBO) return BtnKind::Select;
		if (IsRadioId(id)) return BtnKind::Segment;
		return BtnKind::Secondary;
	}

	// Windows 11 풍 버튼: 기본(강조색), 보조(흰 바탕+테두리), 선택지(켜지면 옅은 강조색),
	// 부수 −/+, 프린터 선택 상자, 미리보기 넘김(둥근 투명 버튼)
	void DrawBtn(DRAWITEMSTRUCT* di)
	{
		using namespace Gdiplus;
		State* s = g;
		RECT rc = di->rcItem;
		int w = rc.right - rc.left, h = rc.bottom - rc.top;
		HDC mem = CreateCompatibleDC(di->hDC);
		HBITMAP bm = CreateCompatibleBitmap(di->hDC, w, h);
		HGDIOBJ old = SelectObject(mem, bm);
		{
			Graphics gfx(mem);
			gfx.SetSmoothingMode(SmoothingModeAntiAlias);
			gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

			const int id = (int)di->CtlID;
			const BtnKind kind = KindOf(id);
			const bool pressed = (di->itemState & ODS_SELECTED) != 0;
			const bool dis = (di->itemState & ODS_DISABLED) != 0;
			const bool hot = !dis && s->hotId == id;
			const bool focus = (di->itemState & ODS_FOCUS) != 0 && s->kbdFocus;
			const bool on = kind == BtnKind::Segment && Checked(di->hwndItem);

			// 버튼 바깥: 미리보기 넘김 버튼은 창 바탕, 나머지는 흰 카드 위
			gfx.Clear(kind == BtnKind::Nav ? ToColor(kWinBg) : Color(255, 255, 255, 255));

			GraphicsPath path;
			const float rad = (kind == BtnKind::Nav) ? (h - 1) / 2.f : 6.f;
			RoundPath(path, 0.5f, 0.5f, (float)w - 1.f, (float)h - 1.f, rad);

			Color fill(0, 0, 0, 0), border(0, 0, 0, 0), txt(255, 28, 28, 32);
			switch (kind)
			{
			case BtnKind::Primary:
				fill = dis ? Color(255, 200, 200, 204)
					 : pressed ? Color(255, 49, 129, 200)
					 : hot ? Color(255, 25, 110, 191) : kAccent;
				txt = Color(255, 255, 255, 255);
				break;
			case BtnKind::Nav:
				if (!dis && (hot || pressed))
					fill = pressed ? Color(255, 214, 215, 221) : Color(255, 226, 227, 233);
				txt = dis ? Color(255, 182, 183, 188) : Color(255, 60, 60, 66);
				break;
			case BtnKind::Segment:
				if (on)
				{
					fill = Color(255, 229, 238, 251);
					border = kAccent;
					txt = dis ? Color(255, 150, 150, 156) : kAccentText;
				}
				else
				{
					fill = pressed ? Color(255, 230, 230, 234) : hot ? Color(255, 238, 238, 241) : Color(255, 245, 245, 247);
					txt = dis ? Color(255, 160, 160, 166) : Color(255, 50, 50, 56);
				}
				break;
			default:   // 보조 버튼, 부수 −/+, 프린터 선택
				fill = dis ? Color(255, 248, 248, 249)
					 : pressed ? Color(255, 240, 240, 242)
					 : hot ? Color(255, 246, 246, 248) : Color(255, 255, 255, 255);
				border = (kind == BtnKind::Select && hot) ? Color(255, 160, 160, 168) : Color(255, 209, 209, 214);
				txt = dis ? Color(255, 160, 160, 166) : Color(255, 28, 28, 32);
				break;
			}
			if (fill.GetA())   { SolidBrush fb(fill); gfx.FillPath(&fb, &path); }
			if (border.GetA()) { Pen bp(border, 1.f); gfx.DrawPath(&bp, &path); }

			FontFamily ff(L"Segoe UI");
			FontFamily semi(L"Segoe UI Semibold");
			FontFamily icf(IconFontName());
			StringFormat sf;
			sf.SetAlignment(StringAlignmentCenter);
			sf.SetLineAlignment(StringAlignmentCenter);
			sf.SetFormatFlags(StringFormatFlagsNoWrap);
			sf.SetTrimming(StringTrimmingEllipsisCharacter);
			SolidBrush tb(txt);
			auto glyph = [&](const wchar_t* ch, const RectF& r, float px, Brush* b)
			{
				Font f(&icf, px, FontStyleRegular, UnitPixel);
				gfx.DrawString(ch, 1, &f, r, &sf, b);
			};

			wchar_t text[256] = {};
			GetWindowTextW(di->hwndItem, text, 256);
			if (kind == BtnKind::Nav)
				glyph(id == ID_PREV ? L"\xE76B" : L"\xE76C", RectF(0, 0, (float)w, (float)h), 12.f, &tb);  // ChevronLeft / Right
			else if (kind == BtnKind::Stepper)
				glyph(id == ID_COPY_DEC ? L"\xE738" : L"\xE710", RectF(0, 0, (float)w, (float)h), 11.f, &tb); // Remove / Add
			else if (kind == BtnKind::Select)
			{
				SolidBrush ib(Color(255, 96, 96, 104));
				glyph(L"\xE749", RectF(8, 0, 22, (float)h), 14.f, &ib);                    // Print
				glyph(L"\xE70D", RectF((float)w - 30, 0, 22, (float)h), 10.f, &ib);        // ChevronDown
				StringFormat sl;
				sl.SetLineAlignment(StringAlignmentCenter);
				sl.SetFormatFlags(StringFormatFlagsNoWrap);
				sl.SetTrimming(StringTrimmingEllipsisCharacter);
				Font f(&ff, 9.f, FontStyleRegular, UnitPoint);
				gfx.DrawString(text, -1, &f, RectF(34, 0, (float)w - 66, (float)h), &sl, &tb);
			}
			else
			{
				const bool useSemi = (kind == BtnKind::Segment && on) && semi.IsAvailable();
				Font f(useSemi ? &semi : &ff, 9.f, FontStyleRegular, UnitPoint);
				gfx.DrawString(text, -1, &f, RectF(4, 0, (float)w - 8, (float)h), &sf, &tb);
			}

			// 키보드 포커스 링 (Windows 11처럼 짙은 링)
			if (focus)
			{
				GraphicsPath fp;
				RoundPath(fp, 1.5f, 1.5f, (float)w - 3.f, (float)h - 3.f, std::max(1.f, rad - 1.f));
				Pen fpen(Color(255, 28, 28, 32), 2.f);
				gfx.DrawPath(&fpen, &fp);
			}
		}
		BitBlt(di->hDC, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
		SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
	}

	void ShowProps()
	{
		State* s = g;
		if (s->sel < 0 || s->devmode.size() < sizeof(DEVMODEW)) return;
		const std::wstring& name = s->printers[s->sel];
		HANDLE hp = nullptr;
		if (!OpenPrinterW(const_cast<LPWSTR>(name.c_str()), &hp, nullptr)) return;
		DEVMODEW* d = reinterpret_cast<DEVMODEW*>(s->devmode.data());
		DocumentPropertiesW(s->hDlg, hp, const_cast<LPWSTR>(name.c_str()), d, d,
							DM_IN_BUFFER | DM_IN_PROMPT | DM_OUT_BUFFER);
		ClosePrinter(hp);
		OnChanged();
	}

	void DoOk()
	{
		State* s = g;
		if (s->sel < 0) return;
		int f, t; GetRange(f, t);
		int copies = std::clamp(ReadInt(s->hEdCopies, 1), 1, 999);

		std::vector<BYTE> dm = BuildDevMode();
		HDC dc = CreateDCW(L"WINSPOOL", s->printers[s->sel].c_str(), nullptr,
						   dm.empty() ? nullptr : reinterpret_cast<DEVMODEW*>(dm.data()));
		if (!dc)
		{
			MessageBoxW(s->hDlg, T(L"\uD504\uB9B0\uD130\uB97C \uC5F4 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4.", L"Cannot open the printer."),
						T(L"\uC778\uC1C4", L"Print"), MB_OK | MB_ICONERROR);
			return;
		}
		s->out->hDC = dc;
		s->out->fromPage = f;
		s->out->toPage = t;
		s->out->copies = copies;
		s->out->fitToPage = Checked(s->hRadFit);
		s->out->poster = Checked(s->hRadPoster);
		s->out->posterCols = std::clamp(ReadInt(s->hEdCols, 2), 1, 10);
		s->out->posterRows = std::clamp(ReadInt(s->hEdRows, 2), 1, 10);
		s->out->twoUp = TwoUp();
		s->ok = true;
		s->done = true;
	}

	LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		switch (m)
		{
		case WM_ERASEBKGND: return 1;
		case WM_PAINT:
		{
			PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
			RECT rc; GetClientRect(h, &rc);
			HDC mem = CreateCompatibleDC(hdc);
			HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
			HGDIOBJ old = SelectObject(mem, bm);
			if (g) PaintPreview(mem, rc);
			BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
			SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
			EndPaint(h, &ps);
			return 0;
		}
		}
		return DefWindowProcW(h, m, w, l);
	}

	BOOL CALLBACK SetFontProc(HWND c, LPARAM l)
	{
		SendMessageW(c, WM_SETFONT, (WPARAM)l, TRUE);
		return TRUE;
	}

	// 입력 상자 테두리 영역 (부모 창이 그린다) — 상자보다 3px 바깥
	RECT EditFrame(HWND ed)
	{
		RECT r; GetWindowRect(ed, &r);
		MapWindowPoints(nullptr, g->hDlg, reinterpret_cast<POINT*>(&r), 2);
		InflateRect(&r, 3, 3);
		return r;
	}

	bool IsEditId(int id)
	{
		return id == ID_COPIES || id == ID_ED_FROM || id == ID_ED_TO || id == ID_ED_COLS || id == ID_ED_ROWS;
	}

	void PickPrinter()
	{
		State* s = g;
		HMENU menu = CreatePopupMenu();
		for (size_t i = 0; i < s->printers.size(); ++i)
			AppendMenuW(menu, MF_STRING | (i == (size_t)s->sel ? MF_CHECKED : 0), 1000 + (UINT)i, s->printers[i].c_str());
		RECT r; GetWindowRect(s->hCombo, &r);
		const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
											r.left, r.bottom + 2, 0, s->hDlg, nullptr);
		DestroyMenu(menu);
		const int i = cmd - 1000;
		if (cmd >= 1000 && i < (int)s->printers.size() && i != s->sel)
		{
			s->sel = i;
			SetWindowTextW(s->hCombo, s->printers[i].c_str());
			InvalidateRect(s->hCombo, nullptr, FALSE);
			LoadDevMode(s->printers[i], s->devmode);
			OnChanged();
		}
	}

	LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		State* s = g;
		switch (m)
		{
		case WM_CTLCOLOREDIT:
		{
			if (!s) break;
			HDC dc = (HDC)w;
			SetBkColor(dc, RGB(255, 255, 255));
			SetTextColor(dc, RGB(28, 28, 32));
			return (LRESULT)s->brWhite;
		}
		case WM_CTLCOLORSTATIC:
		{
			// 꺼진 입력 상자는 STATIC 색을 묻는다 → 옅은 회색 바탕
			if (!s) break;
			HDC dc = (HDC)w;
			SetBkColor(dc, kEditOff);
			SetTextColor(dc, RGB(150, 150, 156));
			return (LRESULT)s->brDisabled;
		}
		case WM_CTLCOLORBTN:
		{
			if (!s) break;
			SetBkColor((HDC)w, RGB(255, 255, 255));
			return (LRESULT)s->brWhite;
		}
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT:
		{
			PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
			RECT rc; GetClientRect(h, &rc);
			HDC mem = CreateCompatibleDC(hdc);
			HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
			HGDIOBJ old = SelectObject(mem, bm);
			{
				using namespace Gdiplus;
				Graphics gfx(mem);
				gfx.SetSmoothingMode(SmoothingModeAntiAlias);
				gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
				gfx.Clear(ToColor(kWinBg));

				// 설정 카드
				GraphicsPath card;
				RoundPath(card, 16.5f, 12.5f, 355.f, (float)rc.bottom - 25.f, 8.f);
				SolidBrush white(Color(255, 255, 255, 255));
				gfx.FillPath(&white, &card);
				Pen border(Color(255, 229, 229, 234), 1.f);
				gfx.DrawPath(&border, &card);

				FontFamily ff(L"Segoe UI");
				FontFamily semi(L"Segoe UI Semibold");
				Font secFont(semi.IsAvailable() ? &semi : &ff, 9.f,
							 semi.IsAvailable() ? FontStyleRegular : FontStyleBold, UnitPoint);
				Font txtFont(&ff, 9.f, FontStyleRegular, UnitPoint);
				SolidBrush secBr(Color(255, 92, 92, 100));
				SolidBrush txtBr(Color(255, 60, 60, 66));
				auto sec = [&](const wchar_t* t, float y) { gfx.DrawString(t, -1, &secFont, PointF(32.f, y), &secBr); };
				sec(T(L"프린터", L"Printer"), 24.f);
				sec(T(L"부수", L"Copies"), 128.f);
				sec(T(L"페이지 범위", L"Page range"), 194.f);
				sec(T(L"방향", L"Orientation"), 298.f);
				sec(T(L"배율", L"Scale"), 364.f);

				StringFormat sfc;
				sfc.SetAlignment(StringAlignmentCenter);
				sfc.SetLineAlignment(StringAlignmentCenter);
				gfx.DrawString(L"~", -1, &txtFont, RectF(122.f, 256.f, 26.f, 30.f), &sfc, &txtBr);
				gfx.DrawString(L"×", -1, &txtFont, RectF(102.f, 462.f, 24.f, 30.f), &sfc, &txtBr);

				if (s)
				{
					// 입력 상자 테두리: 둥근 테두리 + 포커스 시 아래쪽 강조선 (Windows 11 입력 상자)
					for (HWND ed : { s->hEdCopies, s->hEdFrom, s->hEdTo, s->hEdCols, s->hEdRows })
					{
						if (!ed) continue;
						const RECT fr = EditFrame(ed);
						const bool en = IsWindowEnabled(ed) != FALSE;
						const bool foc = GetFocus() == ed;
						GraphicsPath ep;
						RoundPath(ep, fr.left + 0.5f, fr.top + 0.5f, (float)(fr.right - fr.left) - 1.f,
								  (float)(fr.bottom - fr.top) - 1.f, 5.f);
						SolidBrush eb(en ? Color(255, 255, 255, 255) : ToColor(kEditOff));
						gfx.FillPath(&eb, &ep);
						Pen epen(en ? Color(255, 209, 209, 214) : Color(255, 229, 229, 233), 1.f);
						gfx.DrawPath(&epen, &ep);
						if (foc)
						{
							Pen ap(kAccent, 2.f);
							gfx.DrawLine(&ap, fr.left + 4.f, fr.bottom - 1.5f, fr.right - 4.f, fr.bottom - 1.5f);
						}
					}

					wchar_t buf[32];
					int f, t; GetRange(f, t);
					if (TwoUp() && s->previewPage + 1 <= t)   // 이 장의 두 페이지 번호
						swprintf_s(buf, L"%d–%d / %d", s->previewPage + 1, s->previewPage + 2, s->pageCount);
					else
						swprintf_s(buf, L"%d / %d", s->previewPage + 1, s->pageCount);
					gfx.DrawString(buf, -1, &txtFont,
						RectF((float)NAV_LABEL_RC.left, (float)NAV_LABEL_RC.top,
							  (float)(NAV_LABEL_RC.right - NAV_LABEL_RC.left),
							  (float)(NAV_LABEL_RC.bottom - NAV_LABEL_RC.top)), &sfc, &txtBr);
				}
			}
			BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
			SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
			EndPaint(h, &ps);
			return 0;
		}
		case WM_DRAWITEM:
			DrawBtn(reinterpret_cast<DRAWITEMSTRUCT*>(l));
			return TRUE;
		case WM_TIMER:
		{
			if (!s) break;
			POINT pt; GetCursorPos(&pt);
			HWND hw = WindowFromPoint(pt);
			int id = 0;
			if (hw && GetParent(hw) == h && hw != s->hPreview)
			{
				wchar_t cls[16] = {};
				GetClassNameW(hw, cls, 16);
				if (_wcsicmp(cls, L"Button") == 0) id = GetDlgCtrlID(hw);
			}
			if (id != s->hotId)
			{
				int old = s->hotId; s->hotId = id;
				if (old) InvalidateRect(GetDlgItem(h, old), nullptr, FALSE);
				if (id)  InvalidateRect(GetDlgItem(h, id), nullptr, FALSE);
			}
			return 0;
		}
		case WM_COMMAND:
		{
			if (!s) break;
			int id = LOWORD(w), code = HIWORD(w);
			if (IsEditId(id) && (code == EN_SETFOCUS || code == EN_KILLFOCUS))
			{
				RECT fr = EditFrame((HWND)l);
				InvalidateRect(h, &fr, FALSE);
				return 0;
			}
			if ((id == ID_ED_COLS || id == ID_ED_ROWS) && code == EN_CHANGE) { OnChanged(); return 0; }
			if ((id == ID_ED_FROM || id == ID_ED_TO) && code == EN_CHANGE) { OnChanged(); return 0; }
			if (code != BN_CLICKED) break;
			if (IsRadioId(id)) SelectRadio(id);
			switch (id)
			{
			case IDOK:     DoOk(); return 0;
			case IDCANCEL: s->done = true; return 0;
			case ID_PROPS: ShowProps(); return 0;
			case ID_COMBO: PickPrinter(); return 0;
			case ID_COPY_DEC:
			case ID_COPY_INC:
			{
				const int n = std::clamp(ReadInt(s->hEdCopies, 1) + (id == ID_COPY_INC ? 1 : -1), 1, 999);
				wchar_t b[8]; swprintf_s(b, L"%d", n);
				SetWindowTextW(s->hEdCopies, b);
				return 0;
			}
			case ID_PREV:
			case ID_NEXT:
			{
				int f, t; GetRange(f, t);
				const int step = TwoUp() ? 2 : 1; // 2쪽 모아찍기는 한 장(두 페이지)씩 넘긴다
				s->previewPage = std::clamp(s->previewPage + (id == ID_NEXT ? step : -step), f, t);
				if (TwoUp()) s->previewPage = SheetStart(s->previewPage, f);
				UpdateNav();
				InvalidateRect(s->hPreview, nullptr, FALSE);
				return 0;
			}
			case ID_RAD_ALL:
			case ID_RAD_CUR:
			case ID_RAD_RNG:
				EnableWindow(s->hEdFrom, Checked(s->hRadRng));
				EnableWindow(s->hEdTo, Checked(s->hRadRng));
				if (id == ID_RAD_CUR) s->previewPage = s->curPage;
				InvalidateRect(h, nullptr, FALSE);   // 입력 상자 테두리 색
				OnChanged();
				return 0;
			case ID_RAD_AUTO: case ID_RAD_PORT: case ID_RAD_LAND:
			case ID_RAD_FIT:  case ID_RAD_ACT: case ID_RAD_POSTER: case ID_RAD_TWOUP:
				EnableWindow(s->hEdCols, Checked(s->hRadPoster));
				EnableWindow(s->hEdRows, Checked(s->hRadPoster));
				InvalidateRect(h, nullptr, FALSE);
				OnChanged();
				return 0;
			}
			break;
		}
		case WM_CLOSE:
			if (s) s->done = true;
			return 0;
		}
		return DefWindowProcW(h, m, w, l);
	}
}

// 정보 창과 같은 페이드 효과 (같은 시간, 같은 부드러운 가감속).
// 투명도만 바꾸므로 메시지를 처리하지 않아도 DWM이 화면에 반영한다.
static void FadeWindow(HWND h, bool fadeIn, int durationMs)
{
	const ULONGLONG start = GetTickCount64();
	for (;;)
	{
		const int elapsed = (int)(GetTickCount64() - start);
		float t = std::min(1.0f, (float)elapsed / durationMs);
		t = t * t * (3.0f - 2.0f * t);
		SetLayeredWindowAttributes(h, 0, (BYTE)(255 * (fadeIn ? t : 1.0f - t)), LWA_ALPHA);
		if (elapsed >= durationMs) break;
		Sleep(16);
	}
}

bool ShowPrintDialog(HWND parent, FPDF_DOCUMENT doc, int pageCount, int currentPage,
					 const std::vector<int>& rotations, std::mutex& pdfiumMtx, PrintDialogResult& out)
{
	if (!doc || pageCount <= 0) return false;

	// 프린터 목록
	State s;
	{
		DWORD needed = 0, count = 0;
		const DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
		EnumPrintersW(flags, nullptr, 4, nullptr, 0, &needed, &count);
		if (needed > 0)
		{
			std::vector<BYTE> buf(needed);
			if (EnumPrintersW(flags, nullptr, 4, buf.data(), needed, &needed, &count))
			{
				auto* info = reinterpret_cast<PRINTER_INFO_4W*>(buf.data());
				for (DWORD i = 0; i < count; ++i)
					if (info[i].pPrinterName) s.printers.push_back(info[i].pPrinterName);
			}
		}
	}
	if (s.printers.empty())
	{
		MessageBoxW(parent, T(L"\uC0AC\uC6A9 \uAC00\uB2A5\uD55C \uD504\uB9B0\uD130\uAC00 \uC5C6\uC2B5\uB2C8\uB2E4.", L"No printers are available."),
					T(L"\uC778\uC1C4", L"Print"), MB_OK | MB_ICONINFORMATION);
		return false;
	}

	ULONG_PTR gdipToken = 0;
	Gdiplus::GdiplusStartupInput gsi{};
	Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr);

	static bool registered = false;
	if (!registered)
	{
		WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.lpfnWndProc = DlgProc; wc.lpszClassName = PD_CLASS;
		RegisterClassExW(&wc);
		wc.lpfnWndProc = PreviewProc; wc.lpszClassName = PV_CLASS;
		RegisterClassExW(&wc);
		registered = true;
	}

	s.doc = doc; s.pageCount = pageCount; s.rotations = rotations; s.mtx = &pdfiumMtx;
	s.curPage = std::clamp(currentPage, 0, pageCount - 1);
	s.previewPage = s.curPage;
	s.out = &out;
	s.font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
						 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
						 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
	s.brWhite = CreateSolidBrush(RGB(255, 255, 255));
	s.brDisabled = CreateSolidBrush(kEditOff);
	g = &s;

	// 기본 프린터 선택
	{
		wchar_t def[256] = {}; DWORD len = 256;
		s.sel = 0;
		if (GetDefaultPrinterW(def, &len))
			for (size_t i = 0; i < s.printers.size(); ++i)
				if (s.printers[i] == def) { s.sel = (int)i; break; }
		LoadDevMode(s.printers[s.sel], s.devmode);
	}

	RECT wr = { 0, 0, DLG_CW, DLG_CH };
	const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
	const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_LAYERED; // 페이드 인/아웃용
	AdjustWindowRectEx(&wr, style, FALSE, exStyle);
	int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
	int x = 100, y = 100;
	RECT pr;
	if (parent && GetWindowRect(parent, &pr))
	{
		x = pr.left + ((pr.right - pr.left) - ww) / 2;
		y = pr.top + ((pr.bottom - pr.top) - wh) / 2;
	}

	HWND h = CreateWindowExW(exStyle, PD_CLASS, T(L"\uC778\uC1C4", L"Print"), style,
							 x, y, ww, wh, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
	s.hDlg = h;

	auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD st, DWORD ex,
				  int cx, int cy, int cw, int ch, int id) -> HWND
	{
		return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | st, cx, cy, cw, ch,
							   h, (HMENU)(INT_PTR)id, nullptr, nullptr);
	};

	// 입력 상자: 테두리 없이 만들고 부모가 둥근 테두리를 3px 바깥에 그린다 (fx..fh = 테두리 영역)
	auto mkEdit = [&](const wchar_t* text, DWORD st, int fx, int fy, int fw, int fh, int id) -> HWND
	{
		return mk(L"EDIT", text, WS_TABSTOP | ES_NUMBER | ES_CENTER | ES_MULTILINE | st, 0,
				  fx + 3, fy + 3, fw - 6, fh - 6, id);
	};
	auto mkBtn = [&](const wchar_t* text, DWORD st, int bx, int by, int bw, int bh, int id) -> HWND
	{
		return mk(L"BUTTON", text, BS_OWNERDRAW | st, 0, bx, by, bw, bh, id);
	};

	// 프린터: 누르면 목록 메뉴가 뜨는 선택 상자
	s.hCombo = mkBtn(s.printers[s.sel].c_str(), WS_TABSTOP, 32, 46, 324, 34, ID_COMBO);
	s.hProps = mkBtn(T(L"프린터 속성", L"Properties"), WS_TABSTOP, 32, 88, 140, 28, ID_PROPS);

	// 부수: [−] [ n ] [+]
	s.hCopyDec  = mkBtn(L"", WS_GROUP | WS_TABSTOP, 32, 150, 32, 32, ID_COPY_DEC);
	s.hEdCopies = mkEdit(L"1", 0, 68, 150, 60, 32, ID_COPIES);
	s.hCopyInc  = mkBtn(L"", WS_TABSTOP, 132, 150, 32, 32, ID_COPY_INC);
	SendMessageW(s.hEdCopies, EM_LIMITTEXT, 3, 0);

	// 페이지 범위: 세 칸 선택지 + 시작 ~ 끝
	s.hRadAll = mkBtn(T(L"전체", L"All"), WS_GROUP | WS_TABSTOP, 32, 216, 105, 32, ID_RAD_ALL);
	s.hRadCur = mkBtn(T(L"현재 페이지", L"Current"), 0, 141, 216, 105, 32, ID_RAD_CUR);
	s.hRadRng = mkBtn(T(L"지정", L"Pages"), 0, 250, 216, 106, 32, ID_RAD_RNG);
	s.hEdFrom = mkEdit(L"1", WS_GROUP, 32, 256, 90, 30, ID_ED_FROM);
	wchar_t last[16]; swprintf_s(last, L"%d", pageCount);
	s.hEdTo = mkEdit(last, 0, 148, 256, 90, 30, ID_ED_TO);
	SendMessageW(s.hEdFrom, EM_LIMITTEXT, 6, 0);
	SendMessageW(s.hEdTo, EM_LIMITTEXT, 6, 0);
	EnableWindow(s.hEdFrom, FALSE);
	EnableWindow(s.hEdTo, FALSE);

	// 방향
	s.hRadAuto = mkBtn(T(L"자동", L"Auto"), WS_GROUP | WS_TABSTOP, 32, 320, 105, 32, ID_RAD_AUTO);
	s.hRadPort = mkBtn(T(L"세로", L"Portrait"), 0, 141, 320, 105, 32, ID_RAD_PORT);
	s.hRadLand = mkBtn(T(L"가로", L"Landscape"), 0, 250, 320, 106, 32, ID_RAD_LAND);

	// 배율: 2×2 선택지 + 포스터 열 × 행
	s.hRadFit    = mkBtn(T(L"용지에 맞춤", L"Fit to page"), WS_GROUP | WS_TABSTOP, 32, 386, 160, 32, ID_RAD_FIT);
	s.hRadAct    = mkBtn(T(L"실제 크기", L"Actual size"), 0, 196, 386, 160, 32, ID_RAD_ACT);
	s.hRadPoster = mkBtn(T(L"포스터 분할", L"Poster tiles"), 0, 32, 422, 160, 32, ID_RAD_POSTER);
	s.hRadTwoUp  = mkBtn(T(L"2쪽 모아찍기", L"2 pages per sheet"), 0, 196, 422, 160, 32, ID_RAD_TWOUP);
	s.hEdCols = mkEdit(L"2", WS_GROUP, 32, 462, 70, 30, ID_ED_COLS);
	s.hEdRows = mkEdit(L"2", 0, 126, 462, 70, 30, ID_ED_ROWS);
	SendMessageW(s.hEdCols, EM_LIMITTEXT, 2, 0);
	SendMessageW(s.hEdRows, EM_LIMITTEXT, 2, 0);
	EnableWindow(s.hEdCols, FALSE);
	EnableWindow(s.hEdRows, FALSE);

	SetCheck(s.hRadAll, true);
	SetCheck(s.hRadAuto, true);
	SetCheck(s.hRadFit, true);

	s.hCancel = mkBtn(T(L"취소", L"Cancel"), WS_GROUP | WS_TABSTOP, 146, 530, 100, 36, IDCANCEL);
	s.hPrint  = mkBtn(T(L"인쇄", L"Print"), WS_TABSTOP, 256, 530, 100, 36, IDOK);

	s.hPreview = mk(PV_CLASS, L"", 0, 0, 388, 12, 496, 506, 0);
	s.hPrev = mkBtn(L"", WS_TABSTOP, 546, 530, 36, 32, ID_PREV);
	s.hNext = mkBtn(L"", WS_TABSTOP, 694, 530, 36, 32, ID_NEXT);

	EnumChildWindows(h, SetFontProc, (LPARAM)s.font);

	{
		HDC tdc = GetDC(h);
		HGDIOBJ oldF = SelectObject(tdc, s.font);
		TEXTMETRICW tm{}; GetTextMetricsW(tdc, &tm);
		SelectObject(tdc, oldF); ReleaseDC(h, tdc);
		for (HWND ed : { s.hEdCopies, s.hEdFrom, s.hEdTo, s.hEdCols, s.hEdRows })
		{
			RECT er; GetClientRect(ed, &er);
			int top = std::max(0, (int)(er.bottom - tm.tmHeight) / 2);
			RECT fr = { 2, top, er.right - 2, er.bottom };
			SendMessageW(ed, EM_SETRECTNP, 0, (LPARAM)&fr);
		}
	}

	s.ready = true;
	OnChanged();
	SetTimer(h, 1, 60, nullptr);

	if (parent) EnableWindow(parent, FALSE);
	// 투명하게 띄운 뒤 내용을 모두 그려 두고 서서히 나타낸다
	SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
	ShowWindow(h, SW_SHOW);
	SetForegroundWindow(h);
	RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
	FadeWindow(h, true, 220);

	MSG msg;
	while (!s.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
	{
		// 포커스 링은 키보드로 옮겨 다닐 때만 보인다 (마우스로 누르면 숨김)
		if (msg.message == WM_KEYDOWN && (msg.wParam == VK_TAB || (msg.wParam >= VK_LEFT && msg.wParam <= VK_DOWN)))
		{
			if (!s.kbdFocus) { s.kbdFocus = true; if (HWND f = GetFocus()) InvalidateRect(f, nullptr, FALSE); }
		}
		else if (msg.message == WM_LBUTTONDOWN && s.kbdFocus)
		{
			s.kbdFocus = false;
			if (HWND f = GetFocus()) InvalidateRect(f, nullptr, FALSE);
		}
		if (!IsDialogMessageW(h, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	KillTimer(h, 1);
	if (IsWindow(h)) FadeWindow(h, false, 300); // 인쇄·취소·닫기 모두 서서히 사라진다
	if (parent) EnableWindow(parent, TRUE);
	DestroyWindow(h);
	if (parent) SetForegroundWindow(parent);

	FreeCache();
	DeleteObject(s.font);
	DeleteObject(s.brWhite);
	DeleteObject(s.brDisabled);
	Gdiplus::GdiplusShutdown(gdipToken);
	g = nullptr;
	return s.ok;
}
