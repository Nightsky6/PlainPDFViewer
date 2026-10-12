// PdfCompress.cpp - Ghostscript DLL 기반 PDF 압축 (AGPL-3.0)
#define NOMINMAX
#include "PdfCompress.h"
#include "Lang.h"
#include "CleanPopup.h"
#include <windows.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
	typedef int (WINAPI* PFN_new)(void** inst, void* handle);
	typedef int (WINAPI* PFN_enc)(void* inst, int enc);
	typedef int (WINAPI* PFN_stdio)(void* inst, int (*in)(void*, char*, int), int (*out)(void*, const char*, int), int (*err)(void*, const char*, int));
	typedef int (WINAPI* PFN_init)(void* inst, int argc, char** argv);
	typedef int (WINAPI* PFN_exit)(void* inst);
	typedef void (WINAPI* PFN_del)(void* inst);

	int GsIn(void*, char*, int) { return 0; }
	int GsOut(void*, const char*, int len) { return len; }

	std::string ToUtf8(const std::wstring& w)
	{
		if (w.empty()) return {};
		int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
		std::string s(n, 0);
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
		return s;
	}

#ifdef _WIN64
	const wchar_t* kDllName = L"gsdll64.dll";
#else
	const wchar_t* kDllName = L"gsdll32.dll";
