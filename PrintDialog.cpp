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
		ID_RAD_FIT, ID_RAD_ACT, ID_PREV, ID_NEXT
	};

	constexpr int DLG_CW = 900;
	constexpr int DLG_CH = 560;
	const RECT NAV_LABEL_RC = { 586, 500, 690, 532 };

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
		HWND hPrev = nullptr, hNext = nullptr, hPrint = nullptr, hCancel = nullptr;

		FPDF_DOCUMENT doc = nullptr;
		int  pageCount = 0, curPage = 0, rotation = 0;
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
		HFONT  font = nullptr;
		HBRUSH brWhite = nullptr;
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

	bool Checked(HWND h) { return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

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

	bool PageSize(int idx, double& w, double& h)
	{
		State* s = g;
		{
			std::lock_guard<std::mutex> lk(*s->mtx);
			if (FPDF_GetPageSizeByIndex(s->doc, idx, &w, &h) == 0) return false;
		}
		if (s->rotation == 1 || s->rotation == 3) std::swap(w, h);
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
			o = (PageSize(f, w, h) && w > h) ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
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
	}

	bool EnsureCache(int page, int w, int h)
	{
		State* s = g;
		if (s->cache && s->cPage == page && s->cW == w && s->cH == h && s->cRot == s->rotation) return true;
		FreeCache();

		std::lock_guard<std::mutex> lk(*s->mtx);
		FPDF_PAGE pg = FPDF_LoadPage(s->doc, page);
		if (!pg) return false;
		FPDF_BITMAP b = FPDFBitmap_Create(w, h, 0);
		if (b)
		{
			FPDFBitmap_FillRect(b, 0, 0, w, h, 0xFFFFFFFF);
			FPDF_RenderPageBitmap(b, pg, 0, 0, w, h, s->rotation, FPDF_ANNOT | FPDF_LCD_TEXT);
		}
		FPDF_ClosePage(pg);
		if (!b) return false;
		s->cache = b; s->cPage = page; s->cW = w; s->cH = h; s->cRot = s->rotation;
		return true;
	}

	void PaintPreview(HDC dc, const RECT& rc)
	{
		State* s = g;
		HBRUSH bg = CreateSolidBrush(RGB(236, 238, 242));
		FillRect(dc, &rc, bg);
		DeleteObject(bg);

		const Paper& p = s->paper;
		const int M = 18;
		double aw = rc.right - 2.0 * M, ah = rc.bottom - 2.0 * M;
		if (aw < 10 || ah < 10) return;
		double k = std::min(aw / p.physW, ah / p.physH);
		int pw = std::max(1, (int)(p.physW * k)), ph = std::max(1, (int)(p.physH * k));
		int px = (rc.right - pw) / 2, py = (rc.bottom - ph) / 2;

		RECT sh = { px + 3, py + 3, px + pw + 3, py + ph + 3 };
		HBRUSH bs = CreateSolidBrush(RGB(205, 208, 214)); FillRect(dc, &sh, bs); DeleteObject(bs);
		RECT paper = { px, py, px + pw, py + ph };
		HBRUSH bw = CreateSolidBrush(RGB(255, 255, 255)); FillRect(dc, &paper, bw); DeleteObject(bw);
		HBRUSH bb = CreateSolidBrush(RGB(190, 193, 200)); FrameRect(dc, &paper, bb); DeleteObject(bb);

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
		EnableWindow(s->hPrev, s->previewPage > f);
		EnableWindow(s->hNext, s->previewPage < t);
		InvalidateRect(s->hDlg, &NAV_LABEL_RC, FALSE);
	}

	void OnChanged()
	{
		State* s = g;
		if (!s->ready) return;
		int f, t; GetRange(f, t);
		s->previewPage = std::clamp(s->previewPage, f, t);
		UpdatePaper();
		UpdateNav();
		InvalidateRect(s->hPreview, nullptr, FALSE);
	}

	void RoundPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r)
	{
		p.AddArc(x, y, r * 2, r * 2, 180.f, 90.f);
		p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270.f, 90.f);
		p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0.f, 90.f);
		p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90.f, 90.f);
		p.CloseFigure();
	}

	void DrawBtn(DRAWITEMSTRUCT* di)
	{
		State* s = g;
		RECT rc = di->rcItem;
		int w = rc.right - rc.left, h = rc.bottom - rc.top;
		HDC mem = CreateCompatibleDC(di->hDC);
		HBITMAP bm = CreateCompatibleBitmap(di->hDC, w, h);
		HGDIOBJ old = SelectObject(mem, bm);
		{
			Gdiplus::Graphics gfx(mem);
			gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
			gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

			const int id = (int)di->CtlID;
			const bool pressed = (di->itemState & ODS_SELECTED) != 0;
			const bool dis = (di->itemState & ODS_DISABLED) != 0;
			const bool hot = !dis && s->hotId == id;
			const bool nav = (id == ID_PREV || id == ID_NEXT);
			const bool primary = (id == IDOK);

			gfx.Clear(nav ? Gdiplus::Color(255, 247, 248, 250) : Gdiplus::Color(255, 255, 255, 255));

			Gdiplus::GraphicsPath path;
			RoundPath(path, 0.5f, 0.5f, (float)w - 1.f, (float)h - 1.f, 6.f);

			Gdiplus::Color fill, txt, gTop, gBot;
			if (primary)
			{
				gTop = hot ? Gdiplus::Color(255, 14, 148, 244) : Gdiplus::Color(255, 0, 137, 230);
				gBot = hot ? Gdiplus::Color(255, 0, 100, 185) : Gdiplus::Color(255, 0, 108, 196);
				if (pressed) { gTop = Gdiplus::Color(255, 0, 100, 185); gBot = Gdiplus::Color(255, 0, 84, 153); }
				txt = Gdiplus::Color(255, 255, 255, 255);
			}
			else
			{
				gTop = hot ? Gdiplus::Color(255, 245, 245, 245) : Gdiplus::Color(255, 255, 255, 255);
				gBot = hot ? Gdiplus::Color(255, 230, 230, 230) : Gdiplus::Color(255, 242, 242, 242);
				if (pressed) { gTop = Gdiplus::Color(255, 230, 230, 230); gBot = Gdiplus::Color(255, 218, 218, 218); }
				txt = dis ? Gdiplus::Color(255, 175, 177, 182) : Gdiplus::Color(255, 45, 45, 45);
			}
			Gdiplus::LinearGradientBrush fb(Gdiplus::PointF(0.f, 0.f), Gdiplus::PointF(0.f, (float)h), gTop, gBot);
			gfx.FillPath(&fb, &path);
			if (!primary)
			{
				Gdiplus::Pen bp(Gdiplus::Color(255, 200, 200, 200), 1.f);
				gfx.DrawPath(&bp, &path);
			}

			if (nav)
			{
				Gdiplus::Pen ap(txt, 2.f);
				float cx = w / 2.f, cy = h / 2.f;
				if (id == ID_PREV)
				{
					gfx.DrawLine(&ap, cx + 3.f, cy - 6.f, cx - 3.f, cy);
					gfx.DrawLine(&ap, cx - 3.f, cy, cx + 3.f, cy + 6.f);
				}
				else
				{
					gfx.DrawLine(&ap, cx - 3.f, cy - 6.f, cx + 3.f, cy);
					gfx.DrawLine(&ap, cx + 3.f, cy, cx - 3.f, cy + 6.f);
				}
			}
			else
			{
				wchar_t text[64] = {};
				GetWindowTextW(di->hwndItem, text, 64);
				Gdiplus::FontFamily ff(L"Segoe UI");
				Gdiplus::Font font(&ff, 9.f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
				Gdiplus::SolidBrush tb(txt);
				Gdiplus::StringFormat sf;
				sf.SetAlignment(Gdiplus::StringAlignmentCenter);
				sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
				gfx.DrawString(text, -1, &font, Gdiplus::RectF(0, 0, (float)w, (float)h), &sf, &tb);
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

	LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		State* s = g;
		switch (m)
		{
		case WM_CTLCOLORSTATIC:
		case WM_CTLCOLORBTN:
		{
			if (!s) break;
			HDC dc = (HDC)w;
			SetBkColor(dc, RGB(255, 255, 255));
			SetTextColor(dc, IsWindowEnabled((HWND)l) ? RGB(45, 48, 55) : RGB(150, 152, 158));
			return (LRESULT)s->brWhite;
		}
		case WM_ERASEBKGND:
		{
			// 자식 컨트롤(테마 라디오 버튼)이 부모 배경을 요청하면 흰색 카드로 채운다
			HDC dc = (HDC)w;
			if (WindowFromDC(dc) == h) return 1;
			RECT r; GetClipBox(dc, &r);
			HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
			FillRect(dc, &r, b);
			DeleteObject(b);
			return 1;
		}
		case WM_PAINT:
		{
			PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
			RECT rc; GetClientRect(h, &rc);
			HDC mem = CreateCompatibleDC(hdc);
			HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
			HGDIOBJ old = SelectObject(mem, bm);
			{
				Gdiplus::Graphics gfx(mem);
				gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
				gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
				gfx.Clear(Gdiplus::Color(255, 247, 248, 250));

				Gdiplus::GraphicsPath card;
				RoundPath(card, 16.5f, 12.5f, 355.f, (float)rc.bottom - 25.f, 10.f);
				Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
				gfx.FillPath(&white, &card);
				Gdiplus::Pen border(Gdiplus::Color(255, 218, 220, 226), 1.f);
				gfx.DrawPath(&border, &card);

				Gdiplus::FontFamily ff(L"Segoe UI");
				Gdiplus::Font secFont(&ff, 9.f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
				Gdiplus::Font txtFont(&ff, 9.f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
				Gdiplus::SolidBrush secBr(Gdiplus::Color(255, 60, 120, 210));
				Gdiplus::SolidBrush txtBr(Gdiplus::Color(255, 45, 48, 55));
				auto sec = [&](const wchar_t* t, float y) { gfx.DrawString(t, -1, &secFont, Gdiplus::PointF(32.f, y), &secBr); };
				sec(T(L"\uD504\uB9B0\uD130", L"Printer"), 28.f);
				sec(T(L"\uBD80\uC218", L"Copies"), 134.f);
				sec(T(L"\uD398\uC774\uC9C0 \uBC94\uC704", L"Page range"), 198.f);
				sec(T(L"\uBC29\uD5A5", L"Orientation"), 292.f);
				sec(T(L"\uBC30\uC728", L"Scale"), 356.f);
				gfx.DrawString(L"~", -1, &txtFont, Gdiplus::PointF(132.f, 253.f), &txtBr);

				if (s)
				{
					wchar_t buf[32];
					swprintf_s(buf, L"%d / %d", s->previewPage + 1, s->pageCount);
					Gdiplus::StringFormat sf;
					sf.SetAlignment(Gdiplus::StringAlignmentCenter);
					sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
					gfx.DrawString(buf, -1, &txtFont,
						Gdiplus::RectF((float)NAV_LABEL_RC.left, (float)NAV_LABEL_RC.top,
									   (float)(NAV_LABEL_RC.right - NAV_LABEL_RC.left),
									   (float)(NAV_LABEL_RC.bottom - NAV_LABEL_RC.top)), &sf, &txtBr);
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
			if (hw == s->hPrint) id = IDOK;
			else if (hw == s->hCancel) id = IDCANCEL;
			else if (hw == s->hProps) id = ID_PROPS;
			else if (hw == s->hPrev) id = ID_PREV;
			else if (hw == s->hNext) id = ID_NEXT;
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
			if ((id == ID_ED_FROM || id == ID_ED_TO) && code == EN_CHANGE) { OnChanged(); return 0; }
			if (id == ID_COMBO && code == CBN_SELCHANGE)
			{
				int i = (int)SendMessageW(s->hCombo, CB_GETCURSEL, 0, 0);
				if (i >= 0 && i < (int)s->printers.size())
				{
					s->sel = i;
					LoadDevMode(s->printers[i], s->devmode);
					OnChanged();
				}
				return 0;
			}
			if (code != BN_CLICKED) break;
			switch (id)
			{
			case IDOK:     DoOk(); return 0;
			case IDCANCEL: s->done = true; return 0;
			case ID_PROPS: ShowProps(); return 0;
			case ID_PREV:
			case ID_NEXT:
			{
				int f, t; GetRange(f, t);
				s->previewPage = std::clamp(s->previewPage + (id == ID_NEXT ? 1 : -1), f, t);
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
				OnChanged();
				return 0;
			case ID_RAD_AUTO: case ID_RAD_PORT: case ID_RAD_LAND:
			case ID_RAD_FIT:  case ID_RAD_ACT:
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

bool ShowPrintDialog(HWND parent, FPDF_DOCUMENT doc, int pageCount, int currentPage,
					 int rotation, std::mutex& pdfiumMtx, PrintDialogResult& out)
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

	s.doc = doc; s.pageCount = pageCount; s.rotation = rotation; s.mtx = &pdfiumMtx;
	s.curPage = std::clamp(currentPage, 0, pageCount - 1);
	s.previewPage = s.curPage;
	s.out = &out;
	s.font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
						 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
						 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
	s.brWhite = CreateSolidBrush(RGB(255, 255, 255));
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
	const DWORD exStyle = WS_EX_DLGMODALFRAME;
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

	s.hCombo = mk(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 32, 50, 324, 260, ID_COMBO);
	for (const auto& n : s.printers) SendMessageW(s.hCombo, CB_ADDSTRING, 0, (LPARAM)n.c_str());
	SendMessageW(s.hCombo, CB_SETCURSEL, s.sel, 0);

	s.hProps = mk(L"BUTTON", T(L"\uD504\uB9B0\uD130 \uC18D\uC131", L"Properties"), WS_TABSTOP | BS_OWNERDRAW, 0, 32, 86, 130, 30, ID_PROPS);

	s.hEdCopies = mk(L"EDIT", L"1", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_CENTER | ES_MULTILINE, 0, 32, 156, 70, 26, ID_COPIES);
	SendMessageW(s.hEdCopies, EM_LIMITTEXT, 3, 0);

	s.hRadAll = mk(L"BUTTON", T(L"\uC804\uCCB4", L"All"), WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 32, 220, 80, 22, ID_RAD_ALL);
	s.hRadCur = mk(L"BUTTON", T(L"\uD604\uC7AC \uD398\uC774\uC9C0", L"Current"), BS_AUTORADIOBUTTON, 0, 116, 220, 120, 22, ID_RAD_CUR);
	s.hRadRng = mk(L"BUTTON", T(L"\uC9C0\uC815", L"Pages"), BS_AUTORADIOBUTTON, 0, 240, 220, 100, 22, ID_RAD_RNG);
	s.hEdFrom = mk(L"EDIT", L"1", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_CENTER | ES_MULTILINE, 0, 56, 250, 70, 26, ID_ED_FROM);
	wchar_t last[16]; swprintf_s(last, L"%d", pageCount);
	s.hEdTo = mk(L"EDIT", last, WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_CENTER | ES_MULTILINE, 0, 152, 250, 70, 26, ID_ED_TO);
	SendMessageW(s.hEdFrom, EM_LIMITTEXT, 6, 0);
	SendMessageW(s.hEdTo, EM_LIMITTEXT, 6, 0);
	EnableWindow(s.hEdFrom, FALSE);
	EnableWindow(s.hEdTo, FALSE);

	s.hRadAuto = mk(L"BUTTON", T(L"\uC790\uB3D9", L"Auto"), WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 32, 314, 80, 22, ID_RAD_AUTO);
	s.hRadPort = mk(L"BUTTON", T(L"\uC138\uB85C", L"Portrait"), BS_AUTORADIOBUTTON, 0, 116, 314, 80, 22, ID_RAD_PORT);
	s.hRadLand = mk(L"BUTTON", T(L"\uAC00\uB85C", L"Landscape"), BS_AUTORADIOBUTTON, 0, 200, 314, 90, 22, ID_RAD_LAND);

	s.hRadFit = mk(L"BUTTON", T(L"\uC6A9\uC9C0\uC5D0 \uB9DE\uCDA4", L"Fit to page"), WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 32, 378, 140, 22, ID_RAD_FIT);
	s.hRadAct = mk(L"BUTTON", T(L"\uC2E4\uC81C \uD06C\uAE30", L"Actual size"), BS_AUTORADIOBUTTON, 0, 176, 378, 120, 22, ID_RAD_ACT);

	SendMessageW(s.hRadAll, BM_SETCHECK, BST_CHECKED, 0);
	SendMessageW(s.hRadAuto, BM_SETCHECK, BST_CHECKED, 0);
	SendMessageW(s.hRadFit, BM_SETCHECK, BST_CHECKED, 0);

	s.hCancel = mk(L"BUTTON", T(L"\uCDE8\uC18C", L"Cancel"), WS_TABSTOP | BS_OWNERDRAW, 0, 146, 488, 100, 38, IDCANCEL);
	s.hPrint  = mk(L"BUTTON", T(L"\uC778\uC1C4", L"Print"), WS_TABSTOP | BS_OWNERDRAW, 0, 256, 488, 100, 38, IDOK);

	s.hPreview = mk(PV_CLASS, L"", 0, 0, 388, 12, 500, 476, 0);
	s.hPrev = mk(L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, 0, 638 - 92, 500, 36, 32, ID_PREV);
	s.hNext = mk(L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, 0, 638 + 56, 500, 36, 32, ID_NEXT);

	EnumChildWindows(h, SetFontProc, (LPARAM)s.font);

	{
		HDC tdc = GetDC(h);
		HGDIOBJ oldF = SelectObject(tdc, s.font);
		TEXTMETRICW tm{}; GetTextMetricsW(tdc, &tm);
		SelectObject(tdc, oldF); ReleaseDC(h, tdc);
		for (HWND ed : { s.hEdCopies, s.hEdFrom, s.hEdTo })
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
	ShowWindow(h, SW_SHOW);
	SetForegroundWindow(h);

	MSG msg;
	while (!s.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
	{
		if (!IsDialogMessageW(h, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	KillTimer(h, 1);
	if (parent) EnableWindow(parent, TRUE);
	DestroyWindow(h);
	if (parent) SetForegroundWindow(parent);

	FreeCache();
	DeleteObject(s.font);
	DeleteObject(s.brWhite);
	Gdiplus::GdiplusShutdown(gdipToken);
	g = nullptr;
	return s.ok;
}
