#pragma once
// ---------------------------------------------------------------
// GroupPanel - 오른쪽 그룹 패널
// ---------------------------------------------------------------
#include <windows.h>
#include <string>
#include <vector>

// ── 상수 ─────────────────────────────────────────────────────────
#define GROUP_PANEL_WIDTH   260
#define GROUP_TAB_H          36   // 그룹 탭 행 높이
#define GROUP_HEADER_H       44   // 패널 헤더 높이
#define GROUP_ITEM_H         36   // 항목 한 줄 높이
#define GROUP_BTN_AREA_H     36   // 항목 확장 버튼 영역 높이

// ── 데이터 구조 ──────────────────────────────────────────────────
struct GroupItem {
    std::wstring name;      // 항목 이름
    std::wstring pdfPath;   // 등록된 PDF 경로
    std::wstring memo;      // 메모 텍스트
};

struct GroupDef {
    std::wstring            name;   // 그룹 이름
    std::vector<GroupItem>  items;  // 항목 목록
};

// ── API ──────────────────────────────────────────────────────────
namespace GroupPanel
{
    void Toggle(HWND hParent);
    bool IsVisible();

    // 그룹/항목 관리 팝업
    void ShowManageDialog(HWND parent);

    void Draw(HDC hdc, const RECT& clientRect);
    void OnLButtonDown(HWND hWnd, int x, int y, const RECT& clientRect);
    void OnLButtonUp  (HWND hWnd, int x, int y, const RECT& clientRect);
    void OnMouseMove  (HWND hWnd, int x, int y, const RECT& clientRect);
    void OnMouseLeave ();
    void OnMouseWheel (int delta);

    RECT GetPanelRect(const RECT& clientRect);

    void SaveGroups();
    void LoadGroups();
}
