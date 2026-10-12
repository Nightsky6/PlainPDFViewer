#pragma once

#include <windows.h>

// 토스트 표시 위치
enum class ToastPosition { Center, Bottom };

// 토스트 알림 표시
// hParent    - 부모 창 기준으로 위치 결정
// message    - 표시할 메시지
// durationMs - 표시 시간 (밀리초, 기본 1000)
// position   - 표시 위치 (기본 Center)
void ShowToast(HWND hParent, const wchar_t* message, int durationMs = 1400,
               ToastPosition position = ToastPosition::Bottom);
