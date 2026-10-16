// ExportImage.cpp - PDFium 기반 페이지 이미지 내보내기 (PNG/JPG)
#define NOMINMAX
#include "framework.h"
#include "PlainPDFViewer.h"
#include "ExportImage.h"
#include "Lang.h"
#include "CleanPopup.h"
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

extern std::mutex g_pdfiumMtx;

namespace
{
	enum { ID_FROM = 2001, ID_TO = 2002, ID_DPI = 2003 };
	const int kDpis[] = { 72, 150, 300, 600 };

	struct DlgState { int from = 1, to = 1, dpiIdx = 1, pageCount = 1; };

	const wchar_t* L10N(const wchar_t* ko, const wchar_t* en, const wchar_t* ja, const wchar_t* zh,
						const wchar_t* es, const wchar_t* fr, const wchar_t* de, const wchar_t* pt, const wchar_t* ru)
	{
		return (::LS)(ko, en, ja, zh, es, fr, de, pt, ru);
	}

	const wchar_t* Title()
	{
		return L10N(L"이미지로 내보내기", L"Export as Image", L"画像として書き出し", L"导出为图像",
					L"Exportar como imagen", L"Exporter en image", L"Als Bild exportieren", L"Exportar como imagem", L"Экспорт в изображение");
	}

	// ── CleanPopup 스타일 대화상자 ──────────────────────────────────────────
	constexpr int DLG_W = 360, DLG_H = 290, BTN_W = 100, BTN_H = 34, BTN_GAP = 10;
	constexpr int ID_OK = 1, ID_CANCEL = 2;

	struct Ui
	{
		DlgState* st = nullptr;
		HWND wnd = nullptr, eFrom = nullptr, eTo = nullptr;
		HFONT font = nullptr;
		bool done = false, accepted = false;
		int hover = 0, pressed = 0;
	};
	Ui g_ui;

	RECT OkRect()     { int l = (DLG_W - (BTN_W * 2 + BTN_GAP)) / 2; int t = DLG_H - 20 - BTN_H; return { l, t, l + BTN_W, t + BTN_H }; }
	RECT CancelRect() { int l = (DLG_W - (BTN_W * 2 + BTN_GAP)) / 2 + BTN_W + BTN_GAP; int t = DLG_H - 20 - BTN_H; return { l, t, l + BTN_W, t + BTN_H }; }
	RECT DpiRect(int i) { int w = (DLG_W - 40 - 3 * 8) / 4; int l = 20 + i * (w + 8); return { l, 190, l + w, 190 + 30 }; }

	Gdiplus::GraphicsPath* RoundPath(const Gdiplus::RectF& r, float rad)
	{
		auto* p = new Gdiplus::GraphicsPath();
		float d = rad * 2.0f;
		p->AddArc(r.X, r.Y, d, d, 180, 90);
		p->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
		p->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
		p->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
		p->CloseFigure();
		return p;
	}

	void DrawBtn(Gdiplus::Graphics& g, const RECT& rc, bool primary, bool hot, bool selected, const wchar_t* label)
	{
		using namespace Gdiplus;
		FontFamily ff(L"Segoe UI");
		RectF rf((float)rc.left + 0.5f, (float)rc.top + 0.5f, (float)(rc.right - rc.left) - 1.0f, (float)(rc.bottom - rc.top) - 1.0f);
		GraphicsPath* path = RoundPath(rf, 5.0f);
		bool blue = primary || selected;
		if (blue)
		{
			Color top = hot ? Color(255, 14, 148, 244) : Color(255, 0, 137, 230);
			Color bot = hot ? Color(255, 0, 100, 185) : Color(255, 0, 108, 196);
			LinearGradientBrush grad(PointF(rf.X, rf.Y), PointF(rf.X, rf.Y + rf.Height), top, bot);
			g.FillPath(&grad, path);
			Pen hl(Color(55, 255, 255, 255), 1.0f);
			g.DrawLine(&hl, rf.X + 5, rf.Y + 1.5f, rf.X + rf.Width - 5, rf.Y + 1.5f);
		}
		else
		{
			Color top = hot ? Color(255, 252, 252, 252) : Color(255, 255, 255, 255);
			Color bot = hot ? Color(255, 230, 230, 230) : Color(255, 242, 242, 242);
			LinearGradientBrush grad(PointF(rf.X, rf.Y), PointF(rf.X, rf.Y + rf.Height), top, bot);
			g.FillPath(&grad, path);
			Pen bdr(Color(255, 200, 200, 200), 1.0f);
			g.DrawPath(&bdr, path);
		}
		delete path;
		SolidBrush tb(blue ? Color(255, 255, 255, 255) : Color(255, 45, 45, 45));
		Font f(&ff, 9, FontStyleBold, UnitPoint);
		StringFormat sf;
		sf.SetAlignment(StringAlignmentCenter);
		sf.SetLineAlignment(StringAlignmentCenter);
		g.DrawString(label, -1, &f, rf, &sf, &tb);
	}

