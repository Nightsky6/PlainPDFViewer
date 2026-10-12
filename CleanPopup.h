#pragma once
// ---------------------------------------------------------------
// CleanPopup - 재사용 가능한 Fluent 스타일 팝업 (Win32 + GDI+)
//
// 사용법:
//   1) 앱 시작 시 CleanPopup::Init(hInstance) 호출
//   2) CleanPopup::Show(hParent, L"제목", L"메시지", true/false)
//   3) 앱 종료 시 CleanPopup::Shutdown() 호출
//
// 필요 라이브러리: dwmapi.lib, gdiplus.lib, ole32.lib
// ---------------------------------------------------------------

#ifndef CLEAN_POPUP_H
#define CLEAN_POPUP_H

#include <windows.h>
#include <string>

namespace CleanPopup
{
    // 앱 시작 시 한 번 호출 (GDI+ 초기화 + 클래스 등록)
    bool Init(HINSTANCE hInstance);

    // 팝업 표시 (hasCancel=true 이면 취소+확인, false 이면 확인만)
    // 반환: 확인=true, 취소=false
    bool Show(HWND parent, const std::wstring& title,
              const std::wstring& msg, bool hasCancel = false);

    // 메모 텍스트 입력 팝업 (여러 줄 편집 가능)
    // 반환: 확인=true(text에 결과 저장), 취소=false
    bool ShowNoteInput(HWND parent, const std::wstring& title, std::wstring& text);

    // 비밀번호 입력 팝업 (마스킹)
    bool ShowPasswordInput(HWND parent, const std::wstring& title,
                           const std::wstring& prompt, std::wstring& password);

    // 정보(About) 팝업 표시
    // pngResourceId: RT_RCDATA로 등록된 PNG 리소스 ID
    void ShowAbout(HWND parent, int pngResourceId,
                   const std::wstring& appName,
                   const std::wstring& version,
                   const std::wstring& copyright);

    // 라이센스 팝업 표시 (Fluent 스타일)
    void ShowLicense(HWND parent, int pngResourceId,
                     const std::wstring& title,
                     const std::wstring& subtitle,
                     const std::wstring& licenseText);

    // 앱 종료 시 호출 (GDI+ 정리)
    void Shutdown();
}

#endif // CLEAN_POPUP_H
