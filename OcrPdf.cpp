// ---------------------------------------------------------------
// OcrPdf.cpp - PDFium 렌더링 + Windows OCR / Tesseract 로 검색 가능한 PDF 생성
// ---------------------------------------------------------------
#include "framework.h"
#include "PlainPDFViewer.h"
#include "OcrPdf.h"
#include "Lang.h"
#include "CleanPopup.h"
#include "WinOcr.h"
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <atomic>
#include <thread>
#include <memory>
#include <functional>
#include <cmath>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
// Tesseract(vcpkg)는 x64에만 구성되어 있음 - Win32/ARM64는 Windows OCR만 사용
#if defined(_M_X64)
#define OCR_HAS_TESSERACT 1
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>
#else
#define OCR_HAS_TESSERACT 0
#endif

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "xmllite.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wldap32.lib")
#pragma comment(lib, "normaliz.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "advapi32.lib")

extern std::mutex g_pdfiumMtx;

namespace
{
	const wchar_t* S(const wchar_t* ko, const wchar_t* en, const wchar_t* ja, const wchar_t* zh,
					 const wchar_t* es, const wchar_t* fr, const wchar_t* de, const wchar_t* pt, const wchar_t* ru)
	{
		return (::LS)(ko, en, ja, zh, es, fr, de, pt, ru);
	}

	const wchar_t* TitleStr() { return L"OCR"; }

	constexpr int OCR_DPI = 300;

	// ── tessdata / 언어 목록 ──────────────────────────────────────
	bool g_cancel = false;

	void PumpMessages()
	{
		MSG m;
		while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}

