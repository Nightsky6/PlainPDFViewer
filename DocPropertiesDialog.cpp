// ---------------------------------------------------------------
// DocPropertiesDialog.cpp - PDF 문서 속성 팝업 (Fluent 스타일)
// ---------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <fpdfview.h>
#include <fpdf_doc.h>
#include <string>
#include <sstream>
#include "DocPropertiesDialog.h"
#include "PlainPDFViewer.h"
#include "Lang.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

// ---------------------------------------------------------------
// 내부 상수
// ---------------------------------------------------------------
namespace
{
    constexpr int DPD_W        = 480;
    constexpr int DPD_H        = 460;
    constexpr int DPD_HEADER   = 60;
    constexpr int DPD_FOOTER   = 56;
    constexpr int DPD_MARGIN   = 24;
    constexpr int DPD_ROW_H    = 34;
    constexpr int DPD_LABEL_W  = 130;
    constexpr int DPD_COL_PAD  = 12;   // 레이블/값 열 내부 패딩
    constexpr int BTN_W        = 100;
    constexpr int BTN_H        = 36;

    const wchar_t* DPD_CLASS = L"DocPropertiesDlgClass";
    bool g_dpdClassRegistered = false;

    struct DpdButton {
        RECT rc{};
        bool hovered = false;
        bool pressed = false;
        std::wstring label;
    };

    struct DpdState {
        std::wstring title;
        std::wstring author;
        std::wstring subject;
        std::wstring creator;
        std::wstring producer;
        std::wstring creationDate;
        std::wstring modDate;
        std::wstring filePath;
        std::wstring fileSize;
        std::wstring pageCount;
        DpdButton    okBtn;
        bool         closed = false;
    } g_dpd;

    HCURSOR g_curArrow = nullptr;
    HCURSOR g_curHand  = nullptr;

    // ---------------------------------------------------------------
    // 헬퍼: 둥근 사각형 경로
    // ---------------------------------------------------------------
    Gdiplus::GraphicsPath* MakeRoundRectPath(const Gdiplus::RectF& r, float radius)
    {
        using namespace Gdiplus;
        auto* path = new GraphicsPath();
        float d = radius * 2.0f;
        path->AddArc(r.X, r.Y, d, d, 180, 90);
        path->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        path->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        path->CloseFigure();
        return path;
    }

    // ---------------------------------------------------------------
    // 헬퍼: PDF 날짜 문자열(D:YYYYMMDDHHmmSS) → 읽기 쉬운 형식
    // ---------------------------------------------------------------
    std::wstring FormatPdfDate(const std::wstring& raw)
    {
        // 형식: D:20240101120000+09'00'
        std::wstring s = raw;
        if (s.size() >= 2 && s[0] == L'D' && s[1] == L':')
            s = s.substr(2);
        if (s.size() < 8) return raw;

        std::wstring year  = s.substr(0, 4);
        std::wstring month = s.substr(4, 2);
        std::wstring day   = s.substr(6, 2);
        std::wstring hour  = s.size() >= 10 ? s.substr(8, 2)  : L"00";
        std::wstring min   = s.size() >= 12 ? s.substr(10, 2) : L"00";

        return year + L"-" + month + L"-" + day + L"  " + hour + L":" + min;
    }

    // ---------------------------------------------------------------
    // 헬퍼: FPDF 메타데이터 읽기
    // ---------------------------------------------------------------
    std::wstring GetPdfMeta(FPDF_DOCUMENT doc, const char* tag)
    {
        if (!doc) return L"";
        unsigned long len = FPDF_GetMetaText(doc, tag, nullptr, 0);
        if (len <= 2) return L"";
        std::wstring buf(len / sizeof(wchar_t), L'\0');
        FPDF_GetMetaText(doc, tag, buf.data(), len);
        // null 종결자 제거
        while (!buf.empty() && buf.back() == L'\0')
            buf.pop_back();
        return buf;
    }

