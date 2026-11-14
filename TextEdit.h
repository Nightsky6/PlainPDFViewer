// TextEdit.h : 텍스트 편집 모드
//
// 편집 모드를 켜면 페이지의 텍스트 오브젝트마다 점선 상자를 그리고,
// 상자를 클릭하면 그 자리에서 글자를 고칠 수 있다.
// 고친 내용은 저장 전까지 화면 오버레이로만 보이고, 저장할 때 PDF 페이지 내용에 반영된다.

#pragma once

#include <windows.h>
#include <fpdfview.h>

namespace TextEdit
{
    bool IsActive();
    void SetActive(HWND hWnd, bool active);

    // 문서를 닫기 직전에 호출 — 그 문서의 캐시와 저장하지 않은 편집을 버린다
    void ForgetDocument(FPDF_DOCUMENT doc);

    // 페이지 위에 편집 미리보기와 (편집 모드일 때) 점선 상자를 그린다
    void Draw(HDC hdc, const RECT& clientRect);

    // 메인 창 WM_PAINT에서 BeginPaint 직후 호출: 입력창과 서식 막대 영역을 그리기에서 뺀다
    void ExcludeEditorFromPaint(HWND hWnd, HDC hdc);

    // (x, y)가 편집 가능한 상자 위인지
    bool HitTest(int x, int y);

    // 마우스 이동: 호버 대상이 바뀌면 다시 그린다
    void OnMouseMove(HWND hWnd, int x, int y);

    // 클릭: 상자를 누르면 편집을 시작한다.
    // 편집 모드에서 뷰포트 안을 누른 경우 true를 돌려준다 (다른 마우스 처리를 막음)
    bool OnLButtonDown(HWND hWnd, int x, int y);

    // 우클릭: 상자 위면 편집/삭제/되돌리기 메뉴를 띄우고 true를 돌려준다
    // (ptScreen은 메뉴를 띄울 화면 좌표)
    bool OnContextMenu(HWND hWnd, int x, int y, POINT ptScreen);

    // 열려 있는 입력창의 내용을 확정한다
    void CommitEdit();

    // 입력창에 키보드 포커스가 있는지 (메시지 루프에서 단축키를 건너뛰는 데 쓴다)
    bool IsEditFocused();

    bool HasPendingEdits(FPDF_DOCUMENT doc);

    // 저장 직전에 호출: 저장하지 않은 편집을 doc의 페이지 내용에 반영한다.
    // 반영한 편집 개수를 돌려준다. g_pdfiumMtx를 잡지 않은 상태에서 호출해야 한다.
    int ApplyPendingEdits(FPDF_DOCUMENT doc);
}
