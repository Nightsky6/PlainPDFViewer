// LicenseDlg.cpp : 오픈소스 라이센스 대화 상자
//

#include "framework.h"
#include "PlainPDFViewer.h"
#include "LicenseDlg.h"
#include "Lang.h"
#include <string>
#include <uxtheme.h>
#include <algorithm>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

// ─── 라이센스 대화 상자 ────────────────────────────────────────────────────────

static constexpr COLORREF CLR_BG        = RGB(24, 24, 30);
static constexpr COLORREF CLR_EDIT_BG   = RGB(18, 18, 24);
static constexpr COLORREF CLR_EDIT_FG   = RGB(200, 200, 212);
static constexpr COLORREF CLR_HEADER_T  = RGB(42, 42, 52);
static constexpr COLORREF CLR_HEADER_B  = RGB(28, 28, 36);
static constexpr COLORREF CLR_ACCENT    = RGB(80, 140, 255);
static constexpr COLORREF CLR_TITLE     = RGB(225, 225, 238);
static constexpr COLORREF CLR_SUBTITLE  = RGB(130, 135, 155);
static constexpr COLORREF CLR_BTN_BG    = RGB(55, 58, 75);
static constexpr COLORREF CLR_BTN_HOV   = RGB(70, 100, 180);
static constexpr COLORREF CLR_ROSE_L    = RGB(183, 110, 121);
static constexpr COLORREF CLR_ROSE_R    = RGB(226, 170, 152);
static constexpr COLORREF CLR_BTN_FG    = RGB(230, 230, 240);
static constexpr COLORREF CLR_SEP       = RGB(55, 55, 68);

struct LicenseDlgData
{
    HBRUSH hBgBrush;
    HBRUSH hEditBrush;
    HFONT  hEditFont;
    HFONT  hHeaderFont;
    HFONT  hSubFont;
    HFONT  hBtnFont;
    BOOL   bBtnHover;
};

static void FillRoundRect(HDC hdc, const RECT& rc, int r, COLORREF clr)
{
    HBRUSH br = CreateSolidBrush(clr);
    HBRUSH ob = static_cast<HBRUSH>(SelectObject(hdc, br));
    HPEN   op = static_cast<HPEN>(SelectObject(hdc, static_cast<HPEN>(GetStockObject(NULL_PEN))));
    // +1 보정: GDI RoundRect는 right/bottom을 exclusive로 처리
    RoundRect(hdc, rc.left, rc.top, rc.right + 1, rc.bottom + 1, r, r);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(br);
}

