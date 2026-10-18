// ---------------------------------------------------------------
// OcrPdf.cpp - PDFium 렌더링 + tesseract.exe 로 검색 가능한 PDF 생성
// ---------------------------------------------------------------
#include "framework.h"
#include "PlainPDFViewer.h"
#include "OcrPdf.h"
#include "Lang.h"
#include "CleanPopup.h"
#include <fpdf_edit.h>
#include <fpdf_save.h>
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
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#if !defined(_M_ARM64)
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>
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

#if defined(_M_ARM64)
// ARM64: Tesseract(vcpkg) 미지원 - OCR 비활성
void OcrCurrentDocument(HWND parent)
{
    MessageBoxW(parent, L"OCR is not supported on ARM64 builds.", L"OCR", MB_OK | MB_ICONINFORMATION);
}
#else
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

	bool RecognizeWords(tesseract::TessBaseAPI& api, const uint8_t* gray, int w, int h, std::vector<OcrWord>& words)
	{
		api.SetImage(gray, w, h, 1, w);
		std::atomic<bool> done{ false };
		bool ok = false;
		std::thread th([&] { ok = (api.Recognize(nullptr) == 0); done = true; });
		while (!done)
		{
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
			PumpMessages();
		}
		th.join();
		if (!ok) { api.Clear(); return false; }

		std::unique_ptr<tesseract::ResultIterator> it(api.GetIterator());
		if (it)
		{
			do
			{
				std::unique_ptr<char[]> t(it->GetUTF8Text(tesseract::RIL_WORD));
				if (!t) continue;
				const float conf = it->Confidence(tesseract::RIL_WORD);
								if (conf < 60.f) continue;
				int l, tp, r, b;
				if (!it->BoundingBox(tesseract::RIL_WORD, &l, &tp, &r, &b)) continue;
				std::wstring txt = Utf8ToW(t.get());
				while (!txt.empty() && (txt.back() == L' ' || txt.back() == L'\t' || txt.back() == L'\n' || txt.back() == L'\r')) txt.pop_back();
				if (txt.empty() || r <= l || b <= tp) continue;
								{
									size_t good = 0;
									for (wchar_t c : txt)
										if (iswalnum(c) || (c >= 0xAC00 && c <= 0xD7A3)) ++good;
									if (good == 0 || good * 10 < txt.size() * 6) continue;
									if (txt.size() == 1 && conf < 85.f) continue;
									if ((b - tp) > 3 * (r - l) + 60 && txt.size() > 1) continue;
								}
				OcrWord wd;
				wd.left = l; wd.top = tp; wd.w = r - l; wd.h = b - tp;
				wd.text = std::move(txt);
				words.push_back(std::move(wd));
			} while (it->Next(tesseract::RIL_WORD));
		}
		api.Clear();
		return true;
	}

	// 보이지 않는 텍스트로 단어 추가
	void AddInvisibleWords(FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_FONT font,
						   const std::vector<OcrWord>& words, int bmpW, int bmpH)
	{
		for (const auto& wd : words)
		{
			double x0, y0, x1, y1;
			FPDF_DeviceToPage(page, 0, 0, bmpW, bmpH, 0, wd.left, wd.top + wd.h, &x0, &y0);
			FPDF_DeviceToPage(page, 0, 0, bmpW, bmpH, 0, wd.left + wd.w, wd.top, &x1, &y1);
			const double bw = x1 - x0, bh = y1 - y0;
			if (bw <= 0 || bh <= 0) continue;

			FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, font, (float)bh);
			if (!obj) continue;
			FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(wd.text.c_str()));
			FPDFTextObj_SetTextRenderMode(obj, FPDF_TEXTRENDERMODE_INVISIBLE);

			float l = 0, b = 0, r = 0, t = 0;
			if (FPDFPageObj_GetBounds(obj, &l, &b, &r, &t) && r - l > 0.01f)
			{
				const double sx = bw / (r - l);
				FPDFPageObj_Transform(obj, sx, 0, 0, 1, x0 - l * sx, y0 - b);
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
}