    // ---------------------------------------------------------------
    // 헬퍼: 파일 크기 문자열
    // ---------------------------------------------------------------
    std::wstring GetFileSizeStr(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
            return LS(STR_DP_UNKNOWN);
        ULONGLONG sz = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        wchar_t buf[64];
        if (sz < 1024)
            swprintf_s(buf, L"%llu B", sz);
        else if (sz < 1024 * 1024)
            swprintf_s(buf, L"%.1f KB", sz / 1024.0);
        else
            swprintf_s(buf, L"%.2f MB", sz / (1024.0 * 1024.0));
        return buf;
    }

    // ---------------------------------------------------------------
    // 속성 행 그리기
    // ---------------------------------------------------------------
    void DrawRow(Gdiplus::Graphics& g, int y, const wchar_t* label,
                 const std::wstring& value, const Gdiplus::FontFamily& ff, bool isOdd)
    {
        using namespace Gdiplus;

        if (isOdd) {
            SolidBrush rowBg(Color(255, 248, 249, 252));
            g.FillRectangle(&rowBg,
                (float)DPD_MARGIN, (float)y,
                (float)(DPD_W - DPD_MARGIN * 2), (float)DPD_ROW_H);
        }

        // 레이블/값 열 경계 x 좌표
        const int colX = DPD_MARGIN + DPD_LABEL_W;

        Font labelFont(&ff, 8, FontStyleBold, UnitPoint);
        Font valueFont(&ff, 8, FontStyleRegular, UnitPoint);
        SolidBrush labelBrush(Color(255, 100, 100, 115));
        SolidBrush valueBrush(Color(255, 28, 28, 38));
        SolidBrush emptyBrush(Color(255, 180, 180, 185));

        // 레이블: 오른쪽 정렬, 세로 중앙
        StringFormat sfLabel;
        sfLabel.SetAlignment(StringAlignmentFar);
        sfLabel.SetLineAlignment(StringAlignmentCenter);
        sfLabel.SetFormatFlags(StringFormatFlagsNoWrap);
        sfLabel.SetTrimming(StringTrimmingNone);
        RectF labelRc((float)(DPD_MARGIN + 4), (float)y,
                      (float)(DPD_LABEL_W - DPD_COL_PAD - 4), (float)DPD_ROW_H);
        g.DrawString(label, -1, &labelFont, labelRc, &sfLabel, &labelBrush);

        // 세로 구분선
        Pen colPen(Color(255, 220, 220, 225), 1.0f);
        g.DrawLine(&colPen, (float)colX, (float)(y + 5),
                   (float)colX, (float)(y + DPD_ROW_H - 5));

        // 값: 왼쪽 정렬, 세로 중앙
        StringFormat sfValue;
        sfValue.SetAlignment(StringAlignmentNear);
        sfValue.SetLineAlignment(StringAlignmentCenter);
        sfValue.SetFormatFlags(StringFormatFlagsNoWrap);
        sfValue.SetTrimming(StringTrimmingEllipsisCharacter);
        RectF valueRc((float)(colX + DPD_COL_PAD), (float)y,
                      (float)(DPD_W - DPD_MARGIN - colX - DPD_COL_PAD), (float)DPD_ROW_H);
        std::wstring disp = value.empty() ? std::wstring(LS(STR_DP_NONE)) : value;
        g.DrawString(disp.c_str(), -1, &valueFont, valueRc, &sfValue,
                     value.empty() ? &emptyBrush : &valueBrush);
    }

