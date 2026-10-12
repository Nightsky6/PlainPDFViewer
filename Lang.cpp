// Lang.cpp — 언어 설정 저장/로드
#include "framework.h"
#include "Lang.h"

AppLang g_appLang = AppLang::Korean;

static const wchar_t* REG_KEY = L"Software\\PlainPDFViewer";
static const wchar_t* REG_VAL = L"Language";

void Lang_Init()
{
	HKEY hKey;
	LONG openRes = RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey);
	wchar_t dbg[128];
	swprintf_s(dbg, L"[Lang] RegOpenKey result=%ld\n", openRes);
	OutputDebugStringW(dbg);
	if (openRes == ERROR_SUCCESS)
	{
		DWORD val = 0, sz = sizeof(DWORD);
		LONG qRes = RegQueryValueExW(hKey, REG_VAL, nullptr, nullptr,
							 reinterpret_cast<LPBYTE>(&val), &sz);
		swprintf_s(dbg, L"[Lang] RegQuery result=%ld val=%lu\n", qRes, val);
		OutputDebugStringW(dbg);
		if (qRes == ERROR_SUCCESS)
		{
			if (val == 1)      g_appLang = AppLang::English;
			else if (val == 2) g_appLang = AppLang::Japanese;
			else if (val == 3) g_appLang = AppLang::Chinese;
			else if (val == 4) g_appLang = AppLang::Spanish;
			else if (val == 5) g_appLang = AppLang::French;
			else if (val == 6) g_appLang = AppLang::German;
			else if (val == 7) g_appLang = AppLang::Portuguese;
			else if (val == 8) g_appLang = AppLang::Russian;
			else               g_appLang = AppLang::Korean;
		}
		RegCloseKey(hKey);
	}
	swprintf_s(dbg, L"[Lang] g_appLang=%d\n", (int)g_appLang);
	OutputDebugStringW(dbg);
}

void Lang_Set(AppLang lang)
{
	g_appLang = lang;
	HKEY hKey;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
						REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
	{
		DWORD val = static_cast<DWORD>(lang);
		RegSetValueExW(hKey, REG_VAL, 0, REG_DWORD,
					   reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
		RegCloseKey(hKey);
	}
}