INT_PTR CALLBACK LicenseDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    auto* pData = reinterpret_cast<LicenseDlgData*>(
        GetWindowLongPtrW(hDlg, GWLP_USERDATA));

    switch (message)
    {
    case WM_INITDIALOG:
    {
        auto* pd = new LicenseDlgData{};
        pd->hBgBrush   = CreateSolidBrush(CLR_BG);
        pd->hEditBrush  = CreateSolidBrush(CLR_EDIT_BG);
        pd->hEditFont   = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
            FIXED_PITCH, L"Consolas");
        pd->hHeaderFont = CreateFontW(19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
            0, L"Segoe UI");
        pd->hSubFont    = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
            0, L"Segoe UI");
        pd->hBtnFont    = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
            0, L"Segoe UI");
        pd->bBtnHover   = FALSE;
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pd));

        // DWM 다크 모드 타이틀 바
        BOOL useDark = TRUE;
        DwmSetWindowAttribute(hDlg, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
            &useDark, sizeof(useDark));

        static const wchar_t* licenseBodyText =
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"PDFium\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"Copyright 2014 The PDFium Authors\r\n"
            L"BSD 3-Clause License\r\n"
            L"\r\n"
            L"Redistribution and use in source and binary forms, with or without\r\n"
            L"modification, are permitted provided that the following conditions are met:\r\n"
            L"\r\n"
            L"1. Redistributions of source code must retain the above copyright notice,\r\n"
            L"   this list of conditions and the following disclaimer.\r\n"
            L"2. Redistributions in binary form must reproduce the above copyright notice,\r\n"
            L"   this list of conditions and the following disclaimer in the documentation\r\n"
            L"   and/or other materials provided with the distribution.\r\n"
            L"3. Neither the name of the copyright holder nor the names of its contributors\r\n"
            L"   may be used to endorse or promote products derived from this software\r\n"
            L"   without specific prior written permission.\r\n"
            L"\r\n"
            L"THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\r\n"
            L"AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\r\n"
            L"IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE\r\n"
            L"ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE\r\n"
            L"LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR\r\n"
            L"CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF\r\n"
            L"SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS\r\n"
            L"INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN\r\n"
            L"CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)\r\n"
            L"ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE\r\n"
            L"POSSIBILITY OF SUCH DAMAGE.\r\n"
            L"\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"FreeType\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"Copyright (C) 1996-2023 David Turner, Robert Wilhelm, and Werner Lemberg.\r\n"
            L"FreeType License (FTL) \u2014 BSD-style license.\r\n"
            L"https://freetype.org/license.html\r\n"
            L"\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"ICU (International Components for Unicode)\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"Copyright (C) 1991-2023 Unicode, Inc.\r\n"
            L"ICU License \u2014 MIT-style license.\r\n"
            L"https://unicode.org/copyright.html\r\n"
            L"\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"libjpeg-turbo\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"Copyright (C) 2009-2023 D. R. Commander.\r\n"
            L"BSD 3-Clause License / IJG License.\r\n"
            L"https://libjpeg-turbo.org/About/License\r\n"
            L"\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"zlib\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"Copyright (C) 1995-2023 Jean-loup Gailly and Mark Adler.\r\n"
            L"zlib License.\r\n"
            L"https://zlib.net/zlib_license.html\r\n"
            L"\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"OpenJPEG\r\n"
            L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\r\n"
            L"Copyright (C) 2002-2023 Communications and Remote Sensing Laboratory.\r\n"
            L"BSD 2-Clause License.\r\n"
            L"https://www.openjpeg.org/\r\n";

        std::wstring licenseText = LS(STR_LIC_APP_NAME);
        licenseText += L"Copyright (C) 2026\r\n\r\n";
        licenseText += LS(STR_LIC_DESC);
        licenseText += L"\r\n";
        licenseText += licenseBodyText;

        SetDlgItemTextW(hDlg, IDC_LICENSE_TEXT, licenseText.c_str());

        // 에디트 폰트 + 비주얼 테마 제거 (WM_CTLCOLORSTATIC 신뢰성 확보)
        HWND hEdit = GetDlgItem(hDlg, IDC_LICENSE_TEXT);
        SendMessageW(hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(pd->hEditFont), TRUE);
        SetWindowTheme(hEdit, L"", L"");

        // 스크롤 맨 위로
        SendMessageW(hEdit, EM_SETSEL, 0, 0);
        SendMessageW(hEdit, EM_SCROLLCARET, 0, 0);

        // ── 부모 창 중앙 배치 ──────────────────────────────────────
        {
            HWND hParent = GetParent(hDlg);
            if (!hParent) hParent = GetDesktopWindow();

            RECT rcParent, rcDlg;
            GetWindowRect(hParent, &rcParent);
            GetWindowRect(hDlg, &rcDlg);

            const int dlgW = rcDlg.right - rcDlg.left;
            const int dlgH = rcDlg.bottom - rcDlg.top;
            int x = rcParent.left + (rcParent.right - rcParent.left - dlgW) / 2;
            int y = rcParent.top + (rcParent.bottom - rcParent.top - dlgH) / 2;

            // 모니터 작업 영역 밖으로 나가지 않도록 클램핑
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfo(MonitorFromWindow(hParent, MONITOR_DEFAULTTONEAREST), &mi))
            {
                x = std::clamp(x, (int)mi.rcWork.left, (int)mi.rcWork.right - dlgW);
                y = std::clamp(y, (int)mi.rcWork.top, (int)mi.rcWork.bottom - dlgH);
            }

            SetWindowPos(hDlg, nullptr, x, y, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        // ────────────────────────────────────────────────────────────

        SetFocus(GetDlgItem(hDlg, IDOK));
        return (INT_PTR)FALSE;
    }

    case WM_ERASEBKGND:
    {
        if (!pData) return FALSE;
        HDC  hdc = reinterpret_cast<HDC>(wParam);
        RECT rc;
        GetClientRect(hDlg, &rc);

        RECT editRc{};
        if (HWND hEdit = GetDlgItem(hDlg, IDC_LICENSE_TEXT))
        {
            GetWindowRect(hEdit, &editRc);
            MapWindowPoints(HWND_DESKTOP, hDlg,
                reinterpret_cast<POINT*>(&editRc), 2);
        }
        const int HEADER_H = (editRc.top > 4) ? editRc.top - 2 : 48;

        // ① 헤더 그라데이션
        TRIVERTEX tv[2] = {};
        tv[0] = { rc.left,  rc.top,
            static_cast<COLOR16>(GetRValue(CLR_HEADER_T) << 8),
            static_cast<COLOR16>(GetGValue(CLR_HEADER_T) << 8),
            static_cast<COLOR16>(GetBValue(CLR_HEADER_T) << 8), 0 };
        tv[1] = { rc.right, HEADER_H,
            static_cast<COLOR16>(GetRValue(CLR_HEADER_B) << 8),
            static_cast<COLOR16>(GetGValue(CLR_HEADER_B) << 8),
            static_cast<COLOR16>(GetBValue(CLR_HEADER_B) << 8), 0 };
        GRADIENT_RECT grc = { 0, 1 };
        GradientFill(hdc, tv, 2, &grc, 1, GRADIENT_FILL_RECT_V);

        // ② 악센트 라인 (상단 2px)
        RECT accentRc = { rc.left, rc.top, rc.right, rc.top + 2 };
        HBRUSH accentBr = CreateSolidBrush(CLR_ACCENT);
        FillRect(hdc, &accentRc, accentBr);
        DeleteObject(accentBr);

        // ③ 헤더 하단 구분선
        HPEN sep = CreatePen(PS_SOLID, 1, CLR_SEP);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, sep));
        MoveToEx(hdc, rc.left, HEADER_H - 1, nullptr);
        LineTo(hdc, rc.right, HEADER_H - 1);
        SelectObject(hdc, oldPen);
        DeleteObject(sep);

        // ④ 앱 아이콘 (plain.png, 40×40)
        {
            const int iconSize = 50;
            const int iconX = 10;
            const int iconY = (HEADER_H - iconSize) / 2 + 1;
            const int textLeft = iconX + iconSize + 10;

            HRSRC hRes = FindResourceW(GetModuleHandleW(nullptr),
                MAKEINTRESOURCE(IDB_PLAIN_PNG), RT_RCDATA);
            if (hRes)
            {
                DWORD sz = SizeofResource(GetModuleHandleW(nullptr), hRes);
                HGLOBAL hMem = LoadResource(GetModuleHandleW(nullptr), hRes);
                void* pData2 = LockResource(hMem);
                if (pData2)
                {
                    HGLOBAL hBuf = GlobalAlloc(GMEM_MOVEABLE, sz);
                    void* pBuf = GlobalLock(hBuf);
                    memcpy(pBuf, pData2, sz);
                    GlobalUnlock(hBuf);

                    IStream* pStream = nullptr;
                    CreateStreamOnHGlobal(hBuf, TRUE, &pStream);
                    if (pStream)
                    {
                        Gdiplus::Graphics gfx(hdc);
                        gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                        Gdiplus::Image img(pStream);
                        gfx.DrawImage(&img, iconX, iconY, iconSize, iconSize);
                        pStream->Release();
                    }
                }
            }
            else
            {
                // fallback: 아이콘 사용
                HICON hIco = static_cast<HICON>(
                    LoadImage(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_PLAINPDFVIEWER),
                        IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR));
                if (hIco)
                {
                    DrawIconEx(hdc, iconX, iconY,
                        hIco, iconSize, iconSize, 0, nullptr, DI_NORMAL);
                    DestroyIcon(hIco);
                }
            }

            // ⑤ 헤더 제목
            const int midY = HEADER_H / 2;
            SetBkMode(hdc, TRANSPARENT);
            HFONT oldFont = static_cast<HFONT>(
                SelectObject(hdc, pData->hHeaderFont));
            SetTextColor(hdc, CLR_TITLE);
            RECT titleRc = { textLeft, midY - 20, rc.right - 10, midY };
            DrawTextW(hdc, LS(STR_LIC_TITLE), -1, &titleRc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // ⑥ 서브 타이틀
            SelectObject(hdc, pData->hSubFont);
            SetTextColor(hdc, CLR_SUBTITLE);
            RECT subRc = { textLeft, midY, rc.right - 10, midY + 18 };
            DrawTextW(hdc, L"Plain PDF Viewer · Third-party Licenses", -1, &subRc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldFont);
        }

        // ⑦ 본문 배경
        RECT bodyRc = { rc.left, HEADER_H, rc.right, rc.bottom };
        FillRect(hdc, &bodyRc, pData->hBgBrush);

        // ⑧ 에디트 영역 주변 미묘한 보더
        if (editRc.right > editRc.left)
        {
            RECT borderRc = { editRc.left - 1, editRc.top - 1,
                              editRc.right + 1, editRc.bottom + 1 };
            HPEN bPen = CreatePen(PS_SOLID, 1, RGB(45, 48, 62));
            HPEN oPen = static_cast<HPEN>(SelectObject(hdc, bPen));
            HBRUSH oBr = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
            Rectangle(hdc, borderRc.left, borderRc.top, borderRc.right, borderRc.bottom);
            SelectObject(hdc, oBr);
            SelectObject(hdc, oPen);
            DeleteObject(bPen);
        }

        return 1;
    }

    // ES_READONLY 에디트는 WM_CTLCOLORSTATIC 으로 전달됨
    case WM_CTLCOLORSTATIC:
    {
        if (!pData) break;
        HDC  hdc = reinterpret_cast<HDC>(wParam);
        HWND hCtl = reinterpret_cast<HWND>(lParam);
        if (hCtl == GetDlgItem(hDlg, IDC_LICENSE_TEXT))
        {
            SetBkColor(hdc, CLR_EDIT_BG);
            SetTextColor(hdc, CLR_EDIT_FG);
            return reinterpret_cast<INT_PTR>(pData->hEditBrush);
        }
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, CLR_EDIT_FG);
        return reinterpret_cast<INT_PTR>(pData->hBgBrush);
    }

    // 일반 에디트 컨트롤 fallback
    case WM_CTLCOLOREDIT:
    {
        if (!pData) break;
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, CLR_EDIT_BG);
        SetTextColor(hdc, CLR_EDIT_FG);
        return reinterpret_cast<INT_PTR>(pData->hEditBrush);
    }

    case WM_CTLCOLORDLG:
        if (pData)
            return reinterpret_cast<INT_PTR>(pData->hBgBrush);
        break;

    case WM_DRAWITEM:
    {
        if (!pData) break;
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlID != IDOK) break;

        BOOL pressed = (dis->itemState & ODS_SELECTED);
        BOOL focused = (dis->itemState & ODS_FOCUS);
        COLORREF bg = pressed  ? RGB(60, 110, 220)
                    : pData->bBtnHover ? CLR_BTN_HOV
                    : CLR_BTN_BG;
        COLORREF border = pressed  ? CLR_ACCENT
                        : pData->bBtnHover ? RGB(200, 140, 130)
                        : focused ? RGB(70, 80, 110)
                        : RGB(65, 68, 85);

        // 버튼 윈도우의 사각 영역을 부모 배경으로 먼저 채워 모서리 잔상 제거
        HBRUSH bgBr = CreateSolidBrush(CLR_BG);
        FillRect(dis->hDC, &dis->rcItem, bgBr);
        DeleteObject(bgBr);

        // 배경
        if (pData->bBtnHover && !pressed)
        {
            // 로즈골드 그라데이션 (좌→우)
            HRGN hRgn = CreateRoundRectRgn(dis->rcItem.left, dis->rcItem.top,
                dis->rcItem.right + 1, dis->rcItem.bottom + 1, 8, 8);
            SelectClipRgn(dis->hDC, hRgn);
            TRIVERTEX vtx[2] = {};
            vtx[0] = { dis->rcItem.left, dis->rcItem.top,
                static_cast<COLOR16>(GetRValue(CLR_ROSE_L) << 8),
                static_cast<COLOR16>(GetGValue(CLR_ROSE_L) << 8),
                static_cast<COLOR16>(GetBValue(CLR_ROSE_L) << 8), 0 };
            vtx[1] = { dis->rcItem.right, dis->rcItem.bottom,
                static_cast<COLOR16>(GetRValue(CLR_ROSE_R) << 8),
                static_cast<COLOR16>(GetGValue(CLR_ROSE_R) << 8),
                static_cast<COLOR16>(GetBValue(CLR_ROSE_R) << 8), 0 };
            GRADIENT_RECT gr = { 0, 1 };
            GradientFill(dis->hDC, vtx, 2, &gr, 1, GRADIENT_FILL_RECT_H);
            SelectClipRgn(dis->hDC, nullptr);
            DeleteObject(hRgn);
        }
        else
        {
            FillRoundRect(dis->hDC, dis->rcItem, 8, bg);
        }

        // 테두리 (배경 위에 정확히 겹치도록)
        HPEN bp = CreatePen(PS_INSIDEFRAME, 1, border);
        HPEN op = static_cast<HPEN>(SelectObject(dis->hDC, bp));
        HBRUSH ob = static_cast<HBRUSH>(SelectObject(dis->hDC, GetStockObject(NULL_BRUSH)));
        RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top,
            dis->rcItem.right, dis->rcItem.bottom, 8, 8);
        SelectObject(dis->hDC, ob);
        SelectObject(dis->hDC, op);
        DeleteObject(bp);

        // 텍스트
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, CLR_BTN_FG);
        HFONT of = static_cast<HFONT>(SelectObject(dis->hDC, pData->hBtnFont));
        RECT textRc = dis->rcItem;
        if (pressed) { textRc.top += 1; textRc.left += 1; }
        DrawTextW(dis->hDC, LS(STR_LIC_BTN_CLOSE), -1, &textRc,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dis->hDC, of);
        return (INT_PTR)TRUE;
    }

    case WM_NOTIFY:
    {
        // 버튼 호버 트래킹 — subclass 대신 타이머 기반 간이 구현
        break;
    }

    case WM_TIMER:
    {
        if (wParam == 100 && pData)
        {
            HWND hBtn = GetDlgItem(hDlg, IDOK);
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hBtn, &pt);
            RECT rc;
            GetClientRect(hBtn, &rc);
            BOOL hover = PtInRect(&rc, pt);
            if (hover != pData->bBtnHover)
            {
                pData->bBtnHover = hover;
                InvalidateRect(hBtn, nullptr, TRUE);
            }
            if (!hover)
            {
                KillTimer(hDlg, 100);
            }
        }
        break;
    }

    case WM_PARENTNOTIFY:
    {
        break;
    }

    case WM_SETCURSOR:
    {
        if (pData && reinterpret_cast<HWND>(wParam) == GetDlgItem(hDlg, IDOK))
        {
            if (!pData->bBtnHover)
            {
                pData->bBtnHover = TRUE;
                InvalidateRect(GetDlgItem(hDlg, IDOK), nullptr, TRUE);
                SetTimer(hDlg, 100, 50, nullptr);
            }
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;

    case WM_NCDESTROY:
        if (pData)
        {
            KillTimer(hDlg, 100);
            DeleteObject(pData->hBgBrush);
            DeleteObject(pData->hEditBrush);
            DeleteObject(pData->hEditFont);
            DeleteObject(pData->hHeaderFont);
            DeleteObject(pData->hSubFont);
            DeleteObject(pData->hBtnFont);
            delete pData;
            SetWindowLongPtrW(hDlg, GWLP_USERDATA, 0);
        }
        break;
    }
    return (INT_PTR)FALSE;
}