    // ---------------------------------------------------------------
    // 팝업 렌더링
    // ---------------------------------------------------------------
    void DrawDpd(HWND hwnd, HDC hdc)
    {
        using namespace Gdiplus;

        RECT cr;
        GetClientRect(hwnd, &cr);
        int W = cr.right, H = cr.bottom;

        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // 배경
        SolidBrush bgBrush(Color(255, 255, 255, 255));
        g.FillRectangle(&bgBrush, 0.0f, 0.0f, (float)W, (float)H);

        // 테두리
        Pen borderPen(Color(255, 218, 218, 218), 1.0f);
        g.DrawRectangle(&borderPen, 0.5f, 0.5f, (float)W - 1.0f, (float)H - 1.0f);

        FontFamily ff(L"Segoe UI");

        // ── 헤더 ──────────────────────────────────────────────
        // 헤더 배경
        SolidBrush hdrBg(Color(255, 248, 249, 252));
        g.FillRectangle(&hdrBg, 0.0f, 0.0f, (float)W, (float)DPD_HEADER);

        Font titleFont(&ff, 12, FontStyleBold, UnitPoint);
        SolidBrush darkBrush(Color(255, 20, 20, 20));
        RectF titleRc((float)DPD_MARGIN, 10.0f, (float)(W - DPD_MARGIN * 2), 28.0f);
        g.DrawString(LS(STR_DP_TITLE), -1, &titleFont, titleRc, nullptr, &darkBrush);

        Font subFont(&ff, 8, FontStyleRegular, UnitPoint);
        SolidBrush grayBrush(Color(255, 130, 130, 140));
        // 파일명 표시
        size_t pos = g_dpd.filePath.find_last_of(L"\\/");
        std::wstring fname = (pos != std::wstring::npos) ? g_dpd.filePath.substr(pos + 1) : g_dpd.filePath;
        fname = LS(STR_DP_NOFILE);
        RectF subRc((float)DPD_MARGIN, 36.0f, (float)(W - DPD_MARGIN * 2), 18.0f);
        StringFormat sfSub;
        sfSub.SetFormatFlags(StringFormatFlagsNoWrap);
        sfSub.SetTrimming(StringTrimmingEllipsisPath);
        g.DrawString(fname.c_str(), -1, &subFont, subRc, &sfSub, &grayBrush);

        // 헤더 구분선
        Pen sepPen(Color(255, 228, 228, 232), 1.0f);
        g.DrawLine(&sepPen, (float)0, (float)DPD_HEADER - 1,
                   (float)W, (float)DPD_HEADER - 1);

        // ── 속성 행 ───────────────────────────────────────────
        int rowY = DPD_HEADER;
        DrawRow(g, rowY, LS(STR_DP_DOCTITLE), g_dpd.title,        ff, true);  rowY += DPD_ROW_H;
        DrawRow(g, rowY, LS(STR_DP_AUTHOR),   g_dpd.author,       ff, false); rowY += DPD_ROW_H;
        DrawRow(g, rowY, LS(STR_DP_SUBJECT),  g_dpd.subject,      ff, true);  rowY += DPD_ROW_H;
        DrawRow(g, rowY, LS(STR_DP_CREATOR),  g_dpd.creator,      ff, false); rowY += DPD_ROW_H;
        DrawRow(g, rowY, LS(STR_DP_PRODUCER), g_dpd.producer,     ff, true);  rowY += DPD_ROW_H;
        DrawRow(g, rowY, LS(STR_DP_CREATED),  g_dpd.creationDate, ff, false); rowY += DPD_ROW_H;
        DrawRow(g, rowY, LS(STR_DP_MODIFIED), g_dpd.modDate,      ff, true);  rowY += DPD_ROW_H;
        DrawRow(g, rowY, LS(STR_DP_PAGES),    g_dpd.pageCount,    ff, false); rowY += DPD_ROW_H;
        DrawRow(g, rowY, LS(STR_DP_FILESIZE), g_dpd.fileSize,     ff, true);  rowY += DPD_ROW_H;
        DrawRow(g, rowY, LS(STR_DP_FILEPATH), g_dpd.filePath,     ff, false); rowY += DPD_ROW_H;

        // 행 하단 구분선 (마지막 행 제외 — 푸터 구분선과 이중선 방지)
        Pen divPen(Color(255, 232, 232, 236), 1.0f);
        for (int i = 0; i < 9; ++i) {
            int lineY = DPD_HEADER + DPD_ROW_H * (i + 1);
            g.DrawLine(&divPen, (float)DPD_MARGIN, (float)lineY,
                       (float)(W - DPD_MARGIN), (float)lineY);
        }

        // ── 푸터 ──────────────────────────────────────────────
        float footerY = (float)(H - DPD_FOOTER);
        g.DrawLine(&sepPen, 0.0f, footerY, (float)W, footerY);

        // 확인 버튼
        const DpdButton& btn = g_dpd.okBtn;
        RectF btnRF(
            (float)btn.rc.left  + 0.5f,
            (float)btn.rc.top   + 0.5f,
            (float)(btn.rc.right  - btn.rc.left) - 1.0f,
            (float)(btn.rc.bottom - btn.rc.top)  - 1.0f
        );
        GraphicsPath* btnPath = MakeRoundRectPath(btnRF, 4.0f);
        Color topC = btn.hovered ? Color(255, 14, 148, 244) : Color(255, 0, 137, 230);
        Color botC = btn.hovered ? Color(255, 0, 100, 185)  : Color(255, 0, 108, 196);
        LinearGradientBrush grad(
            PointF(btnRF.X, btnRF.Y),
            PointF(btnRF.X, btnRF.Y + btnRF.Height),
            topC, botC);
        g.FillPath(&grad, btnPath);
        Pen hlPen(Color(55, 255, 255, 255), 1.0f);
        g.DrawLine(&hlPen, btnRF.X + 5, btnRF.Y + 1.5f,
                   btnRF.X + btnRF.Width - 5, btnRF.Y + 1.5f);
        delete btnPath;

        SolidBrush wBrush(Color(255, 255, 255, 255));
        Font btnFont(&ff, 9, FontStyleBold, UnitPoint);
        StringFormat btnSf;
        btnSf.SetAlignment(StringAlignmentCenter);
        btnSf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(btn.label.c_str(), -1, &btnFont, btnRF, &btnSf, &wBrush);
    }

