#include "pch.h"
#include "CppUnitTest.h"
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include "../Toolbar.h"
#pragma comment(lib, "gdiplus.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// DrawToolbar 베이스라인: 호버 버튼이 바뀔 때 매번 DrawToolbar가 호출되는 패턴 측정
TEST_CLASS(ToolbarPerfTest)
{
    ULONG_PTR gdiplusToken = 0;
    HDC       hdc          = nullptr;
    HWND      hWnd         = nullptr;
    HBITMAP   hBmp         = nullptr;
    HDC       memDC        = nullptr;

public:
    TEST_METHOD_INITIALIZE(Setup)
    {
        Gdiplus::GdiplusStartupInput si;
        Gdiplus::GdiplusStartup(&gdiplusToken, &si, nullptr);

        hWnd = CreateWindowExW(0, L"STATIC", nullptr, WS_POPUP,
                               0, 0, 1280, 800, nullptr, nullptr,
                               GetModuleHandle(nullptr), nullptr);
        hdc   = GetDC(hWnd);
        memDC = CreateCompatibleDC(hdc);
        hBmp  = CreateCompatibleBitmap(hdc, 1280, 40);
        SelectObject(memDC, hBmp);
    }

    TEST_METHOD_CLEANUP(Teardown)
    {
        DeleteObject(hBmp);
        DeleteDC(memDC);
        ReleaseDC(hWnd, hdc);
        DestroyWindow(hWnd);
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }

    // 툴바 전체 재그리기를 100회 반복 (호버 변경 시뮬레이션)
    TEST_METHOD(DrawToolbar_HoverCycle_100)
    {
        RECT clientRect = { 0, 0, 1280, 800 };
        for (int i = 0; i < 100; ++i)
        {
            g_hoverButton = i % 25; // 버튼 인덱스 순환
            DrawToolbar(memDC, clientRect);
        }
    }
};