	bool DirExists(const std::wstring& p)
	{
		DWORD a = GetFileAttributesW(p.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
	}

	std::string WToUtf8(const std::wstring& w)
	{
		if (w.empty()) return {};
		int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
		std::string s(n, '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
		return s;
	}
	std::wstring Utf8ToW(const std::string& s)
	{
		if (s.empty()) return L"";
		int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
		std::wstring w(n, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
		return w;
	}

	std::string PathToNarrow(const std::wstring& p)
	{
		wchar_t sp[MAX_PATH] = {};
		const wchar_t* use = p.c_str();
		DWORD n = GetShortPathNameW(p.c_str(), sp, MAX_PATH);
		if (n > 0 && n < MAX_PATH) use = sp;
		int len = WideCharToMultiByte(CP_ACP, 0, use, -1, nullptr, 0, nullptr, nullptr);
		std::string s(len > 0 ? len - 1 : 0, '\0');
		if (len > 1) WideCharToMultiByte(CP_ACP, 0, use, -1, s.data(), len, nullptr, nullptr);
		return s;
	}

	bool HasTrainedData(const std::wstring& dir)
	{
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW((dir + L"\\*.traineddata").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return false;
		FindClose(h);
		return true;
	}

	std::wstring FindTessData()
	{
		std::vector<std::wstring> cands;
		wchar_t mod[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, mod, MAX_PATH);
		PathRemoveFileSpecW(mod);
		{
			std::wstring d = mod;
			for (int up = 0; up < 5 && !d.empty(); ++up)
			{
				cands.push_back(d + L"\\tessdata");
				wchar_t tmp[MAX_PATH] = {};
				wcsncpy_s(tmp, d.c_str(), _TRUNCATE);
				if (!PathRemoveFileSpecW(tmp)) break;
				d = tmp;
			}
		}

		wchar_t env[MAX_PATH] = {};
		if (GetEnvironmentVariableW(L"TESSDATA_PREFIX", env, MAX_PATH) > 0)
		{
			cands.push_back(env);
			cands.push_back(std::wstring(env) + L"\\tessdata");
		}
		cands.push_back(L"C:\\Program Files\\Tesseract-OCR\\tessdata");
		for (auto& c : cands)
			if (DirExists(c) && HasTrainedData(c)) return c;
		return L"";
	}

	std::vector<std::wstring> ListLanguages(const std::wstring& dir)
	{
		std::vector<std::wstring> langs;
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW((dir + L"\\*.traineddata").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) return langs;
		do
		{
			std::wstring n = fd.cFileName;
			n.resize(n.size() - wcslen(L".traineddata"));
			if (n != L"osd") langs.push_back(n);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
		std::sort(langs.begin(), langs.end());
		return langs;
	}

#include "OcrPopupUi.h"

	// ── 이미지/문서 유틸 ─────────────────────────────────────────

	struct MemWrite
	{
		FPDF_FILEWRITE fw;
		std::vector<char>* buf;
	};

	int MemWriteBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size)
	{
		auto* mw = reinterpret_cast<MemWrite*>(self);
		const char* p = static_cast<const char*>(data);
		mw->buf->insert(mw->buf->end(), p, p + size);
		return 1;
	}

	bool SerializeDoc(FPDF_DOCUMENT doc, std::vector<char>& out)
	{
		MemWrite mw = {};
		mw.fw.version = 1;
		mw.fw.WriteBlock = MemWriteBlock;
		mw.buf = &out;
		out.clear();
		return FPDF_SaveAsCopy(doc, &mw.fw, 0) != 0;
	}

	bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out)
	{
		FILE* fp = nullptr;
		if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;
		fseek(fp, 0, SEEK_END);
		long sz = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		if (sz <= 0) { fclose(fp); return false; }
		out.resize((size_t)sz);
		size_t rd = fread(out.data(), 1, (size_t)sz, fp);
		fclose(fp);
		return rd == (size_t)sz;
	}

	struct OcrWord
	{
		int left = 0, top = 0, w = 0, h = 0;
		std::wstring text;
	};

	// 문자(숫자/라틴/한글/가나/한자 등) 판정 - CRT 로캘에 의존하지 않도록 CJK 범위 명시
	bool IsWordChar(wchar_t c)
	{
		if (c >= 0xAC00 && c <= 0xD7A3) return true; // 한글 음절
		if (c >= 0x1100 && c <= 0x11FF) return true; // 한글 자모
		if (c >= 0x3130 && c <= 0x318F) return true; // 한글 호환 자모
		if (c >= 0x3040 && c <= 0x30FF) return true; // 히라가나/가타카나
		if (c >= 0x3400 && c <= 0x4DBF) return true; // CJK 확장 A
		if (c >= 0x4E00 && c <= 0x9FFF) return true; // CJK 통합 한자
		if (c >= 0xF900 && c <= 0xFAFF) return true; // CJK 호환 한자
		if (c >= 0xFF10 && c <= 0xFF5A) return iswalnum(c - 0xFEE0) != 0; // 전각 영숫자
		return iswalnum(c) != 0;
	}

	bool IsCjkChar(wchar_t c)
	{
		return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) ||
			   (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF);
	}

	// 작업 스레드에서 실행하면서 UI 메시지 처리
	void RunPumping(const std::function<void()>& fn)
	{
		std::atomic<bool> done{ false };
		std::thread th([&] { fn(); done = true; });
		while (!done)
		{
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
			PumpMessages();
		}
		th.join();
	}

	// 시계 방향 cw도 회전 (90이면 결과 크기는 h x w) - 그레이(BYTE) / BGRA(uint32_t) 공용
	template <class T>
	void RotatePixels(const std::vector<T>& src, int w, int h, int cw, std::vector<T>& dst, int& dw, int& dh)
	{
		dw = (cw == 180) ? w : h;
		dh = (cw == 180) ? h : w;
		dst.resize((size_t)w * h);
		for (int y = 0; y < dh; ++y)
			for (int x = 0; x < dw; ++x)
			{
				int sx, sy;
				if (cw == 90)       { sx = y;         sy = h - 1 - x; }
				else if (cw == 180) { sx = w - 1 - x; sy = h - 1 - y; }
				else                { sx = w - 1 - y; sy = x; }
				dst[(size_t)y * dw + x] = src[(size_t)sy * w + sx];
			}
	}

	// Windows OCR (신뢰도 점수가 없으므로 score는 인식된 글자 수)
	bool RecognizeWin(const std::wstring& tag, const uint32_t* bgra, int w, int h, std::vector<OcrWord>& words, double& score)
	{
		score = 0;
		std::vector<WinOcrWord> res;
		bool ok = false;
		RunPumping([&] { ok = WinOcr_Recognize(tag, reinterpret_cast<const uint8_t*>(bgra), w, h, w * 4, res); });
		if (!ok) return false;
		for (auto& r : res)
		{
			if (r.text.empty() || r.w <= 0 || r.h <= 0) continue;
			for (wchar_t c : r.text)
				if (IsWordChar(c)) score += 1;
			OcrWord wd;
			wd.left = r.left; wd.top = r.top; wd.w = r.w; wd.h = r.h;
			wd.text = std::move(r.text);
			words.push_back(std::move(wd));
		}
		return true;
	}

#if OCR_HAS_TESSERACT
	// OSD: 바로 세우려면 시계 방향으로 몇 도 돌려야 하는지 (0/90/180/270)
	int DetectRotation(tesseract::TessBaseAPI& osd, const uint8_t* gray, int w, int h)
	{
		osd.SetImage(gray, w, h, 1, w);
		int deg = 0;
		float conf = 0.f;
		bool ok = false;
		RunPumping([&] { ok = osd.DetectOrientationScript(&deg, &conf, nullptr, nullptr); });
		osd.Clear();
		if (!ok || conf < 2.f) return 0;
		return (360 - deg) % 360;
	}

	bool IsCjkText(const std::wstring& s)
	{
		return std::any_of(s.begin(), s.end(), IsCjkChar);
	}

	struct LineWord
	{
		OcrWord wd;
		float conf = 0.f;
		size_t good = 0;         // 문자(IsWordChar) 수
		bool shortLatin = false; // 한글/CJK 없이 1~2글자뿐인 토큰 ("ox", "El", "J")
		bool keep = false;       // 단어 단위 필터 통과 여부
	};

	// 줄 단위 잡음 판정: 그림·차트·아이콘을 글자로 읽은 줄은
	// 평균 신뢰도가 낮거나, 아주 짧거나, 1~2글자 라틴 토큰이 대부분
	bool IsNoiseLine(const std::vector<LineWord>& line)
	{
		if (line.empty()) return true;
		double sum = 0;
		size_t chars = 0, shortTok = 0, keptGood = 0;
		for (const auto& lw : line)
		{
			const size_t c = std::max<size_t>(1, lw.good);
			sum += lw.conf * c;
			chars += c;
			if (lw.shortLatin) ++shortTok;
			if (lw.keep) keptGood += lw.good;
		}
		const double mean = sum / chars; // 글자 수 가중 평균 신뢰도
		if (keptGood == 0) return true;                                              // 남는 게 기호뿐 ("-", "/", "\")
		if (mean < 50.0) return true;
		if (chars < 3 && mean < 80.0) return true;                                   // "ox", "rot", "이"
		if (line.size() >= 3 && shortTok * 10 >= line.size() * 6 && mean < 75.0) return true; // "ollxhu n 고 J Ho Ras"
		return false;
	}

	bool RecognizeWords(tesseract::TessBaseAPI& api, const uint8_t* gray, int w, int h, std::vector<OcrWord>& words, double& score)
	{
		score = 0;
		api.SetImage(gray, w, h, 1, w);
		bool ok = false;
		RunPumping([&] { ok = (api.Recognize(nullptr) == 0); });
		if (!ok) { api.Clear(); return false; }

		// 한 줄씩 모아서 줄 전체가 잡음인지 판단한 뒤 단어를 내보냄
		std::vector<LineWord> line;
		auto flush = [&]
		{
			if (!IsNoiseLine(line))
				for (auto& lw : line)
				{
					if (!lw.keep) continue;
					score += lw.conf * (double)lw.good;
					words.push_back(std::move(lw.wd));
				}
			line.clear();
		};

		std::unique_ptr<tesseract::ResultIterator> it(api.GetIterator());
		if (it)
		{
			do
			{
				if (it->IsAtBeginningOf(tesseract::RIL_TEXTLINE)) flush();
				// 그림/차트로 분류된 영역은 건너뜀
				if (tesseract::PTIsImageType(it->BlockType())) continue;
				std::unique_ptr<char[]> t(it->GetUTF8Text(tesseract::RIL_WORD));
				if (!t) continue;
				const float conf = it->Confidence(tesseract::RIL_WORD);
				int l, tp, r, b;
				if (!it->BoundingBox(tesseract::RIL_WORD, &l, &tp, &r, &b)) continue;
				std::wstring txt = Utf8ToW(t.get());
				while (!txt.empty() && (txt.back() == L' ' || txt.back() == L'\t' || txt.back() == L'\n' || txt.back() == L'\r')) txt.pop_back();
				if (txt.empty() || r <= l || b <= tp) continue;

				LineWord lw;
				lw.conf = conf;
				bool cjk = false;
				for (wchar_t c : txt)
				{
					if (IsWordChar(c)) ++lw.good;
					if (IsCjkChar(c) || (c >= 0xAC00 && c <= 0xD7A3)) cjk = true;
				}
				lw.shortLatin = !cjk && lw.good <= 2;

				// 단어 단위 필터 - LSTM 단어 신뢰도는 정답이어도 50~70이 흔하므로 낮게 잡음
				lw.keep = conf >= 30.f;
				// 기호만 있는 토큰("-", "※" 등)은 신뢰도가 높을 때만 유지
				if (lw.good == 0 && conf < 80.f) lw.keep = false;
				// "(주)", "2024.10.20" 등은 통과, "|||" 같은 잡음은 제외
				if (lw.good > 0 && lw.good * 10 < txt.size() * 3) lw.keep = false;
				if (txt.size() == 1 && lw.good == 1 && conf < 50.f) lw.keep = false;
				// 세로로 긴 잡음(괘선 등) 제외 - CJK는 세로쓰기가 있으므로 예외
				if (!IsCjkText(txt) && (b - tp) > 3 * (r - l) + 60 && txt.size() > 1) lw.keep = false;

				lw.wd.left = l; lw.wd.top = tp; lw.wd.w = r - l; lw.wd.h = b - tp;
				lw.wd.text = std::move(txt);
				line.push_back(std::move(lw));
			} while (it->Next(tesseract::RIL_WORD));
		}
		flush();
		api.Clear();
		return true;
	}
#endif // OCR_HAS_TESSERACT

	// 페이지에 이미 쓸 만한 텍스트가 있는지 (디지털 PDF, 이미 OCR된 PDF)
	// 쪽 번호·워터마크 정도의 짧은 텍스트나 글꼴 매핑이 깨진 텍스트는 없는 것으로 봄
	bool HasTextLayer(FPDF_PAGE page)
	{
		constexpr int MIN_CHARS = 20;
		FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
		if (!tp) return false;
		const int n = FPDFText_CountChars(tp);
		int valid = 0;
		for (int i = 0; i < n && valid < MIN_CHARS; ++i)
		{
			const unsigned u = FPDFText_GetUnicode(tp, i);
			if (u <= 0x20 || u == 0xFFFD || u == 0xFFFE || (u >= 0xE000 && u <= 0xF8FF)) continue;
			++valid;
		}
		FPDFText_ClosePage(tp);
		return valid >= MIN_CHARS;
	}

	// PDF 행렬 (x,y) -> (a*x + c*y + e, b*x + d*y + f)
	struct Mat { double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; };

	// first 적용 후 second 적용
	Mat Then(const Mat& m1, const Mat& m2)
	{
		Mat r;
		r.a = m2.a * m1.a + m2.c * m1.b;
		r.b = m2.b * m1.a + m2.d * m1.b;
		r.c = m2.a * m1.c + m2.c * m1.d;
		r.d = m2.b * m1.c + m2.d * m1.d;
		r.e = m2.a * m1.e + m2.c * m1.f + m2.e;
		r.f = m2.b * m1.e + m2.d * m1.f + m2.f;
		return r;
	}

	// 보이지 않는 텍스트로 단어 추가
	// words 좌표는 원본 비트맵(bmpW x bmpH)을 시계 방향 cw도 돌린 이미지 기준
	void AddInvisibleWords(FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_FONT font,
						   const std::vector<OcrWord>& words, int bmpW, int bmpH, int cw)
	{
		// 원본 비트맵 픽셀 -> 페이지 좌표 (/Rotate 포함)
		double x0, y0, xx, xy, yx, yy;
		FPDF_DeviceToPage(page, 0, 0, bmpW, bmpH, 0, 0, 0, &x0, &y0);
		FPDF_DeviceToPage(page, 0, 0, bmpW, bmpH, 0, bmpW, 0, &xx, &xy);
		FPDF_DeviceToPage(page, 0, 0, bmpW, bmpH, 0, 0, bmpH, &yx, &yy);
		Mat dev;
		dev.a = (xx - x0) / bmpW; dev.b = (xy - y0) / bmpW;
		dev.c = (yx - x0) / bmpH; dev.d = (yy - y0) / bmpH;
		dev.e = x0; dev.f = y0;

		// 회전된 이미지 픽셀 -> 원본 비트맵 픽셀
		Mat rot;
		if (cw == 90)       { rot.a = 0;  rot.b = -1; rot.c = 1;  rot.d = 0;  rot.e = 0;    rot.f = bmpH; }
		else if (cw == 180) { rot.a = -1; rot.b = 0;  rot.c = 0;  rot.d = -1; rot.e = bmpW; rot.f = bmpH; }
		else if (cw == 270) { rot.a = 0;  rot.b = 1;  rot.c = -1; rot.d = 0;  rot.e = bmpW; rot.f = 0; }

		const Mat toPage = Then(rot, dev);
		const double k = std::sqrt(toPage.a * toPage.a + toPage.b * toPage.b); // 픽셀당 페이지 단위
		if (k <= 0) return;

		for (const auto& wd : words)
		{
			const double bh = wd.h * k;
			if (wd.w <= 0 || bh <= 0) continue;

			FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, font, (float)bh);
			if (!obj) continue;
			FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(wd.text.c_str()));
			FPDFTextObj_SetTextRenderMode(obj, FPDF_TEXTRENDERMODE_INVISIBLE);

			float l = 0, b = 0, r = 0, t = 0;
			if (FPDFPageObj_GetBounds(obj, &l, &b, &r, &t) && r - l > 0.01f)
			{
				// 텍스트 공간 -> 회전된 이미지 픽셀: 가로는 단어 폭에 맞추고, 아래쪽을 단어 하단에 정렬
				Mat txt;
				txt.a = wd.w / (r - l);
				txt.d = -1.0 / k;
				txt.e = wd.left - l * txt.a;
				txt.f = (wd.top + wd.h) - b * txt.d;
				const Mat m = Then(txt, toPage);
				FPDFPageObj_Transform(obj, m.a, m.b, m.c, m.d, m.e, m.f);
				FPDFPage_InsertObject(page, obj);
			}
			else
			{
				FPDFPageObj_Destroy(obj);
			}
		}
	}