    // ---------------------------------------------------------------
    // 윈도우 프로시저
    // ---------------------------------------------------------------
    LRESULT CALLBACK DpdProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP old = (HBITMAP)SelectObject(memDC, bmp);
            FillRect(memDC, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
            DrawDpd(hWnd, memDC);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, old);
            DeleteObject(bmp);
            DeleteDC(memDC);
            EndPaint(hWnd, &ps);
        }
        break;

        case WM_MOUSEMOVE:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool over = PtInRect(&g_dpd.okBtn.rc, pt) != FALSE;
            if (over != g_dpd.okBtn.hovered) {
                g_dpd.okBtn.hovered = over;
                SetCursor(over ? g_curHand : g_curArrow);
                InvalidateRect(hWnd, &g_dpd.okBtn.rc, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;

        case WM_MOUSELEAVE:
            g_dpd.okBtn.hovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case WM_LBUTTONDOWN:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PtInRect(&g_dpd.okBtn.rc, pt))
                g_dpd.okBtn.pressed = true;
        }
        break;

        case WM_LBUTTONUP:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (g_dpd.okBtn.pressed && PtInRect(&g_dpd.okBtn.rc, pt)) {
                g_dpd.closed = true;
                HWND parent = GetParent(hWnd);
                for (int a = 255; a >= 0; a -= 30) {
                    SetLayeredWindowAttributes(hWnd, 0, (BYTE)a, LWA_ALPHA);
                    UpdateWindow(hWnd);
                    Sleep(8);
                }
                ShowWindow(hWnd, SW_HIDE);
                DestroyWindow(hWnd);
                SetForegroundWindow(parent);
            }
            g_dpd.okBtn.pressed = false;
        }
        break;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
                g_dpd.closed = true;
                HWND parent = GetParent(hWnd);
                DestroyWindow(hWnd);
                SetForegroundWindow(parent);
            }
            break;

        case WM_NCHITTEST:
        {
            LRESULT hit = DefWindowProc(hWnd, msg, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hWnd, &pt);
                if (!PtInRect(&g_dpd.okBtn.rc, pt) && pt.y < DPD_HEADER)
                    return HTCAPTION;
            }
            return hit;
        }

        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
        }
        return 0;
    }

} // anonymous namespace

