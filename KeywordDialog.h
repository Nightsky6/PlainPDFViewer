#pragma once
#include "framework.h"
#include <string>
#include <vector>

struct KeywordEntry {
    std::wstring keyword;
    COLORREF     color;
};

extern std::vector<KeywordEntry> g_keywordList;

void ShowKeywordDialog(HWND hParent);
void ApplyKeywordHighlights(HWND hWnd);
void RemoveKeywordHighlights();