	void Msg(HWND parent, const wchar_t* text, UINT icon)
	{
		CleanPopup::Show(parent, TitleStr(), text, false);
	}

	// ── 마지막 선택 기억 (HKCU\Software\PlainPDFViewer) ──────────
	constexpr const wchar_t* OCR_REG_KEY = L"Software\\PlainPDFViewer";

	std::wstring RegReadStr(const wchar_t* name)
	{
		wchar_t buf[512] = {};
		DWORD cb = sizeof(buf) - sizeof(wchar_t);
		if (RegGetValueW(HKEY_CURRENT_USER, OCR_REG_KEY, name, RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return L"";
		return buf;
	}

	void LoadOcrChoice(OcrChoice& c, bool& hasEngine)
	{
		DWORD v = 0, cb = sizeof(v);
		hasEngine = RegGetValueW(HKEY_CURRENT_USER, OCR_REG_KEY, L"OcrEngine", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS;
		c.engine = (v == ENGINE_TESS) ? ENGINE_TESS : ENGINE_WIN;
		c.winLang = RegReadStr(L"OcrWinLang");
		c.tessLangs.clear();
		std::wstring t = RegReadStr(L"OcrTessLangs");
		for (size_t p = 0; p < t.size();)
		{
			size_t q = t.find(L'+', p);
			if (q == std::wstring::npos) q = t.size();
			if (q > p) c.tessLangs.push_back(t.substr(p, q - p));
			p = q + 1;
		}
	}

	void SaveOcrChoice(const OcrChoice& c)
	{
		HKEY k = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, OCR_REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
		DWORD v = (DWORD)c.engine;
		RegSetValueExW(k, L"OcrEngine", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
		RegSetValueExW(k, L"OcrWinLang", 0, REG_SZ, (const BYTE*)c.winLang.c_str(), (DWORD)((c.winLang.size() + 1) * sizeof(wchar_t)));
		std::wstring t;
		for (size_t i = 0; i < c.tessLangs.size(); ++i)
			t += (i ? L"+" : L"") + c.tessLangs[i];
		RegSetValueExW(k, L"OcrTessLangs", 0, REG_SZ, (const BYTE*)t.c_str(), (DWORD)((t.size() + 1) * sizeof(wchar_t)));
		RegCloseKey(k);
	}

	// 저장된 선택이 현재 설치된 언어와 맞지 않으면 기본값으로 보정
	void FixupOcrChoice(OcrChoice& c, bool hasEngine, bool canSwitch,
						const std::vector<WinOcrLang>& winLangs, const std::vector<std::wstring>& tessLangs)
	{
		// 엔진: 처음 실행이면 Windows OCR 언어가 있을 때 Windows OCR
		if (!canSwitch) c.engine = ENGINE_WIN;
		else if (!hasEngine) c.engine = winLangs.empty() && !tessLangs.empty() ? ENGINE_TESS : ENGINE_WIN;

		auto hasWin = [&](const std::wstring& tag)
		{
			for (const auto& l : winLangs)
				if (_wcsicmp(l.tag.c_str(), tag.c_str()) == 0) return true;
			return false;
		};
		if (!hasWin(c.winLang))
		{
			c.winLang.clear();
			const std::wstring pre = DefaultWinLangPrefix();
			for (const auto& l : winLangs)
			{
				const std::wstring& t = l.tag;
				if (_wcsnicmp(t.c_str(), pre.c_str(), pre.size()) == 0 && (t.size() == pre.size() || t[pre.size()] == L'-'))
				{
					c.winLang = t;
					break;
				}
			}
			if (c.winLang.empty() && !winLangs.empty()) c.winLang = winLangs[0].tag;
		}

		std::vector<std::wstring> keep;
		for (const auto& t : c.tessLangs)
			if (std::find(tessLangs.begin(), tessLangs.end(), t) != tessLangs.end()) keep.push_back(t);
		if (keep.empty())
			for (const auto& t : tessLangs)
				if (t == DefaultLangCode() || t == L"eng") keep.push_back(t);
		c.tessLangs = keep;
	}
}

void OcrCurrentDocument(HWND parent)
{
	if (!g_pdfDoc || g_pageCount <= 0) return;

	// 0) 이미 텍스트가 있는 페이지는 건너뜀 (기존 텍스트 위에 OCR 텍스트가 겹치지 않도록)
	std::vector<char> skipPage;
	int skipCount = 0;
	{
		std::lock_guard<std::mutex> lk(g_pdfiumMtx);
		const int n = FPDF_GetPageCount(g_pdfDoc);
		skipPage.assign(n, 0);
		for (int i = 0; i < n; ++i)
		{
			FPDF_PAGE p = FPDF_LoadPage(g_pdfDoc, i);
			if (!p) continue;
			if (HasTextLayer(p)) { skipPage[i] = 1; ++skipCount; }
			FPDF_ClosePage(p);
		}
	}
	if (skipCount > 0 && skipCount == (int)skipPage.size())
	{
		Msg(parent, S(L"모든 페이지에 이미 텍스트가 있어 OCR할 페이지가 없습니다.\n(텍스트 선택과 검색이 이미 가능한 문서입니다.)",
					  L"Every page already contains text, so there is nothing to OCR.\n(Text in this document can already be selected and searched.)",
					  L"すべてのページに既にテキストがあるため、OCR するページがありません。\n(この文書は既にテキストの選択と検索ができます。)",
					  L"所有页面都已包含文本，无需 OCR。\n(该文档已可选择和搜索文本。)",
					  L"Todas las páginas ya contienen texto, no hay nada que procesar con OCR.\n(El texto de este documento ya se puede seleccionar y buscar.)",
					  L"Toutes les pages contiennent déjà du texte, aucune page à traiter par OCR.\n(Le texte de ce document peut déjà être sélectionné et recherché.)",
					  L"Alle Seiten enthalten bereits Text, es gibt nichts zu erkennen.\n(Der Text dieses Dokuments ist bereits auswählbar und durchsuchbar.)",
					  L"Todas as páginas já contêm texto, não há nada para o OCR.\n(O texto deste documento já pode ser selecionado e pesquisado.)",
					  L"На всех страницах уже есть текст, распознавать нечего.\n(Текст этого документа уже можно выделять и искать.)"), MB_ICONINFORMATION);
		return;
	}

	// 1) 사용 가능한 엔진/언어 확인 (없는 쪽은 대화상자에서 설치 안내 표시)
	const std::vector<WinOcrLang> winLangs = WinOcr_Languages();
	const bool canSwitch = OCR_HAS_TESSERACT != 0;
	std::wstring tessdata;
	std::vector<std::wstring> tessLangs;
#if OCR_HAS_TESSERACT
	tessdata = FindTessData();
	if (!tessdata.empty()) tessLangs = ListLanguages(tessdata);
#endif

	// 2) 엔진/언어 선택 (마지막 선택 기억)
	OcrChoice choice;
	bool hasEngine = false;
	LoadOcrChoice(choice, hasEngine);
	FixupOcrChoice(choice, hasEngine, canSwitch, winLangs, tessLangs);
	if (!ChooseLanguages(parent, winLangs, tessLangs, canSwitch, choice)) return;
	SaveOcrChoice(choice);
	const bool useWin = (choice.engine == ENGINE_WIN);

	// 첫 언어가 주 언어로 쓰이므로, 보조로 섞여 쓰이는 경우가 많은 영어는 뒤로 (kor+eng 등)
	std::vector<std::wstring> chosen = choice.tessLangs;
	std::stable_partition(chosen.begin(), chosen.end(), [](const std::wstring& s) { return s != L"eng"; });
	std::wstring langArg;
	for (size_t i = 0; i < chosen.size(); ++i)
		langArg += (i ? L"+" : L"") + chosen[i];

	// 3) 저장 경로
	wchar_t outPath[MAX_PATH] = {};
	{
		std::wstring base = g_filePath;
		size_t dot = base.find_last_of(L'.');
		if (dot != std::wstring::npos) base = base.substr(0, dot);
		base += L"_ocr.pdf";
		wcsncpy_s(outPath, base.c_str(), _TRUNCATE);
	}
	OPENFILENAMEW ofn = { sizeof(ofn) };
	ofn.hwndOwner = parent;
	ofn.lpstrFilter = L"PDF (*.pdf)\0*.pdf\0";
	ofn.lpstrFile = outPath;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrDefExt = L"pdf";
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetSaveFileNameW(&ofn)) return;

	// 4) 현재 문서를 복제 (원본 유지)
	std::vector<char> docBuf;
	{
		std::lock_guard<std::mutex> lk(g_pdfiumMtx);
		if (!SerializeDoc(g_pdfDoc, docBuf)) { Msg(parent, L"Failed to copy the document.", MB_ICONERROR); return; }
	}
	FPDF_DOCUMENT doc = nullptr;
	{
		std::lock_guard<std::mutex> lk(g_pdfiumMtx);
		doc = FPDF_LoadMemDocument(docBuf.data(), (int)docBuf.size(), nullptr);
	}
	if (!doc) { Msg(parent, L"Failed to copy the document.", MB_ICONERROR); return; }

	// 폰트 (투명 텍스트용 CID 폰트)
	std::vector<uint8_t> fontData;
	wchar_t winDir[MAX_PATH] = {};
	UINT wl = GetWindowsDirectoryW(winDir, MAX_PATH);
	if (wl == 0 || wl >= MAX_PATH) wcscpy_s(winDir, L"C:\\Windows");
	if (!ReadFileBytes(std::wstring(winDir) + L"\\Fonts\\malgun.ttf", fontData))
		ReadFileBytes(std::wstring(winDir) + L"\\Fonts\\arial.ttf", fontData);
	FPDF_FONT font = nullptr;
	if (!fontData.empty())
	{
		std::lock_guard<std::mutex> lk(g_pdfiumMtx);
		font = FPDFText_LoadFont(doc, fontData.data(), (uint32_t)fontData.size(), FPDF_FONT_TRUETYPE, 1);
	}
	if (!font)
	{
		std::lock_guard<std::mutex> lk(g_pdfiumMtx);
		FPDF_CloseDocument(doc);
		Msg(parent, L"Failed to load a font for the text layer.", MB_ICONERROR);
		return;
	}

	// 5) 페이지별 OCR
	g_cancel = false;
	ShowProgress(parent);
	bool osdOk = false;
#if OCR_HAS_TESSERACT
	tesseract::TessBaseAPI api;
	if (!useWin)
	{
		if (api.Init(PathToNarrow(tessdata).c_str(), WToUtf8(langArg).c_str(), tesseract::OEM_LSTM_ONLY) != 0)
		{
			CloseProgress(parent);
			std::lock_guard<std::mutex> lk(g_pdfiumMtx);
			FPDFFont_Close(font);
			FPDF_CloseDocument(doc);
			Msg(parent, L"Failed to initialize Tesseract.", MB_ICONERROR);
			return;
		}
		api.SetPageSegMode(tesseract::PSM_AUTO);
		api.SetVariable("user_defined_dpi", "300");
		// Sauvola 적응형 이진화: 그림자, 얼룩, 누렇게 변한 종이 등 배경이 고르지 않은 스캔에 강함
		api.SetVariable("thresholding_method", "2");
	}

	// 방향 감지(OSD) - osd.traineddata가 있을 때만, legacy 엔진 필요. Windows OCR에도 사용
	if (tessdata.empty()) tessdata = FindTessData();
	tesseract::TessBaseAPI osdApi;
	if (!tessdata.empty() &&
		GetFileAttributesW((tessdata + L"\\osd.traineddata").c_str()) != INVALID_FILE_ATTRIBUTES &&
		osdApi.Init(PathToNarrow(tessdata).c_str(), "osd", tesseract::OEM_TESSERACT_ONLY) == 0)
	{
		osdApi.SetPageSegMode(tesseract::PSM_OSD_ONLY);
		osdApi.SetVariable("user_defined_dpi", "300");
		osdOk = true;
	}
#endif

	// Windows OCR은 입력 이미지 크기 상한이 있음
	int maxSide = 9000;
	if (useWin)
	{
		const unsigned md = WinOcr_MaxDimension();
		if (md > 0) maxSide = std::min(maxSide, (int)md);
	}
	const int total = FPDF_GetPageCount(doc);
	int done = 0;
	bool failed = false;

	for (int i = 0; i < total && !g_cancel; ++i)
	{
		SetProgress(i + 1, total);
		if (i < (int)skipPage.size() && skipPage[i]) continue;
		int bw = 0, bh = 0;
		std::vector<BYTE> gray;      // Tesseract 인식 / OSD용
		std::vector<uint32_t> bgra;  // Windows OCR용
		FPDF_PAGE page = nullptr;
		{
			std::lock_guard<std::mutex> lk(g_pdfiumMtx);
			page = FPDF_LoadPage(doc, i);
			if (!page) { failed = true; break; }
			double pw = FPDF_GetPageWidth(page), ph = FPDF_GetPageHeight(page);
			bw = std::max(1, (int)(pw * OCR_DPI / 72.0 + 0.5));
			bh = std::max(1, (int)(ph * OCR_DPI / 72.0 + 0.5));
			double sc = 1.0;
			if (bw > maxSide || bh > maxSide) sc = (double)maxSide / std::max(bw, bh);
			bw = std::max(1, std::min(maxSide, (int)(bw * sc)));
			bh = std::max(1, std::min(maxSide, (int)(bh * sc)));
			FPDF_BITMAP bmp = FPDFBitmap_Create(bw, bh, 0);
			if (!bmp) { FPDF_ClosePage(page); failed = true; break; }
			FPDFBitmap_FillRect(bmp, 0, 0, bw, bh, 0xFFFFFFFF);
			FPDF_RenderPageBitmap(bmp, page, 0, 0, bw, bh, 0, FPDF_ANNOT);
			const BYTE* src = (const BYTE*)FPDFBitmap_GetBuffer(bmp);
			const int stride = FPDFBitmap_GetStride(bmp);
			if (useWin)
			{
				bgra.resize((size_t)bw * bh);
				for (int y = 0; y < bh; ++y)
					memcpy(bgra.data() + (size_t)y * bw, src + (size_t)y * stride, (size_t)bw * 4);
			}
			if (!useWin || osdOk)
			{
				gray.assign((size_t)bw * bh, 255);
				for (int y = 0; y < bh; ++y)
					for (int x = 0; x < bw; ++x)
					{
						const BYTE* p = src + (size_t)y * stride + x * 4;
						// BGRA -> 휘도(BT.601). min(R,G,B)는 노란/컬러 배경을 검게 만들어 이진화를 망침
						gray[(size_t)y * bw + x] = (BYTE)((p[2] * 299 + p[1] * 587 + p[0] * 114 + 500) / 1000);
					}
			}
			FPDFBitmap_Destroy(bmp);
		}

		// 시계 방향 rot도 돌린 이미지로 선택된 엔진 실행
		auto recognize = [&](int rot, std::vector<OcrWord>& out, double& sc) -> bool
		{
			int rw = bw, rh = bh;
			if (useWin)
			{
				if (rot == 0) return RecognizeWin(choice.winLang, bgra.data(), bw, bh, out, sc);
				std::vector<uint32_t> r;
				RotatePixels(bgra, bw, bh, rot, r, rw, rh);
				return RecognizeWin(choice.winLang, r.data(), rw, rh, out, sc);
			}
#if OCR_HAS_TESSERACT
			if (rot == 0) return RecognizeWords(api, gray.data(), bw, bh, out, sc);
			std::vector<BYTE> r;
			RotatePixels(gray, bw, bh, rot, r, rw, rh);
			return RecognizeWords(api, r.data(), rw, rh, out, sc);
#else
			return false;
#endif
		};

		std::vector<OcrWord> words;
		double score = 0;
		int cw = 0;
#if OCR_HAS_TESSERACT
		if (osdOk) cw = DetectRotation(osdApi, gray.data(), bw, bh);
#endif
		bool ran = false;
		if (cw != 0 && !g_cancel)
		{
			ran = recognize(cw, words, score);
			// OSD 오판 대비: 원래 방향 결과가 더 좋으면 그쪽을 사용
			std::vector<OcrWord> w0;
			double s0 = 0;
			if (ran && !g_cancel && recognize(0, w0, s0) && s0 > score)
			{
				words.swap(w0);
				cw = 0;
			}
		}
		else if (!g_cancel)
		{
			ran = recognize(0, words, score);
		}
		if (g_cancel) { std::lock_guard<std::mutex> lk(g_pdfiumMtx); FPDF_ClosePage(page); break; }
		if (!ran) { std::lock_guard<std::mutex> lk(g_pdfiumMtx); FPDF_ClosePage(page); failed = true; break; }
		{
			std::lock_guard<std::mutex> lk(g_pdfiumMtx);
			AddInvisibleWords(doc, page, font, words, bw, bh, cw);
			FPDFPage_GenerateContent(page);
			FPDF_ClosePage(page);
		}
		++done;
	}
#if OCR_HAS_TESSERACT
	if (!useWin) api.End();
	if (osdOk) osdApi.End();
#endif
	CloseProgress(parent);

	// 6) 저장
	bool saved = false;
	if (!g_cancel && !failed)
	{
		std::vector<char> outBuf;
		{
			std::lock_guard<std::mutex> lk(g_pdfiumMtx);
			saved = SerializeDoc(doc, outBuf);
		}
		if (saved)
		{
			FILE* fp = nullptr;
			saved = (_wfopen_s(&fp, outPath, L"wb") == 0 && fp);
			if (saved)
			{
				saved = fwrite(outBuf.data(), 1, outBuf.size(), fp) == outBuf.size();
				fclose(fp);
			}
		}
	}
	{
		std::lock_guard<std::mutex> lk(g_pdfiumMtx);
		FPDFFont_Close(font);
		FPDF_CloseDocument(doc);
	}

	if (g_cancel) return;
	if (failed || !saved)
	{
		Msg(parent, S(L"OCR 처리 중 오류가 발생했습니다.", L"An error occurred during OCR.",
					  L"OCR 処理中にエラーが発生しました。", L"OCR 处理过程中出错。",
					  L"Se produjo un error durante el OCR.", L"Une erreur est survenue pendant l'OCR.",
					  L"Beim OCR ist ein Fehler aufgetreten.", L"Ocorreu um erro durante o OCR.",
					  L"Во время OCR произошла ошибка."), MB_ICONERROR);
		return;
	}
	std::wstring msg = std::wstring(S(L"OCR이 완료되었습니다.\n저장 위치: ", L"OCR completed.\nSaved to: ",
									  L"OCR が完了しました。\n保存先: ", L"OCR 已完成。\n保存位置: ",
									  L"OCR completado.\nGuardado en: ", L"OCR terminé.\nEnregistré dans : ",
									  L"OCR abgeschlossen.\nGespeichert unter: ", L"OCR concluído.\nSalvo em: ",
									  L"OCR завершён.\nСохранено в: ")) + outPath;
	if (skipCount > 0)
	{
		wchar_t buf[256];
		swprintf_s(buf, S(L"\n\n이미 텍스트가 있는 %d개 페이지는 건너뛰었습니다.", L"\n\nSkipped %d page(s) that already contain text.",
						  L"\n\n既にテキストがある %d ページはスキップしました。", L"\n\n已跳过 %d 个已包含文本的页面。",
						  L"\n\nSe omitieron %d página(s) que ya contienen texto.", L"\n\n%d page(s) contenant déjà du texte ont été ignorée(s).",
						  L"\n\n%d Seite(n) mit vorhandenem Text wurde(n) übersprungen.", L"\n\n%d página(s) que já contêm texto foram ignoradas.",
						  L"\n\nПропущено страниц с уже имеющимся текстом: %d."), skipCount);
		msg += buf;
	}
	Msg(parent, msg.c_str(), MB_ICONINFORMATION);
}