// ---------------------------------------------------------------
// 공개 API
// ---------------------------------------------------------------
void DocPropertiesDialog::Show(HWND parent, FPDF_DOCUMENT pdfDoc,
                                const std::wstring& filePath, int pageCount)
{
    // 메타데이터 수집
    g_dpd.title        = GetPdfMeta(pdfDoc, "Title");
    g_dpd.author       = GetPdfMeta(pdfDoc, "Author");
    g_dpd.subject      = GetPdfMeta(pdfDoc, "Subject");
    g_dpd.creator      = GetPdfMeta(pdfDoc, "Creator");
    g_dpd.producer     = GetPdfMeta(pdfDoc, "Producer");
    g_dpd.creationDate = FormatPdfDate(GetPdfMeta(pdfDoc, "CreationDate"));
    g_dpd.modDate      = FormatPdfDate(GetPdfMeta(pdfDoc, "ModDate"));
    g_dpd.filePath     = filePath;
    g_dpd.fileSize     = filePath.empty() ? L"" : GetFileSizeStr(filePath);
    if (pageCount > 0) {
        wchar_t buf[32];
        swprintf_s(buf, LS(STR_DP_PAGES_FMT), pageCount);
        g_dpd.pageCount = buf;
    } else {
        g_dpd.pageCount = L"";
    }
    g_dpd.closed = false;

    // 버튼 위치
    int btnX = (DPD_W - BTN_W) / 2;
    int btnY = DPD_H - DPD_FOOTER + (DPD_FOOTER - BTN_H) / 2;
    g_dpd.okBtn.rc      = { btnX, btnY, btnX + BTN_W, btnY + BTN_H };
    g_dpd.okBtn.label   = LS(STR_CLOSE);
    g_dpd.okBtn.hovered = false;
    g_dpd.okBtn.pressed = false;

    // 커서
    g_curArrow = LoadCursor(nullptr, IDC_ARROW);
    g_curHand  = LoadCursor(nullptr, IDC_HAND);

    // 윈도우 클래스 등록 (최초 1회)
    if (!g_dpdClassRegistered) {
        WNDCLASSEXW wcex = {};
        wcex.cbSize        = sizeof(wcex);
        wcex.style         = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc   = DpdProc;
        wcex.hInstance     = hInst;
        wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = nullptr;
        wcex.lpszClassName = DPD_CLASS;
        RegisterClassExW(&wcex);
        g_dpdClassRegistered = true;
    }

    // 부모 중앙에 배치
    RECT parentRc;
    GetWindowRect(parent, &parentRc);
    int x = parentRc.left + (parentRc.right  - parentRc.left - DPD_W) / 2;
    int y = parentRc.top  + (parentRc.bottom - parentRc.top  - DPD_H) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        DPD_CLASS, nullptr, WS_POPUP,
        x, y, DPD_W, DPD_H,
        parent, nullptr, hInst, nullptr
    );
    if (!hwnd) return;

    // Windows 11 둥근 모서리
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // 페이드 인
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    AnimateWindow(hwnd, 180, AW_BLEND);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    SetFocus(hwnd);

    // 모달 메시지 루프
    MSG msg2;
    while (!g_dpd.closed && GetMessage(&msg2, nullptr, 0, 0))
    {
        if (msg2.hwnd == parent || IsChild(parent, msg2.hwnd))
        {
            if ((msg2.message >= WM_MOUSEFIRST && msg2.message <= WM_MOUSELAST) ||
                (msg2.message >= WM_KEYFIRST   && msg2.message <= WM_KEYLAST)   ||
                msg2.message == WM_NCLBUTTONDOWN || msg2.message == WM_NCRBUTTONDOWN)
            {
                if (msg2.message == WM_LBUTTONDOWN || msg2.message == WM_NCLBUTTONDOWN)
                    FlashWindow(hwnd, TRUE);
                continue;
            }
        }
        TranslateMessage(&msg2);
        DispatchMessage(&msg2);
    }
}
