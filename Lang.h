// Lang.h — 런타임 언어 선택
#pragma once
#include <windows.h>
#include "Strings.h"

enum class AppLang { Korean = 0, English = 1, Japanese = 2, Chinese = 3, Spanish = 4, French = 5, German = 6, Portuguese = 7, Russian = 8 };

extern AppLang g_appLang;

void Lang_Init();
void Lang_Set(AppLang lang);

inline const wchar_t* LS(const wchar_t* ko, const wchar_t* en, const wchar_t* ja,
						 const wchar_t* zh, const wchar_t* es, const wchar_t* fr,
						 const wchar_t* de, const wchar_t* pt, const wchar_t* ru)
{
	switch (g_appLang)
	{
	case AppLang::English:    return en;
	case AppLang::Japanese:   return ja;
	case AppLang::Chinese:    return zh;
	case AppLang::Spanish:    return es;
	case AppLang::French:     return fr;
	case AppLang::German:     return de;
	case AppLang::Portuguese: return pt;
	case AppLang::Russian:    return ru;
	default:                  return ko;
	}
}

#define LS(id) (::LS(id##_KO, id##_EN, id##_JA, id##_ZH, id##_ES, id##_FR, id##_DE, id##_PT, id##_RU))
