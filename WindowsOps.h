#pragma once
#include "framework.h"
#include "PlainPDFViewer.h"
#include "Toolbar.h"
#include <string>
#include <vector>

// ── ColorMenuItem (AppendColorMenuItem용) ────────────────────────────────────
struct ColorMenuItem
{
    COLORREF     color;
    std::wstring label;
};
extern std::vector<ColorMenuItem> g_colorMenuItems;

// ── WindowsOps 함수 선언 ──────────────────────────────────────────────────────
// (구현은 PlainPDFViewer.cpp에 있으며, PlainPDFViewer.h를 통해 공유됨)
// 이 헤더는 WindowsOps 관련 타입/상수를 다른 모듈에서 참조할 때 사용합니다.
