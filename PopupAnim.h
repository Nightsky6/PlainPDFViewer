#pragma once
// ---------------------------------------------------------------
// PopupAnim.h - 모든 팝업 창이 함께 쓰는 등장/퇴장 애니메이션
//   나타날 때: 12px 아래에서 살짝 떠오르며 서서히 나타남 (ease-out)
//   사라질 때: 움직이지 않고 빠르게 투명해짐 (창을 옮기면 뒤 창을 계속 다시 그려야 해서 버벅임)
//   Windows 설정의 "애니메이션 효과"가 꺼져 있으면 움직이지 않고 바로 표시/숨김
// 대상 창은 WS_EX_LAYERED 최상위(WS_POPUP) 창이어야 한다 (위치는 화면 좌표).
// ---------------------------------------------------------------
#include <windows.h>

namespace PopupAnim
{
    constexpr int SHOW_MS = 190;
    constexpr int HIDE_MS = 90;
    constexpr int RISE_PX = 12;

    inline bool Enabled()
    {
        BOOL on = TRUE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return true;
        return on != FALSE;
    }

    // 지금 위치를 최종(기준) 위치로 기억한다
    inline void Begin(HWND h)
    {
        RECT r; GetWindowRect(h, &r);
        SetPropW(h, L"PopupAnim.x", (HANDLE)(INT_PTR)r.left);
        SetPropW(h, L"PopupAnim.y", (HANDLE)(INT_PTR)r.top);
        SetPropW(h, L"PopupAnim.on", (HANDLE)1);
    }

    // 기억한 위치를 지운다 (창을 없애기 전에 호출)
    inline void End(HWND h)
    {
        RemovePropW(h, L"PopupAnim.x");
        RemovePropW(h, L"PopupAnim.y");
        RemovePropW(h, L"PopupAnim.on");
    }

    // 한 프레임. t = 0~1 진행도 (가감속은 여기서 적용)
    inline void Frame(HWND h, float t, bool showing)
    {
        if (!Enabled()) t = 1.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        float alpha; int dy;
        if (showing)
        {
            const float u = 1.0f - t;
            const float e = 1.0f - u * u * u;              // ease-out cubic
            alpha = e;
            dy = (int)(RISE_PX * (1.0f - e) + 0.5f);
        }
        else
        {
            alpha = 1.0f - t;                                // 사라질 때는 투명도만 (위치 고정)
            dy = 0;
        }
        if (showing && GetPropW(h, L"PopupAnim.on"))
        {
            const int bx = (int)(INT_PTR)GetPropW(h, L"PopupAnim.x");
            const int by = (int)(INT_PTR)GetPropW(h, L"PopupAnim.y");
            SetWindowPos(h, nullptr, bx, by + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        SetLayeredWindowAttributes(h, 0, (BYTE)(255.0f * alpha + 0.5f), LWA_ALPHA);
    }

    // 나타내기 (끝날 때까지 기다림). 창은 최종 위치에 놓여 있어야 한다
    inline void Show(HWND h, int showCmd = SW_SHOW)
    {
        if (!Enabled())
        {
            SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
            ShowWindow(h, showCmd);
            UpdateWindow(h);
            return;
        }
        Begin(h);
        Frame(h, 0.0f, true);                                // 투명 + 아래쪽에서 시작
        ShowWindow(h, showCmd);
        RedrawWindow(h, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);   // 내용을 먼저 다 그려 둔다
        const ULONGLONG start = GetTickCount64();
        for (;;)
        {
            const float t = (float)(GetTickCount64() - start) / SHOW_MS;
            if (t >= 1.0f) break;
            Frame(h, t, true);
            Sleep(8);
        }
        Frame(h, 1.0f, true);
        End(h);
    }

    // 사라지게 하기 (끝날 때까지 기다림). 끝나면 투명한 상태로 남으므로 호출한 쪽에서 숨기거나 없앤다
    inline void Hide(HWND h)
    {
        if (!IsWindowVisible(h) || !Enabled())
        {
            SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
            return;
        }
        const ULONGLONG start = GetTickCount64();
        for (;;)
        {
            const float t = (float)(GetTickCount64() - start) / HIDE_MS;
            if (t >= 1.0f) break;
            Frame(h, t, false);
            Sleep(8);
        }
        Frame(h, 1.0f, false);
    }
}