	void DrawDlg(HDC hdc)
	{
		using namespace Gdiplus;
		Graphics g(hdc);
		g.SetSmoothingMode(SmoothingModeAntiAlias);
		g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
		SolidBrush bg(Color(255, 255, 255, 255));
		g.FillRectangle(&bg, 0.0f, 0.0f, (float)DLG_W, (float)DLG_H);
		Pen border(Color(255, 218, 218, 218), 1.0f);
		g.DrawRectangle(&border, 0.5f, 0.5f, (float)DLG_W - 1.0f, (float)DLG_H - 1.0f);

		FontFamily ff(L"Segoe UI");
		Font tf(&ff, 11, FontStyleBold, UnitPoint);
		Font sf9(&ff, 9, FontStyleRegular, UnitPoint);
		SolidBrush dark(Color(255, 20, 20, 20)), gray(Color(255, 95, 95, 95));
		g.DrawString(Title(), -1, &tf, RectF(20, 20, (float)DLG_W - 40, 28), nullptr, &dark);

		wchar_t sub[96];
		swprintf_s(sub, L10N(L"내보낼 페이지 범위 (1 ~ %d)", L"Page range to export (1 - %d)", L"書き出すページ範囲 (1～%d)", L"导出页码范围 (1 - %d)",
							 L"Rango de páginas (1 - %d)", L"Plage de pages (1 - %d)", L"Seitenbereich (1 - %d)", L"Intervalo de páginas (1 - %d)", L"Диапазон страниц (1 - %d)"),
				   g_ui.st->pageCount);
		g.DrawString(sub, -1, &sf9, RectF(20, 50, (float)DLG_W - 40, 22), nullptr, &gray);
		Pen line(Color(255, 235, 235, 235), 1.0f);
		g.DrawLine(&line, 20.0f, 76.0f, (float)DLG_W - 20, 76.0f);

		g.DrawString(L10N(L"시작", L"From", L"開始", L"起始", L"Desde", L"De", L"Von", L"De", L"С"), -1, &sf9, RectF(20, 88, 150, 20), nullptr, &gray);
		g.DrawString(L10N(L"끝", L"To", L"終了", L"结束", L"Hasta", L"À", L"Bis", L"Até", L"По"), -1, &sf9, RectF(190, 88, 150, 20), nullptr, &gray);
		g.DrawLine(&line, 20.0f, 150.0f, (float)DLG_W - 20, 150.0f);
		g.DrawString(L10N(L"해상도 (DPI)", L"Resolution (DPI)", L"解像度 (DPI)", L"分辨率 (DPI)", L"Resolución (DPI)", L"Résolution (DPI)", L"Auflösung (DPI)", L"Resolução (DPI)", L"Разрешение (DPI)"),
					 -1, &sf9, RectF(20, 162, 300, 20), nullptr, &gray);

		for (int i = 0; i < 4; ++i)
		{
			wchar_t t[16]; swprintf_s(t, L"%d", kDpis[i]);
			DrawBtn(g, DpiRect(i), false, g_ui.hover == 10 + i, g_ui.st->dpiIdx == i, t);
		}
		DrawBtn(g, CancelRect(), false, g_ui.hover == ID_CANCEL, false,
				L10N(L"취소", L"Cancel", L"キャンセル", L"取消", L"Cancelar", L"Annuler", L"Abbrechen", L"Cancelar", L"Отмена"));
		DrawBtn(g, OkRect(), true, g_ui.hover == ID_OK, false,
				L10N(L"내보내기", L"Export", L"書き出し", L"导出", L"Exportar", L"Exporter", L"Exportieren", L"Exportar", L"Экспорт"));
	}

