// Preferences.cpp : 사용자 환경 설정 (레지스트리 I/O, 다이얼로그)
//

#include "framework.h"
#include "Preferences.h"
#include "resource.h"
#include "Lang.h"
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <cstring>

// ── 레지스트리 키 ────────────────────────────────────────────────────────────
#define REGISTRY_KEY       L"Software\\PlainPDFViewer"
#define QUICKJUMP_KEY      L"Software\\PlainPDFViewer\\QuickJumps"

// ── 전역 설정 인스턴스 ────────────────────────────────────────────────────────
AppPreferences g_prefs;
std::vector<QuickJumpItem> g_quickJumps;

// ── 레지스트리 저장 ───────────────────────────────────────────────────────────
void SavePrefs()
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;
    DWORD color = (DWORD)g_prefs.selectionColor;
    RegSetValueExW(hKey, L"SelColor", 0, REG_DWORD, (BYTE*)&color, sizeof(DWORD));
    DWORD speed; std::memcpy(&speed, &g_prefs.scrollSpeed, sizeof(DWORD));
    RegSetValueExW(hKey, L"ScrollSpeed", 0, REG_DWORD, (BYTE*)&speed, sizeof(DWORD));
    DWORD recent = (DWORD)g_prefs.maxRecentFiles;
    RegSetValueExW(hKey, L"MaxRecentFiles", 0, REG_DWORD, (BYTE*)&recent, sizeof(DWORD));
    RegCloseKey(hKey);
}

// ── 레지스트리 불러오기 ───────────────────────────────────────────────────────
// 참고: LoadPrefs() 호출 후 호출자가 g_selectionColor / g_maxRecentFiles 를
//       g_prefs 값으로 동기화해야 합니다.
void LoadPrefs()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return;
    DWORD val, sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"SelColor", nullptr, nullptr, (BYTE*)&val, &sz) == ERROR_SUCCESS)
        g_prefs.selectionColor = (COLORREF)val;
    sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"ScrollSpeed", nullptr, nullptr, (BYTE*)&val, &sz) == ERROR_SUCCESS)
        std::memcpy(&g_prefs.scrollSpeed, &val, sizeof(float));
    sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"MaxRecentFiles", nullptr, nullptr, (BYTE*)&val, &sz) == ERROR_SUCCESS)
        g_prefs.maxRecentFiles = std::max(1, std::min(10, (int)val));
    RegCloseKey(hKey);
}

