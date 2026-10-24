#pragma once
// OcrPdf.cpp 의 익명 namespace 안에서만 포함됩니다.
	// ── CleanPopup 스타일 공통 ───────────────────────────────
	constexpr int BTN_W = 100, BTN_H = 34, BTN_GAP = 10;
	enum { HIT_OK = 1, HIT_CANCEL = 2, HIT_ROW = 100 };

	RECT BtnRect(int W, int H, int slot, int n)
	{
		int total = BTN_W * n + BTN_GAP * (n - 1);
		int l = (W - total) / 2 + slot * (BTN_W + BTN_GAP);
		int t = H - 20 - BTN_H;
		return { l, t, l + BTN_W, t + BTN_H };
	}

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

	void DrawBtn(Gdiplus::Graphics& g, const RECT& rc, bool primary, bool hot, const wchar_t* label)
	{
		using namespace Gdiplus;
		FontFamily ff(L"Segoe UI");
		RectF rf((float)rc.left + 0.5f, (float)rc.top + 0.5f, (float)(rc.right - rc.left) - 1.0f, (float)(rc.bottom - rc.top) - 1.0f);
		GraphicsPath* path = RoundPath(rf, 5.0f);
		if (primary)
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
		SolidBrush tb(primary ? Color(255, 255, 255, 255) : Color(255, 45, 45, 45));
		Font f(&ff, 9, FontStyleBold, UnitPoint);
		StringFormat sf;
		sf.SetAlignment(StringAlignmentCenter);
		sf.SetLineAlignment(StringAlignmentCenter);
		g.DrawString(label, -1, &f, rf, &sf, &tb);
	}

	void DrawFrame(Gdiplus::Graphics& g, int W, int H, const wchar_t* title, const wchar_t* sub, bool line)
	{
		using namespace Gdiplus;
		g.SetSmoothingMode(SmoothingModeAntiAlias);
		g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
		SolidBrush bg(Color(255, 255, 255, 255));
		g.FillRectangle(&bg, 0.0f, 0.0f, (float)W, (float)H);
		Pen border(Color(255, 218, 218, 218), 1.0f);
		g.DrawRectangle(&border, 0.5f, 0.5f, (float)W - 1.0f, (float)H - 1.0f);
		FontFamily ff(L"Segoe UI");
		Font tf(&ff, 11, FontStyleBold, UnitPoint);
		Font sf9(&ff, 9, FontStyleRegular, UnitPoint);
		SolidBrush dark(Color(255, 20, 20, 20)), gray(Color(255, 95, 95, 95));
		g.DrawString(title, -1, &tf, RectF(20, 20, (float)W - 40, 28), nullptr, &dark);
		if (sub) g.DrawString(sub, -1, &sf9, RectF(20, 50, (float)W - 40, 36), nullptr, &gray);
		if (line)
		{
			Pen ln(Color(255, 235, 235, 235), 1.0f);
			g.DrawLine(&ln, 20.0f, 90.0f, (float)W - 20, 90.0f);
		}
	}

	void PaintBuffered(HWND h, int W, int H, void (*draw)(HDC))
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(h, &ps);
		HDC mem = CreateCompatibleDC(hdc);
		HBITMAP bm = CreateCompatibleBitmap(hdc, W, H);
		HBITMAP ob = (HBITMAP)SelectObject(mem, bm);
		draw(mem);
		BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
		SelectObject(mem, ob);
		DeleteObject(bm);
		DeleteDC(mem);
		EndPaint(h, &ps);
	}

	HWND CreatePopup(HWND parent, const wchar_t* cls, int W, int H, WNDPROC proc)
	{
		HINSTANCE inst = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc = {};
		if (!GetClassInfoExW(inst, cls, &wc))
		{
			wc = { sizeof(wc) };
			wc.lpfnWndProc = proc;
			wc.hInstance = inst;
			wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			wc.lpszClassName = cls;
			wc.style = CS_HREDRAW | CS_VREDRAW;
			RegisterClassExW(&wc);
		}
		RECT pr = {};
		int x = 100, y = 100;
		if (parent && GetWindowRect(parent, &pr))
		{
			x = pr.left + ((pr.right - pr.left) - W) / 2;
			y = pr.top + ((pr.bottom - pr.top) - H) / 2;
		}
		HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls, L"", WS_POPUP | WS_CLIPCHILDREN,
								 x, y, W, H, parent, nullptr, inst, nullptr);
		if (h)
		{
			DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
			DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
			MARGINS mg = { 1, 1, 1, 1 };
			DwmExtendFrameIntoClientArea(h, &mg);
		}
		return h;
	}

	const wchar_t* CancelLabel()
	{
		return S(L"취소", L"Cancel", L"キャンセル", L"取消", L"Cancelar", L"Annuler", L"Abbrechen", L"Cancelar", L"Отмена");
	}

	const wchar_t* DefaultLangCode()
	{
		switch (g_appLang)
		{
		case AppLang::Japanese:   return L"jpn";
		case AppLang::Chinese:    return L"chi_sim";
		case AppLang::Spanish:    return L"spa";
		case AppLang::French:     return L"fra";
		case AppLang::German:     return L"deu";
		case AppLang::Portuguese: return L"por";
		case AppLang::Russian:    return L"rus";
		case AppLang::English:    return L"eng";
		default:                  return L"kor";
		}
	}

	// Windows OCR 기본 언어: 앱 표시 언어와 같은 BCP-47 접두어
	const wchar_t* DefaultWinLangPrefix()
	{
		switch (g_appLang)
		{
		case AppLang::Japanese:   return L"ja";
		case AppLang::Chinese:    return L"zh";
		case AppLang::Spanish:    return L"es";
		case AppLang::French:     return L"fr";
		case AppLang::German:     return L"de";
		case AppLang::Portuguese: return L"pt";
		case AppLang::Russian:    return L"ru";
		case AppLang::English:    return L"en";
		default:                  return L"ko";
		}
	}

	// ── 언어 선택 대화상자 ───────────────────────────────────
	enum { ENGINE_WIN = 0, ENGINE_TESS = 1 };

	std::wstring LangDisplayName(const std::wstring& code)
	{
		struct E { const wchar_t* code; const wchar_t* name; };
		static const E tbl[] = {
			{ L"kor", L"한국어 (Korean)" }, { L"eng", L"English" }, { L"jpn", L"日本語 (Japanese)" },
			{ L"chi_sim", L"简体中文 (Chinese Simplified)" }, { L"chi_tra", L"繁體中文 (Chinese Traditional)" },
			{ L"spa", L"Español (Spanish)" }, { L"fra", L"Français (French)" }, { L"deu", L"Deutsch (German)" },
			{ L"por", L"Português (Portuguese)" }, { L"rus", L"Русский (Russian)" },
		};
		for (auto& e : tbl)
			if (code == e.code) return e.name;
		return code;
	}
	constexpr int LD_W = 380, ROW_H = 30;
	constexpr int HIT_TAB = 50; // HIT_TAB + 엔진

	struct LangDlg
	{
		HWND wnd = nullptr;
		bool done = false, ok = false;
		bool canSwitch = true;      // 엔진 토글 표시 (Win32/ARM64는 Windows OCR만)
		int engine = ENGINE_WIN;
		std::vector<std::wstring> codes[2], names[2];
		std::vector<char> sel[2];
		int scroll = 0, hover = 0, pressed = 0;
		int H = 0;
	};
	LangDlg* g_ld = nullptr;

	int LdHeight(bool canSwitch) { return canSwitch ? 484 : 440; }

	RECT LdList()
	{
		const int top = g_ld->canSwitch ? 142 : 98;
		return { 20, top, LD_W - 20, g_ld->H - 80 };
	}

	RECT LdTabRect(int engine)
	{
		const int mid = LD_W / 2;
		return engine == ENGINE_WIN ? RECT{ 20, 100, mid, 132 } : RECT{ mid, 100, LD_W - 20, 132 };
	}

	const std::vector<std::wstring>& LdCodes() { return g_ld->codes[g_ld->engine]; }

	int LdVisible() { RECT l = LdList(); return (l.bottom - l.top) / ROW_H; }

	void LdClampScroll()
	{
		int maxS = std::max(0, (int)LdCodes().size() - LdVisible());
		g_ld->scroll = std::max(0, std::min(g_ld->scroll, maxS));
	}

	RECT LdRowRect(int i)
	{
		RECT l = LdList();
		int top = l.top + (i - g_ld->scroll) * ROW_H;
		return { l.left, top, l.right - 10, top + ROW_H };
	}

	int LdHit(int x, int y)
	{
		POINT p = { x, y };
		RECT r = BtnRect(LD_W, g_ld->H, 0, 2); if (PtInRect(&r, p)) return HIT_OK;
		r = BtnRect(LD_W, g_ld->H, 1, 2);      if (PtInRect(&r, p)) return HIT_CANCEL;
		if (g_ld->canSwitch)
			for (int e = 0; e < 2; ++e)
			{
				r = LdTabRect(e);
				if (PtInRect(&r, p)) return HIT_TAB + e;
			}
		int vis = LdVisible();
		for (int i = g_ld->scroll; i < (int)LdCodes().size() && i < g_ld->scroll + vis; ++i)
		{
			r = LdRowRect(i);
			if (PtInRect(&r, p)) return HIT_ROW + i;
		}
		return 0;
	}

	void LdDrawTabs(Gdiplus::Graphics& g)
	{
		using namespace Gdiplus;
		FontFamily ff(L"Segoe UI");
		Font f(&ff, 9, FontStyleBold, UnitPoint);
		StringFormat sf;
		sf.SetAlignment(StringAlignmentCenter);
		sf.SetLineAlignment(StringAlignmentCenter);

		RECT a = LdTabRect(ENGINE_WIN), b = LdTabRect(ENGINE_TESS);
		RectF outer((float)a.left + 0.5f, (float)a.top + 0.5f, (float)(b.right - a.left) - 1.0f, (float)(a.bottom - a.top) - 1.0f);
		GraphicsPath* op = RoundPath(outer, 6.0f);
		SolidBrush track(Color(255, 240, 240, 240));
		g.FillPath(&track, op);
		delete op;

		const wchar_t* labels[2] = { L"Windows OCR", L"Tesseract" };
		for (int e = 0; e < 2; ++e)
		{
			RECT r = LdTabRect(e);
			RectF rf((float)r.left + 3, (float)r.top + 3, (float)(r.right - r.left) - 6, (float)(r.bottom - r.top) - 6);
			const bool on = (g_ld->engine == e);
			if (on || g_ld->hover == HIT_TAB + e)
			{
				GraphicsPath* p = RoundPath(rf, 4.0f);
				SolidBrush fill(on ? Color(255, 255, 255, 255) : Color(255, 228, 228, 228));
				g.FillPath(&fill, p);
				if (on)
				{
					Pen bd(Color(255, 210, 210, 210), 1.0f);
					g.DrawPath(&bd, p);
				}
				delete p;
			}
			SolidBrush tb(on ? Color(255, 0, 103, 192) : Color(255, 90, 90, 90));
			g.DrawString(labels[e], -1, &f, rf, &sf, &tb);
		}
	}

	void LdDrawEmpty(Gdiplus::Graphics& g)
	{
		using namespace Gdiplus;
		const wchar_t* msg = (g_ld->engine == ENGINE_WIN)
			? S(L"설치된 Windows OCR 언어가 없습니다.\n\n설정 > 시간 및 언어 > 언어 및 지역에서 언어의 '언어 옵션'을 열고 '광학 문자 인식'을 설치해 주세요.",
				L"No Windows OCR language is installed.\n\nOpen Settings > Time & language > Language & region, choose 'Language options' for a language and install 'Optical character recognition'.",
				L"Windows OCR の言語がインストールされていません。\n\n設定 > 時刻と言語 > 言語と地域 で言語の「言語のオプション」を開き、「光学式文字認識」をインストールしてください。",
				L"未安装 Windows OCR 语言。\n\n请在 设置 > 时间和语言 > 语言和区域 中打开语言的“语言选项”，并安装“光学字符识别”。",
				L"No hay ningún idioma de OCR de Windows instalado.\n\nAbra Configuración > Hora e idioma > Idioma y región, elija 'Opciones de idioma' e instale 'Reconocimiento óptico de caracteres'.",
				L"Aucune langue OCR Windows n'est installée.\n\nOuvrez Paramètres > Heure et langue > Langue et région, choisissez « Options de langue » et installez « Reconnaissance optique de caractères ».",
				L"Es ist keine Windows-OCR-Sprache installiert.\n\nÖffnen Sie Einstellungen > Zeit und Sprache > Sprache und Region, wählen Sie „Sprachoptionen“ und installieren Sie „Optische Zeichenerkennung“.",
				L"Nenhum idioma de OCR do Windows está instalado.\n\nAbra Configurações > Hora e idioma > Idioma e região, escolha 'Opções de idioma' e instale 'Reconhecimento óptico de caracteres'.",
				L"Языки OCR Windows не установлены.\n\nОткройте Параметры > Время и язык > Язык и регион, выберите «Параметры языка» и установите «Оптическое распознавание символов».")
			: S(L"Tesseract 언어 데이터(*.traineddata)가 없습니다.\n\n프로그램 폴더의 tessdata 폴더에 언어 파일을 넣어 주세요.",
				L"No Tesseract language data (*.traineddata) was found.\n\nPlease put language files in the 'tessdata' folder next to the program.",
				L"Tesseract の言語データ (*.traineddata) がありません。\n\nプログラムフォルダの tessdata フォルダに言語ファイルを置いてください。",
				L"未找到 Tesseract 语言数据 (*.traineddata)。\n\n请将语言文件放入程序目录下的 tessdata 文件夹。",
				L"No se encontraron datos de idioma de Tesseract (*.traineddata).\n\nColoque los archivos en la carpeta 'tessdata' junto al programa.",
				L"Aucune donnée de langue Tesseract (*.traineddata) trouvée.\n\nPlacez les fichiers dans le dossier 'tessdata' à côté du programme.",
				L"Keine Tesseract-Sprachdaten (*.traineddata) gefunden.\n\nLegen Sie die Dateien in den Ordner 'tessdata' neben dem Programm ab.",
				L"Nenhum dado de idioma do Tesseract (*.traineddata) encontrado.\n\nColoque os arquivos na pasta 'tessdata' ao lado do programa.",
				L"Языковые данные Tesseract (*.traineddata) не найдены.\n\nПоместите файлы в папку 'tessdata' рядом с программой.");
		FontFamily ff(L"Segoe UI");
		Font f(&ff, 9, FontStyleRegular, UnitPoint);
		SolidBrush gray(Color(255, 95, 95, 95));
		RECT l = LdList();
		g.DrawString(msg, -1, &f, RectF((float)l.left + 8, (float)l.top + 12, (float)(l.right - l.left) - 16, (float)(l.bottom - l.top) - 12), nullptr, &gray);
	}

	void LdDraw(HDC hdc)
	{
		using namespace Gdiplus;
		Graphics g(hdc);
		const bool multi = (g_ld->engine == ENGINE_TESS);
		DrawFrame(g, LD_W, g_ld->H, TitleStr(), multi
			? S(L"OCR 언어를 선택하세요 (여러 개 선택 가능)", L"Select OCR language(s) (multiple allowed)",
				L"OCR言語を選択してください (複数可)", L"请选择 OCR 语言 (可多选)",
				L"Seleccione idioma(s) de OCR (varios)", L"Choisissez la/les langue(s) OCR (plusieurs)",
				L"OCR-Sprache(n) auswählen (mehrere möglich)", L"Selecione o(s) idioma(s) do OCR (vários)",
				L"Выберите язык(и) OCR (можно несколько)")
			: S(L"OCR 언어를 선택하세요", L"Select the OCR language",
				L"OCR言語を選択してください", L"请选择 OCR 语言",
				L"Seleccione el idioma de OCR", L"Choisissez la langue OCR",
				L"OCR-Sprache auswählen", L"Selecione o idioma do OCR",
				L"Выберите язык OCR"), true);
		if (g_ld->canSwitch) LdDrawTabs(g);

		FontFamily ff(L"Segoe UI");
		Font f(&ff, 10, FontStyleRegular, UnitPoint);
		SolidBrush dark(Color(255, 30, 30, 30));
		StringFormat sf;
		sf.SetLineAlignment(StringAlignmentCenter);

		const auto& codes = LdCodes();
		const auto& names = g_ld->names[g_ld->engine];
		const auto& sel = g_ld->sel[g_ld->engine];
		if (codes.empty()) LdDrawEmpty(g);

		int vis = LdVisible();
		for (int i = g_ld->scroll; i < (int)codes.size() && i < g_ld->scroll + vis; ++i)
		{
			RECT r = LdRowRect(i);
			bool hot = (g_ld->hover == HIT_ROW + i);
			bool on = sel[i] != 0;
			if (hot)
			{
				SolidBrush hb(Color(255, 243, 246, 250));
				RectF rr((float)r.left, (float)r.top + 1, (float)(r.right - r.left), (float)ROW_H - 2);
				GraphicsPath* p = RoundPath(rr, 5.0f);
				g.FillPath(&hb, p);
				delete p;
			}
			RectF cb((float)r.left + 8, (float)r.top + (ROW_H - 18) / 2.0f, 18, 18);
			if (multi)
			{
				GraphicsPath* cp = RoundPath(cb, 4.0f);
				if (on)
				{
					SolidBrush blue(Color(255, 0, 120, 212));
					g.FillPath(&blue, cp);
					Pen w(Color(255, 255, 255, 255), 2.0f);
					w.SetLineJoin(LineJoinRound);
					PointF pts[3] = { PointF(cb.X + 4.5f, cb.Y + 9.5f), PointF(cb.X + 7.8f, cb.Y + 13.0f), PointF(cb.X + 13.5f, cb.Y + 5.5f) };
					g.DrawLines(&w, pts, 3);
				}
				else
				{
					SolidBrush white(Color(255, 255, 255, 255));
					g.FillPath(&white, cp);
					Pen bd(Color(255, 150, 150, 150), 1.0f);
					g.DrawPath(&bd, cp);
				}
				delete cp;
			}
			else
			{
				// 단일 선택: 라디오 버튼
				SolidBrush white(Color(255, 255, 255, 255));
				g.FillEllipse(&white, cb);
				Pen bd(on ? Color(255, 0, 120, 212) : Color(255, 150, 150, 150), on ? 1.5f : 1.0f);
				g.DrawEllipse(&bd, cb);
				if (on)
				{
					SolidBrush blue(Color(255, 0, 120, 212));
					g.FillEllipse(&blue, cb.X + 4.5f, cb.Y + 4.5f, cb.Width - 9.0f, cb.Height - 9.0f);
				}
			}
			g.DrawString(names[i].c_str(), -1, &f, RectF((float)r.left + 38, (float)r.top, (float)(r.right - r.left) - 40, (float)ROW_H), &sf, &dark);
		}

		if ((int)codes.size() > vis)
		{
			RECT l = LdList();
			float trackH = (float)(l.bottom - l.top);
			float total = (float)codes.size();
			float thumbH = std::max(24.0f, trackH * vis / total);
			float maxS = total - vis;
			float ty = l.top + (trackH - thumbH) * (maxS > 0 ? g_ld->scroll / maxS : 0.0f);
			SolidBrush tb(Color(255, 200, 200, 200));
			GraphicsPath* tp = RoundPath(RectF((float)l.right - 6, ty, 4, thumbH), 2.0f);
			g.FillPath(&tb, tp);
			delete tp;
		}

		DrawBtn(g, BtnRect(LD_W, g_ld->H, 0, 2), true, g_ld->hover == HIT_OK, S(L"확인", L"OK", L"OK", L"确定", L"Aceptar", L"OK", L"OK", L"OK", L"OK"));
		DrawBtn(g, BtnRect(LD_W, g_ld->H, 1, 2), false, g_ld->hover == HIT_CANCEL, CancelLabel());
	}

	void LdAccept()
	{
		const auto& sel = g_ld->sel[g_ld->engine];
		if (std::find(sel.begin(), sel.end(), (char)1) == sel.end()) { MessageBeep(MB_ICONWARNING); return; }
		g_ld->ok = true;
		g_ld->done = true;
	}

	void LdSetEngine(int e)
	{
		if (!g_ld->canSwitch || g_ld->engine == e) return;
		g_ld->engine = e;
		g_ld->scroll = 0;
		InvalidateRect(g_ld->wnd, nullptr, FALSE);
	}

	LRESULT CALLBACK LangProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		if (!g_ld) return DefWindowProcW(h, m, w, l);
		switch (m)
		{
		case WM_PAINT:
			PaintBuffered(h, LD_W, g_ld->H, LdDraw);
			return 0;
		case WM_ERASEBKGND:
			return 1;
		case WM_MOUSEMOVE:
		{
			int hv = LdHit((short)LOWORD(l), (short)HIWORD(l));
			if (hv != g_ld->hover) { g_ld->hover = hv; InvalidateRect(h, nullptr, FALSE); }
			SetCursor(LoadCursorW(nullptr, hv ? IDC_HAND : IDC_ARROW));
			TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
			TrackMouseEvent(&t);
			return 0;
		}
		case WM_MOUSELEAVE:
			g_ld->hover = 0;
			InvalidateRect(h, nullptr, FALSE);
			return 0;
		case WM_MOUSEWHEEL:
			g_ld->scroll -= GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
			LdClampScroll();
			InvalidateRect(h, nullptr, FALSE);
			return 0;
		case WM_LBUTTONDOWN:
			g_ld->pressed = LdHit((short)LOWORD(l), (short)HIWORD(l));
			return 0;
		case WM_LBUTTONUP:
		{
			int hv = LdHit((short)LOWORD(l), (short)HIWORD(l));
			if (hv && hv == g_ld->pressed)
			{
				if (hv == HIT_OK) LdAccept();
				else if (hv == HIT_CANCEL) g_ld->done = true;
				else if (hv >= HIT_ROW)
				{
					int i = hv - HIT_ROW;
					auto& sel = g_ld->sel[g_ld->engine];
					if (g_ld->engine == ENGINE_TESS) sel[i] = !sel[i];
					else { std::fill(sel.begin(), sel.end(), (char)0); sel[i] = 1; }
					InvalidateRect(h, nullptr, FALSE);
				}
				else if (hv >= HIT_TAB) LdSetEngine(hv - HIT_TAB);
			}
			g_ld->pressed = 0;
			return 0;
		}
		case WM_KEYDOWN:
			if (w == VK_RETURN) { LdAccept(); return 0; }
			if (w == VK_ESCAPE) { g_ld->done = true; return 0; }
			if (w == VK_TAB) { LdSetEngine(1 - g_ld->engine); return 0; }
			break;
		case WM_NCHITTEST:
		{
			LRESULT hit = DefWindowProcW(h, m, w, l);
			if (hit == HTCLIENT)
			{
				POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
				ScreenToClient(h, &p);
				RECT list = LdList();
				if (!LdHit(p.x, p.y) && !PtInRect(&list, p)) return HTCAPTION;
			}
			return hit;
		}
		case WM_CLOSE:
			g_ld->done = true;
			return 0;
		}
		return DefWindowProcW(h, m, w, l);
	}

	// 대화상자 입출력 - 들어올 때 기본 선택, 나갈 때 사용자 선택
	struct OcrChoice
	{
		int engine = ENGINE_WIN;
		std::wstring winLang;                // Windows OCR 언어 태그
		std::vector<std::wstring> tessLangs; // Tesseract 언어 코드
	};

	bool ChooseLanguages(HWND parent, const std::vector<WinOcrLang>& winLangs, const std::vector<std::wstring>& tessLangs,
						 bool canSwitch, OcrChoice& choice)
	{
		LangDlg d;
		d.canSwitch = canSwitch;
		d.engine = canSwitch ? choice.engine : ENGINE_WIN;
		d.H = LdHeight(canSwitch);
		for (const auto& wl : winLangs)
		{
			d.codes[ENGINE_WIN].push_back(wl.tag);
			d.names[ENGINE_WIN].push_back(wl.name);
			d.sel[ENGINE_WIN].push_back(_wcsicmp(wl.tag.c_str(), choice.winLang.c_str()) == 0 ? 1 : 0);
		}
		for (const auto& tl : tessLangs)
		{
			d.codes[ENGINE_TESS].push_back(tl);
			d.names[ENGINE_TESS].push_back(LangDisplayName(tl));
			d.sel[ENGINE_TESS].push_back(std::find(choice.tessLangs.begin(), choice.tessLangs.end(), tl) != choice.tessLangs.end() ? 1 : 0);
		}
		g_ld = &d;

		d.wnd = CreatePopup(parent, L"PlainOcrLangPopup", LD_W, d.H, LangProc);
		if (!d.wnd) { g_ld = nullptr; return false; }

		if (parent) EnableWindow(parent, FALSE);
		ShowWindow(d.wnd, SW_SHOW);
		SetForegroundWindow(d.wnd);
		SetFocus(d.wnd);
		MSG msg;
		while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (parent) EnableWindow(parent, TRUE);
		DestroyWindow(d.wnd);
		if (parent) SetForegroundWindow(parent);
		g_ld = nullptr;
		if (!d.ok) return false;

		choice.engine = d.engine;
		const auto& codes = d.codes[d.engine];
		const auto& sel = d.sel[d.engine];
		if (d.engine == ENGINE_WIN)
		{
			for (size_t i = 0; i < codes.size(); ++i)
				if (sel[i]) choice.winLang = codes[i];
		}
		else
		{
			choice.tessLangs.clear();
			for (size_t i = 0; i < codes.size(); ++i)
				if (sel[i]) choice.tessLangs.push_back(codes[i]);
		}
		return true;
	}

	// ── 진행 창 ──────────────────────────────────────────────
	constexpr int PG_W = 360, PG_H = 190;
	HWND g_progWnd = nullptr;
	int g_progCur = 0, g_progTotal = 1, g_progHover = 0;
	std::wstring g_progMsg;

	RECT ProgCancelRect()
	{
		int l = (PG_W - BTN_W) / 2, t = PG_H - 20 - BTN_H;
		return { l, t, l + BTN_W, t + BTN_H };
	}

	void ProgDraw(HDC hdc)
	{
		using namespace Gdiplus;
		Graphics g(hdc);
		DrawFrame(g, PG_W, PG_H, TitleStr(), g_progMsg.c_str(), false);

		float frac = g_progTotal > 0 ? (float)std::max(0, g_progCur - 1) / (float)g_progTotal : 0.0f;
		frac = std::max(0.0f, std::min(1.0f, frac));
		RectF track(20, 92, (float)PG_W - 40, 8);
		SolidBrush tb(Color(255, 232, 232, 232));
		GraphicsPath* tp = RoundPath(track, 4.0f);
		g.FillPath(&tb, tp);
		delete tp;
		if (frac > 0.0f)
		{
			RectF fill(track.X, track.Y, std::max(8.0f, track.Width * frac), track.Height);
			LinearGradientBrush grad(PointF(fill.X, fill.Y), PointF(fill.X, fill.Y + fill.Height), Color(255, 14, 148, 244), Color(255, 0, 108, 196));
			GraphicsPath* fp = RoundPath(fill, 4.0f);
			g.FillPath(&grad, fp);
			delete fp;
		}
		DrawBtn(g, ProgCancelRect(), false, g_progHover == HIT_CANCEL, CancelLabel());
	}

	LRESULT CALLBACK ProgProc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		switch (m)
		{
		case WM_PAINT:
			PaintBuffered(h, PG_W, PG_H, ProgDraw);
			return 0;
		case WM_ERASEBKGND:
			return 1;
		case WM_MOUSEMOVE:
		{
			POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
			RECT r = ProgCancelRect();
			int hv = PtInRect(&r, p) ? HIT_CANCEL : 0;
			if (hv != g_progHover) { g_progHover = hv; InvalidateRect(h, nullptr, FALSE); }
			SetCursor(LoadCursorW(nullptr, hv ? IDC_HAND : IDC_ARROW));
			TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
			TrackMouseEvent(&t);
			return 0;
		}
		case WM_MOUSELEAVE:
			g_progHover = 0;
			InvalidateRect(h, nullptr, FALSE);
			return 0;
		case WM_LBUTTONUP:
		{
			POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
			RECT r = ProgCancelRect();
			if (PtInRect(&r, p)) g_cancel = true;
			return 0;
		}
		case WM_NCHITTEST:
		{
			LRESULT hit = DefWindowProcW(h, m, w, l);
			if (hit == HTCLIENT)
			{
				POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
				ScreenToClient(h, &p);
				RECT r = ProgCancelRect();
				if (!PtInRect(&r, p)) return HTCAPTION;
			}
			return hit;
		}
		case WM_CLOSE:
			g_cancel = true;
			return 0;
		}
		return DefWindowProcW(h, m, w, l);
	}

	void ShowProgress(HWND parent)
	{
		g_progCur = 0; g_progTotal = 1; g_progHover = 0; g_progMsg.clear();
		g_progWnd = CreatePopup(parent, L"PlainOcrProgressPopup", PG_W, PG_H, ProgProc);
		if (!g_progWnd) return;
		if (parent) EnableWindow(parent, FALSE);
		ShowWindow(g_progWnd, SW_SHOW);
		UpdateWindow(g_progWnd);
	}

	void SetProgress(int cur, int total)
	{
		wchar_t buf[128];
		swprintf_s(buf, S(L"OCR 진행 중: %d / %d", L"OCR in progress: %d / %d", L"OCR実行中: %d / %d", L"OCR 进行中: %d / %d",
						  L"OCR en curso: %d / %d", L"OCR en cours : %d / %d", L"OCR läuft: %d / %d", L"OCR em andamento: %d / %d",
						  L"OCR выполняется: %d / %d"), cur, total);
		g_progCur = cur; g_progTotal = total; g_progMsg = buf;
		if (g_progWnd) { InvalidateRect(g_progWnd, nullptr, FALSE); UpdateWindow(g_progWnd); }
		PumpMessages();
	}

	void CloseProgress(HWND parent)
	{
		if (parent) EnableWindow(parent, TRUE);
		if (g_progWnd) DestroyWindow(g_progWnd);
		g_progWnd = nullptr;
		if (parent) SetForegroundWindow(parent);
	}