	int HitTest(int x, int y)
	{
		POINT p = { x, y };
		RECT r = OkRect();     if (PtInRect(&r, p)) return ID_OK;
		r = CancelRect();      if (PtInRect(&r, p)) return ID_CANCEL;
		for (int i = 0; i < 4; ++i) { r = DpiRect(i); if (PtInRect(&r, p)) return 10 + i; }
		return 0;
	}

	void InvalidateHit(HWND h, int id)
	{
		RECT r;
		if (id == ID_OK) r = OkRect();
		else if (id == ID_CANCEL) r = CancelRect();
		else if (id >= 10) r = DpiRect(id - 10);
		else return;
		InflateRect(&r, 2, 2);
		InvalidateRect(h, &r, FALSE);
	}

	void CloseDlg(HWND h)
	{
		for (int a = 255; a >= 0; a -= 40) { SetLayeredWindowAttributes(h, 0, (BYTE)a, LWA_ALPHA); UpdateWindow(h); Sleep(8); }
		DestroyWindow(h);
	}

	void Accept(HWND h)
	{
		wchar_t a[16] = {}, b[16] = {};
		GetWindowTextW(g_ui.eFrom, a, 16); GetWindowTextW(g_ui.eTo, b, 16);
		int f = _wtoi(a), t = _wtoi(b);
		if (f < 1 || t < f || t > g_ui.st->pageCount)
		{
			MessageBeep(MB_ICONWARNING);
			SetFocus(f < 1 || f > g_ui.st->pageCount ? g_ui.eFrom : g_ui.eTo);
			return;
		}
		g_ui.st->from = f; g_ui.st->to = t;
		g_ui.accepted = true;
		CloseDlg(h);
	}

	WNDPROC g_editOrig = nullptr;
	LRESULT CALLBACK EditProc(HWND e, UINT m, WPARAM w, LPARAM l)
	{
		if (m == WM_KEYDOWN)
		{
			if (w == VK_RETURN) { SendMessageW(GetParent(e), WM_COMMAND, ID_OK, 0); return 0; }
			if (w == VK_ESCAPE) { SendMessageW(GetParent(e), WM_COMMAND, ID_CANCEL, 0); return 0; }
		}
		if (m == WM_CHAR && w == VK_RETURN) return 0;
		return CallWindowProcW(g_editOrig, e, m, w, l);
	}