void OcrCurrentDocument(HWND parent)
{
	if (!g_pdfDoc || g_pageCount <= 0) return;

	// 1) tessdata 확인
	const std::wstring tessdata = FindTessData();
	std::vector<std::wstring> langs;
	if (!tessdata.empty()) langs = ListLanguages(tessdata);
	if (langs.empty())
	{
		wchar_t exeDir[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
		Msg(parent, (std::wstring(S(L"OCR 언어 데이터(*.traineddata)를 찾을 수 없습니다.\n프로그램 폴더의 tessdata 폴더에 언어 파일을 넣어 주세요.",
					  L"OCR language data (*.traineddata) was not found.\nPlease put language files in the 'tessdata' folder next to the program.",
					  L"OCR 言語データ (*.traineddata) が見つかりません。\nプログラムフォルダの tessdata フォルダに言語ファイルを置いてください。",
					  L"未找到 OCR 语言数据 (*.traineddata)。\n请将语言文件放入程序目录下的 tessdata 文件夹。",
					  L"No se encontraron los datos de idioma OCR (*.traineddata).\nColoque los archivos en la carpeta 'tessdata' junto al programa.",
					  L"Données de langue OCR (*.traineddata) introuvables.\nPlacez les fichiers dans le dossier 'tessdata' à côté du programme.",
					  L"OCR-Sprachdaten (*.traineddata) wurden nicht gefunden.\nLegen Sie die Dateien in den Ordner 'tessdata' neben dem Programm ab.",
					  L"Dados de idioma OCR (*.traineddata) não encontrados.\nColoque os arquivos na pasta 'tessdata' ao lado do programa.",
					  L"Языковые данные OCR (*.traineddata) не найдены.\nПоместите файлы в папку 'tessdata' рядом с программой.")) + L"\n\n" + exeDir).c_str(), MB_ICONERROR);
		return;
	}

	// 2) 언어 선택
	std::vector<std::wstring> chosen;
	if (!ChooseLanguages(parent, langs, chosen)) return;
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
	tesseract::TessBaseAPI api;
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
	const int total = FPDF_GetPageCount(doc);
	int done = 0;
	bool failed = false;

	for (int i = 0; i < total && !g_cancel; ++i)
	{
		SetProgress(i + 1, total);
		int bw = 0, bh = 0;
		std::vector<BYTE> gray;
		FPDF_PAGE page = nullptr;
		{
			std::lock_guard<std::mutex> lk(g_pdfiumMtx);
			page = FPDF_LoadPage(doc, i);
			if (!page) { failed = true; break; }
			double pw = FPDF_GetPageWidth(page), ph = FPDF_GetPageHeight(page);
			bw = std::max(1, (int)(pw * OCR_DPI / 72.0 + 0.5));
			bh = std::max(1, (int)(ph * OCR_DPI / 72.0 + 0.5));
			double sc = 1.0;
			if (bw > 9000 || bh > 9000) sc = 9000.0 / std::max(bw, bh);
			bw = std::max(1, (int)(bw * sc));
			bh = std::max(1, (int)(bh * sc));
			FPDF_BITMAP bmp = FPDFBitmap_Create(bw, bh, 0);
			if (!bmp) { FPDF_ClosePage(page); failed = true; break; }
			FPDFBitmap_FillRect(bmp, 0, 0, bw, bh, 0xFFFFFFFF);
			FPDF_RenderPageBitmap(bmp, page, 0, 0, bw, bh, 0, FPDF_ANNOT);
			gray.assign((size_t)bw * bh, 255);
			const BYTE* src = (const BYTE*)FPDFBitmap_GetBuffer(bmp);
			const int stride = FPDFBitmap_GetStride(bmp);
			for (int y = 0; y < bh; ++y)
				for (int x = 0; x < bw; ++x)
				{
					const BYTE* p = src + (size_t)y * stride + x * 4;
					gray[(size_t)y * bw + x] = (BYTE)std::min({ (int)p[0], (int)p[1], (int)p[2] });
				}
			FPDFBitmap_Destroy(bmp);
		}

		std::vector<OcrWord> words;
		bool ran = RecognizeWords(api, gray.data(), bw, bh, words);
		if (g_cancel) { std::lock_guard<std::mutex> lk(g_pdfiumMtx); FPDF_ClosePage(page); break; }
		if (!ran) { std::lock_guard<std::mutex> lk(g_pdfiumMtx); FPDF_ClosePage(page); failed = true; break; }
		{
			std::lock_guard<std::mutex> lk(g_pdfiumMtx);
			AddInvisibleWords(doc, page, font, words, bw, bh);
			FPDFPage_GenerateContent(page);
			FPDF_ClosePage(page);
		}
		++done;
	}
	api.End();
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
	Msg(parent, msg.c_str(), MB_ICONINFORMATION);
}
#endif // !_M_ARM64