#endif

	// 앱 폴더 -> 설치된 Ghostscript(레지스트리) 순으로 DLL 탐색
	HMODULE LoadGs()
	{
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		PathRemoveFileSpecW(exe);
		std::wstring local = std::wstring(exe) + L"\\" + kDllName;
		if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			HMODULE m = LoadLibraryExW(local.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (m) return m;
		}

		const wchar_t* roots[] = { L"SOFTWARE\\GPL Ghostscript", L"SOFTWARE\\Artifex Ghostscript", L"SOFTWARE\\Artifex\\GPL Ghostscript" };
		REGSAM views[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
		HMODULE best = nullptr;
		for (auto root : roots)
		for (auto view : views)
		{
			HKEY hk;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ | view, &hk) != ERROR_SUCCESS) continue;
			wchar_t ver[64];
			for (DWORD i = 0; ; ++i)
			{
				DWORD len = 64;
				if (RegEnumKeyExW(hk, i, ver, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
				HKEY hv;
				if (RegOpenKeyExW(hk, ver, 0, KEY_READ | view, &hv) != ERROR_SUCCESS) continue;
				wchar_t path[MAX_PATH] = {};
				DWORD sz = sizeof(path);
				if (RegQueryValueExW(hv, nullptr, nullptr, nullptr, (LPBYTE)path, &sz) == ERROR_SUCCESS && path[0])
				{
					std::wstring cand[] = { std::wstring(path), std::wstring(path) + L"\\bin\\" + kDllName, std::wstring(path) + L"\\" + kDllName };
					for (auto& c : cand)
					{
						DWORD a = GetFileAttributesW(c.c_str());
						if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
						HMODULE m = LoadLibraryExW(c.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
						if (m) { best = m; break; }
					}
				}
				RegCloseKey(hv);
			}
			RegCloseKey(hk);
			if (best) return best;
		}
		return nullptr;
	}

	struct Job
	{
		std::wstring in, out;
		int level = 1;
		int dpi = 150;
		int rc = -1;
		bool noDll = false;
	};

	DWORD WINAPI Worker(LPVOID p)
	{
		Job* j = (Job*)p;
		HMODULE m = LoadGs();
		if (!m) { j->noDll = true; return 0; }

		auto fNew  = (PFN_new)GetProcAddress(m, "gsapi_new_instance");
		auto fEnc  = (PFN_enc)GetProcAddress(m, "gsapi_set_arg_encoding");
		auto fStd  = (PFN_stdio)GetProcAddress(m, "gsapi_set_stdio");
		auto fInit = (PFN_init)GetProcAddress(m, "gsapi_init_with_args");
		auto fExit = (PFN_exit)GetProcAddress(m, "gsapi_exit");
		auto fDel  = (PFN_del)GetProcAddress(m, "gsapi_delete_instance");
		if (!fNew || !fInit || !fExit || !fDel) { j->noDll = true; FreeLibrary(m); return 0; }

		void* inst = nullptr;
		if (fNew(&inst, nullptr) < 0) { FreeLibrary(m); return 0; }
		if (fStd) fStd(inst, GsIn, GsOut, GsOut);
		if (fEnc) fEnc(inst, 1); // GS_ARG_ENCODING_UTF8

		static const char* presets[] = { "/screen", "/ebook", "/printer" };
		std::vector<std::string> a;
		a.push_back("gs");
		a.push_back("-dNOPAUSE");
		a.push_back("-dBATCH");
		a.push_back("-dQUIET");
		a.push_back("-dSAFER");
		a.push_back("-sDEVICE=pdfwrite");
		a.push_back("-dCompatibilityLevel=1.6");
		a.push_back(std::string("-dPDFSETTINGS=") + presets[j->level]);
		a.push_back("-dDetectDuplicateImages=true");
		a.push_back("-dCompressFonts=true");
		a.push_back("-dSubsetFonts=true");
		a.push_back("-sOutputFile=" + ToUtf8(j->out));
		a.push_back("-f");
		a.push_back(ToUtf8(j->in));

		std::vector<char*> argv;
		for (auto& s : a) argv.push_back(s.data());

		int r = fInit(inst, (int)argv.size(), argv.data());
		fExit(inst);
		fDel(inst);
		FreeLibrary(m);
		j->rc = (r == 0 || r == -101) ? 0 : r; // -101: gs_error_Quit
		return 0;
	}

	long long FileSize(const std::wstring& p)
	{
		WIN32_FILE_ATTRIBUTE_DATA d;
		if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
		return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
	}

	std::wstring FmtSize(long long b)
	{
		wchar_t buf[64];
		if (b >= 1024 * 1024) swprintf_s(buf, L"%.2f MB", b / 1048576.0);
		else swprintf_s(buf, L"%.1f KB", b / 1024.0);
		return buf;
	}

	const wchar_t* Title()
	{
		return (::LS)(L"PDF \uC555\uCD95", L"Compress PDF", L"PDF\u3092\u5727\u7E2E", L"\u538B\u7F29 PDF",
				  L"Comprimir PDF", L"Compresser le PDF", L"PDF komprimieren", L"Comprimir PDF", L"\u0421\u0436\u0430\u0442\u044C PDF");
	}
}

void ShowCompressPdfDialog(HWND parent, const std::wstring& inPath)
{
#if defined(_M_ARM64)
	CleanPopup::Show(parent, Title(),
		(::LS)(L"ARM64 \uBC84\uC804\uC5D0\uC11C\uB294 PDF \uC555\uCD95 \uAE30\uB2A5\uC744 \uC9C0\uC6D0\uD558\uC9C0 \uC54A\uC2B5\uB2C8\uB2E4.", L"PDF compression is not supported on ARM64.",
			L"ARM64\u7248\u3067\u306FPDF\u5727\u7E2E\u306F\u30B5\u30DD\u30FC\u30C8\u3055\u308C\u3066\u3044\u307E\u305B\u3093\u3002", L"ARM64 \u7248\u672C\u4E0D\u652F\u6301 PDF \u538B\u7F29\u3002",
			L"La compresi\u00F3n de PDF no es compatible con ARM64.", L"La compression PDF n\u0027est pas prise en charge sur ARM64.", L"Die PDF-Komprimierung wird unter ARM64 nicht unterst\u00FCtzt.",
			L"A compress\u00E3o de PDF n\u00E3o \u00E9 compat\u00EDvel com ARM64.", L"\u0421\u0436\u0430\u0442\u0438\u0435 PDF \u043D\u0435 \u043F\u043E\u0434\u0434\u0435\u0440\u0436\u0438\u0432\u0430\u0435\u0442\u0441\u044F \u0432 ARM64."), false);
	return;
#endif
	if (inPath.empty() || GetFileAttributesW(inPath.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		CleanPopup::Show(parent, Title(),
			(::LS)(L"\uBA3C\uC800 \uD30C\uC77C\uC744 \uC800\uC7A5\uD574 \uC8FC\uC138\uC694.", L"Please save the file first.",
			   L"\u5148\u306B\u30D5\u30A1\u30A4\u30EB\u3092\u4FDD\u5B58\u3057\u3066\u304F\u3060\u3055\u3044\u3002", L"\u8BF7\u5148\u4FDD\u5B58\u6587\u4EF6\u3002",
			   L"Guarde primero el archivo.", L"Enregistrez d'abord le fichier.", L"Bitte speichern Sie die Datei zuerst.",
			   L"Salve o arquivo primeiro.", L"\u0421\u043D\u0430\u0447\u0430\u043B\u0430 \u0441\u043E\u0445\u0440\u0430\u043D\u0438\u0442\u0435 \u0444\u0430\u0439\u043B."), false);
		return;
	}

	// 1) 압축 수준 선택
	const wchar_t* t0 = (::LS)(L"\uCD5C\uB300 \uC555\uCD95 (\uD654\uBA74\uC6A9, 72dpi)", L"Maximum compression (screen, 72dpi)", L"\u6700\u5927\u5727\u7E2E (\u753B\u9762\u7528, 72dpi)", L"\u6700\u5927\u538B\u7F29 (\u5C4F\u5E55, 72dpi)",
						   L"M\u00E1xima compresi\u00F3n (pantalla, 72dpi)", L"Compression maximale (\u00E9cran, 72dpi)", L"Maximale Komprimierung (Bildschirm, 72dpi)", L"M\u00E1xima compress\u00E3o (tela, 72dpi)", L"\u041C\u0430\u043A\u0441\u0438\u043C\u0443\u043C (\u044D\u043A\u0440\u0430\u043D, 72dpi)");
	const wchar_t* t1 = (::LS)(L"\uADE0\uD615 (\uC804\uC790\uCC45, 150dpi)", L"Balanced (ebook, 150dpi)", L"\u30D0\u30E9\u30F3\u30B9 (\u96FB\u5B50\u66F8\u7C4D, 150dpi)", L"\u5E73\u8861 (\u7535\u5B50\u4E66, 150dpi)",
						   L"Equilibrado (ebook, 150dpi)", L"\u00C9quilibr\u00E9 (ebook, 150dpi)", L"Ausgewogen (E-Book, 150dpi)", L"Equilibrado (ebook, 150dpi)", L"\u0411\u0430\u043B\u0430\u043D\u0441 (\u044D-\u043A\u043D\u0438\u0433\u0430, 150dpi)");
	const wchar_t* t2 = (::LS)(L"\uACE0\uD488\uC9C8 (\uC778\uC1C4\uC6A9, 300dpi)", L"High quality (print, 300dpi)", L"\u9AD8\u54C1\u8CEA (\u5370\u5237\u7528, 300dpi)", L"\u9AD8\u8D28\u91CF (\u6253\u5370, 300dpi)",
						   L"Alta calidad (impresi\u00F3n, 300dpi)", L"Haute qualit\u00E9 (impression, 300dpi)", L"Hohe Qualit\u00E4t (Druck, 300dpi)", L"Alta qualidade (impress\u00E3o, 300dpi)", L"\u0412\u044B\u0441\u043E\u043A\u043E\u0435 (\u043F\u0435\u0447\u0430\u0442\u044C, 300dpi)");

	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, 1001, t0);
	AppendMenuW(menu, MF_STRING, 1002, t1);
	AppendMenuW(menu, MF_STRING, 1003, t2);
	POINT pt; GetCursorPos(&pt);
	RECT wr; if (GetWindowRect(parent, &wr) && !PtInRect(&wr, pt)) { pt.x = (wr.left + wr.right) / 2; pt.y = (wr.top + wr.bottom) / 2; }
	SetForegroundWindow(parent);
	int sel = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, parent, nullptr);
	DestroyMenu(menu);
	if (sel < 1001 || sel > 1003) return;

	// 2) 저장 위치
	std::wstring def = inPath;
	size_t dot = def.find_last_of(L'.');
	if (dot != std::wstring::npos) def.insert(dot, L"_compressed");
	else def += L"_compressed.pdf";
	wchar_t outBuf[MAX_PATH * 2] = {};
	wcsncpy_s(outBuf, def.c_str(), _TRUNCATE);
	OPENFILENAMEW ofn = { sizeof(ofn) };
	ofn.hwndOwner = parent;
	ofn.lpstrFilter = L"PDF (*.pdf)\0*.pdf\0\0";
	ofn.lpstrFile = outBuf;
	ofn.nMaxFile = MAX_PATH * 2;
	ofn.lpstrDefExt = L"pdf";
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (!GetSaveFileNameW(&ofn)) return;

	if (_wcsicmp(outBuf, inPath.c_str()) == 0)
	{
		CleanPopup::Show(parent, Title(),
			(::LS)(L"\uC6D0\uBCF8\uACFC \uB2E4\uB978 \uD30C\uC77C\uC5D0 \uC800\uC7A5\uD574 \uC8FC\uC138\uC694.", L"Save to a different file than the original.",
			   L"\u5143\u306E\u30D5\u30A1\u30A4\u30EB\u3068\u306F\u5225\u306B\u4FDD\u5B58\u3057\u3066\u304F\u3060\u3055\u3044\u3002", L"\u8BF7\u4FDD\u5B58\u4E3A\u4E0E\u539F\u6587\u4EF6\u4E0D\u540C\u7684\u6587\u4EF6\u3002",
			   L"Guarde en un archivo distinto del original.", L"Enregistrez dans un fichier diff\u00E9rent de l'original.", L"Speichern Sie in eine andere Datei als das Original.",
			   L"Salve em um arquivo diferente do original.", L"\u0421\u043E\u0445\u0440\u0430\u043D\u0438\u0442\u0435 \u0432 \u0434\u0440\u0443\u0433\u043E\u0439 \u0444\u0430\u0439\u043B."), false);
		return;
	}

	// 3) 백그라운드 실행 (메시지 펌프 유지)
	Job job;
	job.in = inPath;
	job.out = outBuf;
	job.level = sel - 1001;
	HANDLE th = CreateThread(nullptr, 0, Worker, &job, 0, nullptr);
	if (!th) return;

	EnableWindow(parent, FALSE);
	HCURSOR oldCur = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
	while (true)
	{
		DWORD r = MsgWaitForMultipleObjects(1, &th, FALSE, INFINITE, QS_ALLINPUT);
		if (r == WAIT_OBJECT_0) break;
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); break; }
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	CloseHandle(th);
	SetCursor(oldCur);
	EnableWindow(parent, TRUE);
	SetForegroundWindow(parent);

	if (job.noDll)
	{
		CleanPopup::Show(parent, Title(),
			(::LS)(L"Ghostscript DLL(gsdll64.dll)\uC744 \uCC3E\uC744 \uC218 \uC5C6\uC2B5\uB2C8\uB2E4.\n\uC571 \uD3F4\uB354\uC5D0 \uB450\uAC70\uB098 Ghostscript\uB97C \uC124\uCE58\uD574 \uC8FC\uC138\uC694.",
			   L"Ghostscript DLL (gsdll64.dll) was not found.\nPlace it in the app folder or install Ghostscript.",
			   L"Ghostscript DLL\u304C\u898B\u3064\u304B\u308A\u307E\u305B\u3093\u3002\n\u30A2\u30D7\u30EA\u30D5\u30A9\u30EB\u30C0\u30FC\u306B\u7F6E\u304F\u304B\u3001Ghostscript\u3092\u30A4\u30F3\u30B9\u30C8\u30FC\u30EB\u3057\u3066\u304F\u3060\u3055\u3044\u3002",
			   L"\u627E\u4E0D\u5230 Ghostscript DLL\u3002\n\u8BF7\u653E\u5165\u5E94\u7528\u6587\u4EF6\u5939\u6216\u5B89\u88C5 Ghostscript\u3002",
			   L"No se encontr\u00F3 la DLL de Ghostscript.\nC\u00F3piela en la carpeta de la app o instale Ghostscript.",
			   L"DLL Ghostscript introuvable.\nPlacez-la dans le dossier de l'application ou installez Ghostscript.",
			   L"Ghostscript-DLL nicht gefunden.\nLegen Sie sie in den App-Ordner oder installieren Sie Ghostscript.",
			   L"DLL do Ghostscript n\u00E3o encontrada.\nColoque-a na pasta do app ou instale o Ghostscript.",
			   L"DLL Ghostscript \u043D\u0435 \u043D\u0430\u0439\u0434\u0435\u043D\u0430.\n\u041F\u043E\u043C\u0435\u0441\u0442\u0438\u0442\u0435 \u0435\u0451 \u0432 \u043F\u0430\u043F\u043A\u0443 \u043F\u0440\u0438\u043B\u043E\u0436\u0435\u043D\u0438\u044F \u0438\u043B\u0438 \u0443\u0441\u0442\u0430\u043D\u043E\u0432\u0438\u0442\u0435 Ghostscript."), false);
		return;
	}

	long long a = FileSize(job.in), b = FileSize(job.out);
	if (job.rc != 0 || b <= 0)
	{
		DeleteFileW(job.out.c_str());
		CleanPopup::Show(parent, Title(),
			(::LS)(L"\uC555\uCD95\uC5D0 \uC2E4\uD328\uD588\uC2B5\uB2C8\uB2E4.", L"Compression failed.", L"\u5727\u7E2E\u306B\u5931\u6557\u3057\u307E\u3057\u305F\u3002", L"\u538B\u7F29\u5931\u8D25\u3002",
			   L"La compresi\u00F3n fall\u00F3.", L"\u00C9chec de la compression.", L"Komprimierung fehlgeschlagen.", L"Falha na compress\u00E3o.", L"\u041E\u0448\u0438\u0431\u043A\u0430 \u0441\u0436\u0430\u0442\u0438\u044F."), false);
		return;
	}

	wchar_t msg[512];
	int pct = a > 0 ? (int)((a - b) * 100 / a) : 0;
	swprintf_s(msg, L"%s  \u2192  %s  (%d%%)", FmtSize(a).c_str(), FmtSize(b).c_str(), -pct);
	CleanPopup::Show(parent, Title(), msg, false);
}
