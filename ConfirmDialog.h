#pragma once
#include "framework.h"

extern HINSTANCE hInst;
extern bool      g_gotoModalDone; // 메시지 루프 종료 플래그 재사용

// message  : 본문 텍스트 (예: L"이 북마크를 삭제하시겠습니까?")
// returns  : true = 확인, false = 취소
bool ShowConfirmDialog(HWND hWndParent, const wchar_t* message);