	HWND MakeEdit(HWND parent, int x, int id, int value)
	{
		HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER | WS_TABSTOP,
								 x, 110, 150, 30, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
		SendMessageW(e, WM_SETFONT, (WPARAM)g_ui.font, TRUE);
		wchar_t t[16]; swprintf_s(t, L"%d", value);
		SetWindowTextW(e, t);
		g_editOrig = (WNDPROC)SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)EditProc);
		return e;
	}

	LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		switch (m)
		{
		case WM_CREATE:
			g_ui.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
			g_ui.eFrom = MakeEdit(h, 20, 101, g_ui.st->from);
			g_ui.eTo = MakeEdit(h, 190, 102, g_ui.st->to);
			SendMessageW(g_ui.eTo, EM_SETSEL, -1, 0);
			SetFocus(g_ui.eTo);
			return 0;
		case WM_PAINT:
		{
			PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
			HDC mem = CreateCompatibleDC(hdc);
			HBITMAP bm = CreateCompatibleBitmap(hdc, DLG_W, DLG_H);
			HBITMAP ob = (HBITMAP)SelectObject(mem, bm);
			DrawDlg(mem);
			BitBlt(hdc, 0, 0, DLG_W, DLG_H, mem, 0, 0, SRCCOPY);
			SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
			EndPaint(h, &ps);
			return 0;
		}
		case WM_CTLCOLOREDIT:
		{
			HDC d = (HDC)w;
			SetTextColor(d, RGB(20, 20, 20)); SetBkColor(d, RGB(255, 255, 255));
			static HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
			return (LRESULT)br;
		}
		case WM_MOUSEMOVE:
		{
			int hv = HitTest((short)LOWORD(l), (short)HIWORD(l));
			if (hv != g_ui.hover)
			{
				int old = g_ui.hover;
				g_ui.hover = hv;
				InvalidateHit(h, old);
				InvalidateHit(h, hv);
			}
			SetCursor(LoadCursorW(nullptr, hv ? IDC_HAND : IDC_ARROW));
			TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
			return 0;
		}
		case WM_MOUSELEAVE:
			InvalidateHit(h, g_ui.hover);
			g_ui.hover = 0;
			return 0;
		case WM_LBUTTONDOWN:
			g_ui.pressed = HitTest((short)LOWORD(l), (short)HIWORD(l));
			return 0;
		case WM_LBUTTONUP:
		{
			int hv = HitTest((short)LOWORD(l), (short)HIWORD(l));
			if (hv && hv == g_ui.pressed)
			{
				if (hv == ID_OK) SendMessageW(h, WM_COMMAND, ID_OK, 0);
				else if (hv == ID_CANCEL) SendMessageW(h, WM_COMMAND, ID_CANCEL, 0);
				else if (hv >= 10)
								{
									int old = g_ui.st->dpiIdx;
									g_ui.st->dpiIdx = hv - 10;
									InvalidateHit(h, 10 + old);
									InvalidateHit(h, hv);
								}
			}
			g_ui.pressed = 0;
			return 0;
		}
		case WM_COMMAND:
			if (LOWORD(w) == ID_OK) { Accept(h); return 0; }
			if (LOWORD(w) == ID_CANCEL) { g_ui.accepted = false; CloseDlg(h); return 0; }
			break;
		case WM_NCHITTEST:
		{
			LRESULT hit = DefWindowProcW(h, m, w, l);
			if (hit == HTCLIENT)
			{
				POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
				ScreenToClient(h, &p);
				HWND c = ChildWindowFromPoint(h, p);
				if (!HitTest(p.x, p.y) && (c == h || c == nullptr)) return HTCAPTION;
			}
			return hit;
		}
		case WM_ACTIVATE:
			return 0;
		case WM_DESTROY:
			if (g_ui.font) { DeleteObject(g_ui.font); g_ui.font = nullptr; }
			g_ui.done = true;
			return 0;
		}
		return DefWindowProcW(h, m, w, l);
	}

	bool RunDialog(HWND parent, DlgState& st)
	{
		static bool reg = false;
		HINSTANCE inst = GetModuleHandleW(nullptr);
		if (!reg)
		{
			WNDCLASSEXW wc = { sizeof(wc) };
			wc.lpfnWndProc = WndProc;
			wc.hInstance = inst;
			wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			wc.lpszClassName = L"ExportImagePopup";
			wc.style = CS_HREDRAW | CS_VREDRAW;
			RegisterClassExW(&wc);
			reg = true;
		}
		g_ui = Ui();
		g_ui.st = &st;

		RECT pr; GetWindowRect(parent, &pr);
		int x = pr.left + (pr.right - pr.left - DLG_W) / 2;
		int y = pr.top + (pr.bottom - pr.top - DLG_H) / 2;
		HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ExportImagePopup", L"", WS_POPUP | WS_CLIPCHILDREN,
								 x, y, DLG_W, DLG_H, parent, nullptr, inst, nullptr);
		if (!h) return false;
		g_ui.wnd = h;

		DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
		DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
		MARGINS mg = { 1, 1, 1, 1 };
		DwmExtendFrameIntoClientArea(h, &mg);

		SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
		ShowWindow(h, SW_SHOW);
		UpdateWindow(h);
		for (int a = 0; a <= 255; a += 30) { SetLayeredWindowAttributes(h, 0, (BYTE)(a < 255 ? a : 255), LWA_ALPHA); UpdateWindow(h); Sleep(8); }
		SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
		SetFocus(g_ui.eTo);
		SendMessageW(g_ui.eTo, EM_SETSEL, -1, 0);

		MSG msg;
		while (!g_ui.done && GetMessageW(&msg, nullptr, 0, 0))
		{
			if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { g_ui.accepted = false; CloseDlg(h); continue; }
			if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB)
			{
				HWND nx = (GetFocus() == g_ui.eFrom) ? g_ui.eTo : g_ui.eFrom;
				SetFocus(nx);
				SendMessageW(nx, EM_SETSEL, -1, 0);
				continue;
			}
			if (msg.hwnd == parent || IsChild(parent, msg.hwnd))
			{
				if ((msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) ||
					(msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) ||
					msg.message == WM_NCLBUTTONDOWN || msg.message == WM_NCRBUTTONDOWN)
				{
					if (msg.message == WM_LBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN) FlashWindow(h, TRUE);
					continue;
				}
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		SetForegroundWindow(parent);
		SetFocus(parent);
		return g_ui.accepted;
	}

	bool GetEncoder(const wchar_t* mime, CLSID* out)
	{
		UINT n = 0, sz = 0;
		Gdiplus::GetImageEncodersSize(&n, &sz);
		if (!sz) return false;
		std::vector<BYTE> buf(sz);
		auto* ci = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
		Gdiplus::GetImageEncoders(n, sz, ci);
		for (UINT i = 0; i < n; ++i)
			if (wcscmp(ci[i].MimeType, mime) == 0) { *out = ci[i].Clsid; return true; }
		return false;
	}
}