// ── QuickJump 저장 ────────────────────────────────────────────────────────────
void SaveQuickJumps()
{
    // 기존 키 삭제 후 재생성
    RegDeleteKeyW(HKEY_CURRENT_USER, QUICKJUMP_KEY);
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, QUICKJUMP_KEY, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;
    DWORD count = (DWORD)g_quickJumps.size();
    RegSetValueExW(hKey, L"Count", 0, REG_DWORD, (BYTE*)&count, sizeof(DWORD));
    for (DWORD i = 0; i < count; ++i)
    {
        WCHAR valName[32];
        swprintf_s(valName, L"Label%u", i);
        RegSetValueExW(hKey, valName, 0, REG_SZ,
            (BYTE*)g_quickJumps[i].label.c_str(),
            (DWORD)((g_quickJumps[i].label.size() + 1) * sizeof(WCHAR)));
        swprintf_s(valName, L"Keyword%u", i);
        RegSetValueExW(hKey, valName, 0, REG_SZ,
            (BYTE*)g_quickJumps[i].keyword.c_str(),
            (DWORD)((g_quickJumps[i].keyword.size() + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(hKey);
}

// ── QuickJump 불러오기 ────────────────────────────────────────────────────────
void LoadQuickJumps()
{
    g_quickJumps.clear();
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, QUICKJUMP_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return;
    DWORD count = 0, sz = sizeof(DWORD);
    if (RegQueryValueExW(hKey, L"Count", nullptr, nullptr, (BYTE*)&count, &sz) != ERROR_SUCCESS)
        count = 0;
    count = std::min(count, (DWORD)QUICKJUMP_MAX);
    for (DWORD i = 0; i < count; ++i)
    {
        WCHAR valName[32], buf[256];
        QuickJumpItem item;
        DWORD bufSz = sizeof(buf);
        swprintf_s(valName, L"Label%u", i);
        if (RegQueryValueExW(hKey, valName, nullptr, nullptr, (BYTE*)buf, &bufSz) == ERROR_SUCCESS)
            item.label = buf;
        bufSz = sizeof(buf);
        swprintf_s(valName, L"Keyword%u", i);
        if (RegQueryValueExW(hKey, valName, nullptr, nullptr, (BYTE*)buf, &bufSz) == ERROR_SUCCESS)
            item.keyword = buf;
        if (!item.keyword.empty())
            g_quickJumps.push_back(std::move(item));
    }
    RegCloseKey(hKey);
}

// ── 환경 설정 다이얼로그 프로시저 ─────────────────────────────────────────────

// 배경/섹션 색상 상수
static const COLORREF kDlgBg      = RGB(245, 246, 248);
static const COLORREF kSecBg      = RGB(60,  120, 210);
static const COLORREF kSecFg      = RGB(255, 255, 255);
static const COLORREF kSepLine    = RGB(210, 215, 225);
static const COLORREF kCardBg     = RGB(255, 255, 255);

static INT_PTR CALLBACK PreferencesDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static AppPreferences* pPrefs = nullptr;
    static COLORREF s_customColors[16] = {};
    static HBRUSH   s_hBgBrush  = nullptr;
    static HBRUSH   s_hSecBrush = nullptr;
    static HFONT    s_hSecFont  = nullptr;
    static int      s_scrollPos = 0;   // 현재 스크롤 위치 (픽셀)
    static int      s_scrollMax = 0;   // 전체 컨텐츠 높이 - 클라이언트 높이

    // 스크롤 위치 delta만큼 모든 자식 컨트롤을 이동
    auto ScrollBy = [&](int delta) {
        int newPos = s_scrollPos + delta;
        if (newPos < 0) newPos = 0;
        if (newPos > s_scrollMax) newPos = s_scrollMax;
        if (newPos == s_scrollPos) return;
        int dy = s_scrollPos - newPos;
        s_scrollPos = newPos;
        SetScrollPos(hDlg, SB_VERT, s_scrollPos, TRUE);
        // 자식 컨트롤 전체 이동
        HWND hChild = GetWindow(hDlg, GW_CHILD);
        while (hChild) {
            RECT rc; GetWindowRect(hChild, &rc);
            POINT pt = { rc.left, rc.top }; ScreenToClient(hDlg, &pt);
            SetWindowPos(hChild, nullptr, pt.x, pt.y + dy, 0, 0,
                SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
            hChild = GetWindow(hChild, GW_HWNDNEXT);
        }
        InvalidateRect(hDlg, nullptr, TRUE);
    };

    // 스크롤바 범위/페이지 설정
    auto SetupScroll = [&]() {
        RECT client; GetClientRect(hDlg, &client);
        // 컨텐츠 높이: RC 다이얼로그 유닛 270을 픽셀로 변환
        RECT duRect = { 0, 0, 0, 270 };
        MapDialogRect(hDlg, &duRect);
        int contentH = duRect.bottom;
        int clientH  = client.bottom;
        s_scrollMax = contentH > clientH ? contentH - clientH : 0;
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin   = 0;
        si.nMax   = contentH;
        si.nPage  = clientH;
        si.nPos   = s_scrollPos;
        SetScrollInfo(hDlg, SB_VERT, &si, TRUE);
    };

    auto UpdateSpeedLabel = [&]() {
        if (!pPrefs) return;
        HWND hSlider = GetDlgItem(hDlg, IDC_PREF_SCROLL_SPEED);
        int val = (int)SendMessageW(hSlider, TBM_GETPOS, 0, 0);
        float speed = val * 0.3f;
        WCHAR buf[16];
        swprintf_s(buf, L"x%.1f", speed);
        SetDlgItemTextW(hDlg, IDC_PREF_SPEED_LABEL, buf);
        pPrefs->scrollSpeed = speed;
    };

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        pPrefs = reinterpret_cast<AppPreferences*>(lParam);

        // 배경/섹션 브러시 생성
        s_hBgBrush  = CreateSolidBrush(kDlgBg);
        s_hSecBrush = CreateSolidBrush(kCardBg);

        // 섹션 헤더 볼드 폰트
        NONCLIENTMETRICSW ncm = {};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
        ncm.lfMessageFont.lfHeight = -MulDiv(9, GetDeviceCaps(GetDC(hDlg), LOGPIXELSY), 72);
        s_hSecFont = CreateFontIndirectW(&ncm.lfMessageFont);

        // 섹션 헤더 컨트롤에 폰트 적용
        for (int id : { IDC_PREF_SEC_DISPLAY, IDC_PREF_SEC_SCROLL, IDC_PREF_SEC_FILE, IDC_PREF_SEC_LANG })
        {
            SendDlgItemMessageW(hDlg, id, WM_SETFONT, (WPARAM)s_hSecFont, FALSE);
            HWND hH = GetDlgItem(hDlg, id);
            RECT r; GetWindowRect(hH, &r);
            POINT p = { r.left, r.top }; ScreenToClient(hDlg, &p);
            MoveWindow(hH, p.x + 10, p.y, (r.right - r.left) - 10, r.bottom - r.top, FALSE);
        }

        for (int id : { IDOK, IDCANCEL })
            SendDlgItemMessageW(hDlg, id, BM_SETSTYLE, BS_OWNERDRAW, TRUE);

        // 스크롤 슬라이더 초기화
        HWND hSlider = GetDlgItem(hDlg, IDC_PREF_SCROLL_SPEED);
        SendMessageW(hSlider, TBM_SETRANGE, TRUE, MAKELPARAM(1, 10));
        SendMessageW(hSlider, TBM_SETTICFREQ, 1, 0);
        int initVal = std::max(1, std::min(10, (int)roundf(pPrefs->scrollSpeed / 0.3f)));
        SendMessageW(hSlider, TBM_SETPOS, TRUE, initVal);
        UpdateSpeedLabel();

        // 최근 파일 개수 스핀박스 초기화
        HWND hEdit = GetDlgItem(hDlg, IDC_PREF_RECENT_COUNT);
        HWND hSpin = GetDlgItem(hDlg, IDC_PREF_RECENT_SPIN);
        SendMessageW(hSpin, UDM_SETBUDDY, (WPARAM)hEdit, 0);
        SendMessageW(hSpin, UDM_SETRANGE, 0, MAKELPARAM(10, 1));
        SendMessageW(hSpin, UDM_SETPOS,   0, pPrefs->maxRecentFiles);
        InvalidateRect(GetDlgItem(hDlg, IDC_PREF_SEL_COLOR), nullptr, TRUE);

        // 언어 선택 콤보박스 초기화
        HWND hLang = GetDlgItem(hDlg, IDC_PREF_LANG_COMBO);
        SendMessageW(hLang, CB_ADDSTRING, 0, (LPARAM)L"한국어");
        SendMessageW(hLang, CB_ADDSTRING, 0, (LPARAM)L"English");
        SendMessageW(hLang, CB_ADDSTRING, 0, (LPARAM)L"日本語");
        SendMessageW(hLang, CB_SETCURSEL, (WPARAM)(int)g_appLang, 0);

        // 스크롤바 초기화
        s_scrollPos = 0;
        SetupScroll();
        return TRUE;
    }

    case WM_SIZE:
        SetupScroll();
        break;

    case WM_VSCROLL:
    {
        SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_ALL;
        GetScrollInfo(hDlg, SB_VERT, &si);
        int newPos = si.nPos;
        switch (LOWORD(wParam)) {
        case SB_LINEUP:       newPos -= 10; break;
        case SB_LINEDOWN:     newPos += 10; break;
        case SB_PAGEUP:       newPos -= si.nPage; break;
        case SB_PAGEDOWN:     newPos += si.nPage; break;
        case SB_THUMBTRACK:   newPos  = si.nTrackPos; break;
        case SB_THUMBPOSITION:newPos  = si.nTrackPos; break;
        }
        ScrollBy(newPos - s_scrollPos);
        return TRUE;
    }

    case WM_MOUSEWHEEL:
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        ScrollBy(-delta * 20);
        return TRUE;
    }

    case WM_DESTROY:
        if (s_hBgBrush)  { DeleteObject(s_hBgBrush);  s_hBgBrush  = nullptr; }
        if (s_hSecBrush) { DeleteObject(s_hSecBrush); s_hSecBrush = nullptr; }
        if (s_hSecFont)  { DeleteObject(s_hSecFont);  s_hSecFont  = nullptr; }
        break;

    case WM_ERASEBKGND:
    {
        // 전체 배경을 kDlgBg로 채움
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hDlg, &rc);
        FillRect(hdc, &rc, s_hBgBrush ? s_hBgBrush : GetSysColorBrush(COLOR_BTNFACE));

        const int hdrIds[4] = { IDC_PREF_SEC_DISPLAY, IDC_PREF_SEC_SCROLL, IDC_PREF_SEC_FILE, IDC_PREF_SEC_LANG };
        RECT hr[4];
        for (int i = 0; i < 4; ++i)
        {
            RECT r; GetWindowRect(GetDlgItem(hDlg, hdrIds[i]), &r);
            POINT a = { r.left, r.top }, b = { r.right, r.bottom };
            ScreenToClient(hDlg, &a); ScreenToClient(hDlg, &b);
            hr[i] = { a.x, a.y, b.x, b.y };
        }
        RECT langRc; GetWindowRect(GetDlgItem(hDlg, IDC_PREF_LANG_COMBO), &langRc);
        POINT lb = { langRc.left, langRc.bottom }; ScreenToClient(hDlg, &lb);

        HBRUSH hCard    = CreateSolidBrush(kCardBg);
        HPEN   hCardPen = CreatePen(PS_SOLID, 1, kSepLine);
        HBRUSH hAccent  = CreateSolidBrush(kSecBg);
        HBRUSH oB = (HBRUSH)SelectObject(hdc, hCard);
        HPEN   oP = (HPEN)SelectObject(hdc, hCardPen);
        for (int i = 0; i < 4; ++i)
        {
            RECT card = { hr[i].left - 10, hr[i].top - 6, hr[i].right + 2,
                          (i < 3) ? hr[i + 1].top - 12 : lb.y + 10 };
            RoundRect(hdc, card.left, card.top, card.right, card.bottom, 8, 8);
            RECT bar = { card.left + 1, hr[i].top, card.left + 4, hr[i].bottom };
            FillRect(hdc, &bar, hAccent);
            MoveToEx(hdc, card.left + 10, hr[i].bottom + 3, nullptr);
            LineTo(hdc, card.right - 10, hr[i].bottom + 3);
        }
        SelectObject(hdc, oB); SelectObject(hdc, oP);
        DeleteObject(hCard); DeleteObject(hCardPen); DeleteObject(hAccent);
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdcCtrl = (HDC)wParam;
        HWND hCtrl  = (HWND)lParam;
        int id = GetDlgCtrlID(hCtrl);

        // 섹션 헤더: 파란 배경 + 흰 글자
        if (id == IDC_PREF_SEC_DISPLAY || id == IDC_PREF_SEC_SCROLL || id == IDC_PREF_SEC_FILE || id == IDC_PREF_SEC_LANG)
        {
            SetBkColor(hdcCtrl, kCardBg);
            SetTextColor(hdcCtrl, kSecBg);
            return (INT_PTR)(s_hSecBrush ? s_hSecBrush : GetSysColorBrush(COLOR_WINDOW));
        }

        // 나머지 Static: 카드 배경색
        SetBkColor(hdcCtrl, kCardBg);
        SetTextColor(hdcCtrl, RGB(50, 50, 50));
        return (INT_PTR)(s_hSecBrush ? s_hSecBrush : GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_CTLCOLOREDIT:
    {
        HDC hdcCtrl = (HDC)wParam;
        SetBkColor(hdcCtrl, RGB(255, 255, 255));
        SetTextColor(hdcCtrl, RGB(30, 30, 30));
        static HBRUSH hWhite = CreateSolidBrush(RGB(255, 255, 255));
        return (INT_PTR)hWhite;
    }

    case WM_HSCROLL: // 슬라이더 변경
        if (reinterpret_cast<HWND>(lParam) == GetDlgItem(hDlg, IDC_PREF_SCROLL_SPEED))
            UpdateSpeedLabel();
        break;

    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlID == IDOK || dis->CtlID == IDCANCEL)
        {
            const bool ok      = (dis->CtlID == IDOK);
            const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            POINT cp; GetCursorPos(&cp); ScreenToClient(dis->hwndItem, &cp);
            const bool hot = PtInRect(&dis->rcItem, cp) != 0;
            COLORREF bg = ok ? (pressed ? RGB(40, 95, 180) : hot ? RGB(75, 135, 225) : kSecBg)
                             : (pressed ? RGB(215, 220, 230) : hot ? RGB(236, 240, 247) : RGB(255, 255, 255));
            HDC dc = dis->hDC;
            RECT rc = dis->rcItem;
            HBRUSH bgDlg = CreateSolidBrush(kDlgBg);
            FillRect(dc, &rc, bgDlg);
            DeleteObject(bgDlg);
            HBRUSH hb = CreateSolidBrush(bg);
            HPEN   hp = CreatePen(PS_SOLID, 1, ok ? bg : kSepLine);
            HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
            HPEN   op = (HPEN)SelectObject(dc, hp);
            RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(hb); DeleteObject(hp);
            WCHAR txt[64] = {};
            GetWindowTextW(dis->hwndItem, txt, 64);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ok ? RGB(255, 255, 255) : RGB(50, 50, 50));
            HFONT of = (HFONT)SelectObject(dc, (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
            DrawTextW(dc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, of);
            return TRUE;
        }
        if (dis->CtlID != IDC_PREF_SEL_COLOR || !pPrefs) break;
        RECT rc = dis->rcItem;

        // 버튼 배경 (둥근 사각형 스타일)
        HBRUSH hBtnBg = CreateSolidBrush((dis->itemState & ODS_SELECTED) ? RGB(215,220,230) : RGB(255,255,255));
        FillRect(dis->hDC, &rc, hBtnBg);
        DeleteObject(hBtnBg);

        // 테두리
        HPEN hBorderPen = CreatePen(PS_SOLID, 1, (dis->itemState & ODS_SELECTED) ? RGB(100,140,200) : kSepLine);
        HPEN oPen = (HPEN)SelectObject(dis->hDC, hBorderPen);
        HBRUSH oBr = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
        RoundRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom, 5, 5);
        SelectObject(dis->hDC, oPen); SelectObject(dis->hDC, oBr);
        DeleteObject(hBorderPen);

        // 색상 미리보기 원
        RECT colorRc = { rc.left + 8, rc.top + 4, rc.left + 24, rc.bottom - 4 };
        HBRUSH hClrBr = CreateSolidBrush(pPrefs->selectionColor);
        HPEN   hPen   = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
        oPen = (HPEN)SelectObject(dis->hDC, hPen);
        oBr  = (HBRUSH)SelectObject(dis->hDC, hClrBr);
        Ellipse(dis->hDC, colorRc.left, colorRc.top, colorRc.right, colorRc.bottom);
        SelectObject(dis->hDC, oPen); SelectObject(dis->hDC, oBr);
        DeleteObject(hClrBr); DeleteObject(hPen);

        // 버튼 텍스트
        RECT txtRc = { rc.left + 30, rc.top, rc.right - 4, rc.bottom };
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, RGB(50, 50, 50));
        DrawTextW(dis->hDC, LS(STR_PREF_COLOR_PICK), -1, &txtRc, DT_VCENTER | DT_SINGLELINE | DT_LEFT);

        if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &rc);
        return TRUE;
    }

    case WM_COMMAND:
        if (!pPrefs) break;
        switch (LOWORD(wParam))
        {
        case IDC_PREF_SEL_COLOR:
        {
            CHOOSECOLORW cc  = {};
            cc.lStructSize   = sizeof(cc);
            cc.hwndOwner     = hDlg;
            cc.rgbResult     = pPrefs->selectionColor;
            cc.lpCustColors  = s_customColors;
            cc.Flags         = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColorW(&cc))
            {
                pPrefs->selectionColor = cc.rgbResult;
                InvalidateRect(GetDlgItem(hDlg, IDC_PREF_SEL_COLOR), nullptr, TRUE);
            }
            break;
        }
        case IDOK:
        {
            // 최근 파일 개수 읽기
            if (pPrefs)
            {
                BOOL translated = FALSE;
                int val = (int)GetDlgItemInt(hDlg, IDC_PREF_RECENT_COUNT, &translated, FALSE);
                if (translated)
                    pPrefs->maxRecentFiles = std::max(1, std::min(10, val));
            }
            // 언어 선택 저장
            {
                HWND hLang = GetDlgItem(hDlg, IDC_PREF_LANG_COMBO);
                int sel = (int)SendMessageW(hLang, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel <= 2)
                    Lang_Set(static_cast<AppLang>(sel));
            }
            EndDialog(hDlg, IDOK);
            break;
        }
        case IDCANCEL: EndDialog(hDlg, IDCANCEL); break;
        }
        break;
    }
    return FALSE;
}

// ── 환경 설정 다이얼로그 표시 ─────────────────────────────────────────────────
// 반환값: IDOK 이면 g_prefs 가 업데이트된 상태, IDCANCEL 이면 변경 없음
INT_PTR ShowPreferencesDialog(HWND hWnd)
{
    AppPreferences tmp = g_prefs; // 취소 시 되돌리기 위해 복사
    INT_PTR res = DialogBoxParamW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDD_PREFERENCES),
        hWnd, PreferencesDlgProc, reinterpret_cast<LPARAM>(&tmp));
    if (res == IDOK)
        g_prefs = tmp;
    return res;
}
