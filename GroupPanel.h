#pragma once
// ---------------------------------------------------------------
// GroupPanel - 오른쪽 그룹 패널
// ---------------------------------------------------------------
#include <windows.h>
#include <string>
#include <vector>

// ── 상수 ─────────────────────────────────────────────────────────
#define GROUP_PANEL_WIDTH   260
#define GROUP_HEADER_H       36   // 패널 헤더 높이 (사이드바 SIDEBAR_HEADER_H와 동일)
#define GROUP_TAB_H          44   // 그룹 선택 상자 행 높이
#define GROUP_ITEM_H         34   // 항목 한 줄 높이
#define GROUP_FOOTER_H       38   // 하단 [+ 항목 추가] 버튼 영역 높이
#define GROUP_DROP_ROW_H     32   // 드롭다운 목록 한 줄 높이

// ── 데이터 구조 ──────────────────────────────────────────────────
struct GroupItem {
    std::wstring name;      // 항목 이름
    std::wstring pdfPath;   // 등록된 PDF 경로
    std::wstring memo;      // 메모 텍스트
    bool missing = false;   // 등록된 PDF가 디스크에 없음 (실행 중 캐시, 저장 안 함)
};

struct GroupDef {
    std::wstring            name;   // 그룹 이름
    std::vector<GroupItem>  items;  // 항목 목록
};

// ── API ──────────────────────────────────────────────────────────
namespace GroupPanel
{
    void Toggle(HWND hParent);      // 열기/닫기 (슬라이드 애니메이션)
    bool IsVisible();               // 레이아웃상 열려 있는지 (애니메이션 중에도 목표 상태)
    bool IsAnimating();             // 슬라이드 애니메이션 진행 중

    // 그룹/항목 관리 팝업
    void ShowManageDialog(HWND parent);

    void Draw(HDC hdc, const RECT& clientRect);
    // 패널이 클릭을 처리했으면 true → 호출 측은 페이지 쪽 처리를 건너뛴다
    bool OnLButtonDown(HWND hWnd, int x, int y, const RECT& clientRect);
    bool OnLButtonUp  (HWND hWnd, int x, int y, const RECT& clientRect);
    void OnMouseMove  (HWND hWnd, int x, int y, const RECT& clientRect);
    void OnMouseLeave (HWND hWnd);
    void OnMouseWheel (int delta, const RECT& clientRect);

    // 패널이 보이고 (x,y)가 패널 위에 있으면 true
    bool HitTest(int x, int y, const RECT& clientRect);

    // 우클릭: 그룹(드롭다운 버튼/목록) 또는 항목 줄 위면 삭제 컨텍스트 메뉴 표시.
    // 패널 위 클릭이면 true → 호출 측은 페이지 컨텍스트 메뉴를 띄우지 않는다
    bool OnRButtonDown(HWND hWnd, int x, int y, const RECT& clientRect);

    // 패널 위면 알맞은 커서를 설정하고 true (WM_SETCURSOR에서 호출)
    bool OnSetCursor(int x, int y, const RECT& clientRect);

    RECT GetPanelRect(const RECT& clientRect);

    void SaveGroups();
    void LoadGroups();
}