void ExportPagesAsImages(HWND parent)
{
	if (!g_pdfDoc || g_pageCount <= 0) return;

	DlgState st;
	st.pageCount = g_pageCount;
	st.from = 1;
	st.to = g_pageCount;
	if (!RunDialog(parent, st)) return;
	const int dpi = kDpis[st.dpiIdx];

	std::wstring def = L"page";
	size_t sep = g_filePath.find_last_of(L"\\/");
	std::wstring name = (sep == std::wstring::npos) ? g_filePath : g_filePath.substr(sep + 1);
	size_t dot = name.find_last_of(L'.');
	if (dot != std::wstring::npos) name.erase(dot);
	if (!name.empty()) def = name;

	wchar_t outBuf[MAX_PATH * 2] = {};
	wcsncpy_s(outBuf, def.c_str(), _TRUNCATE);
	OPENFILENAMEW ofn = { sizeof(ofn) };
	ofn.hwndOwner = parent;
	ofn.lpstrFilter = L"PNG (*.png)\0*.png\0JPEG (*.jpg)\0*.jpg\0\0";
	ofn.lpstrFile = outBuf;
	ofn.nMaxFile = MAX_PATH * 2;
	ofn.lpstrDefExt = L"png";
	ofn.lpstrTitle = Title();
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (!GetSaveFileNameW(&ofn)) return;

	std::wstring path = outBuf;
	std::wstring ext;
	size_t d2 = path.find_last_of(L'.');
	size_t s2 = path.find_last_of(L"\\/");
	if (d2 != std::wstring::npos && (s2 == std::wstring::npos || d2 > s2))
	{
		ext = path.substr(d2);
		path.erase(d2);
	}
	std::wstring lext = ext;
	std::transform(lext.begin(), lext.end(), lext.begin(), ::towlower);
	bool jpeg;
	if (lext == L".jpg" || lext == L".jpeg") jpeg = true;
	else if (lext == L".png") jpeg = false;
	else { jpeg = (ofn.nFilterIndex == 2); ext = jpeg ? L".jpg" : L".png"; }

	CLSID clsid;
	if (!GetEncoder(jpeg ? L"image/jpeg" : L"image/png", &clsid)) return;

	const bool multi = st.to > st.from;
	int saved = 0;
	HCURSOR oldCur = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

	for (int i = st.from - 1; i <= st.to - 1; ++i)
	{
		std::lock_guard<std::mutex> lk(g_pdfiumMtx);
		FPDF_PAGE page = FPDF_LoadPage(g_pdfDoc, i);
		if (!page) continue;

		double pw = FPDF_GetPageWidth(page), ph = FPDF_GetPageHeight(page);
		if (g_rotation == 1 || g_rotation == 3) std::swap(pw, ph);
		double wpx = pw * dpi / 72.0, hpx = ph * dpi / 72.0;
		const double kMax = 16000.0;
		double k = std::min(1.0, kMax / std::max(wpx, hpx));
		int W = (int)(wpx * k), H = (int)(hpx * k);
		if (W <= 0 || H <= 0) { FPDF_ClosePage(page); continue; }

		FPDF_BITMAP bmp = FPDFBitmap_Create(W, H, 0);
		if (bmp)
		{
			FPDFBitmap_FillRect(bmp, 0, 0, W, H, 0xFFFFFFFF);
			FPDF_RenderPageBitmap(bmp, page, 0, 0, W, H, g_rotation, FPDF_ANNOT | FPDF_LCD_TEXT);

			Gdiplus::Bitmap gb(W, H, FPDFBitmap_GetStride(bmp), PixelFormat32bppRGB,
							   reinterpret_cast<BYTE*>(FPDFBitmap_GetBuffer(bmp)));
			std::wstring file = path;
			if (multi)
			{
				wchar_t suf[16]; swprintf_s(suf, L"_%03d", i + 1);
				file += suf;
			}
			file += ext;

			if (jpeg)
			{
				ULONG q = 92;
				Gdiplus::EncoderParameters ep;
				ep.Count = 1;
				ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
				ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
				ep.Parameter[0].NumberOfValues = 1;
				ep.Parameter[0].Value = &q;
				if (gb.Save(file.c_str(), &clsid, &ep) == Gdiplus::Ok) ++saved;
			}
			else if (gb.Save(file.c_str(), &clsid, nullptr) == Gdiplus::Ok) ++saved;

			FPDFBitmap_Destroy(bmp);
		}
		FPDF_ClosePage(page);
	}

	SetCursor(oldCur);

	const int total = st.to - st.from + 1;
	wchar_t msg[200];
	if (saved == total)
		swprintf_s(msg, L10N(L"%d개 페이지를 이미지로 저장했습니다.", L"Saved %d page(s) as images.", L"%dページを画像として保存しました。", L"已将 %d 页保存为图像。",
							 L"Se guardaron %d página(s) como imágenes.", L"%d page(s) enregistrée(s) en image.", L"%d Seite(n) als Bild gespeichert.", L"%d página(s) salva(s) como imagem.",
							 L"Сохранено страниц: %d."), saved);
	else
		swprintf_s(msg, L10N(L"일부 실패: %d / %d 페이지 저장됨.", L"Partially failed: saved %d of %d pages.", L"一部失敗: %d / %dページを保存。", L"部分失败：已保存 %d / %d 页。",
							 L"Error parcial: %d de %d páginas guardadas.", L"Échec partiel : %d sur %d pages enregistrées.", L"Teilweise fehlgeschlagen: %d von %d Seiten gespeichert.",
							 L"Falha parcial: %d de %d páginas salvas.", L"Частичная ошибка: сохранено %d из %d."), saved, total);
	CleanPopup::Show(parent, Title(), msg, false);
}
