#pragma once
// ---------------------------------------------------------------
// SelToolbar - 텍스트 선택 직후 마우스 근처에 뜨는 작은 주석 도구 모음
//
// 사용법:
//   SelToolbar::Show(hOwner, ptScreen, palette, count, OnAction)
//   SelToolbar::Hide()
// 버튼을 누르면 OnAction(owner, action, color)이 호출된다.
// 도구 모음은 활성화되지 않으므로 메인 창의 포커스를 빼앗지 않는다.
// ---------------------------------------------------------------
#include <windows.h>

namespace SelToolbar
{
    enum class Action { Highlight, Underline, Strikeout, Squiggly, Copy };

    using ActionFn = void (*)(HWND owner, Action action, COLORREF color);

    // anchor: 화면 좌표(보통 선택을 마친 마우스 위치). 그 위쪽에 표시하고,
    // 공간이 없으면 아래쪽에 표시한다.
    void Show(HWND owner, POINT anchor, const COLORREF* palette, int paletteCount, ActionFn onAction);
    void Hide();
    bool IsVisible();
}
