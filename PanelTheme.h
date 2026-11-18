#pragma once
// 왼쪽 사이드바(썸네일·목차)와 오른쪽 그룹 패널이 함께 쓰는 색 팔레트.
// 평소에는 Windows 11 탐색 창 같은 밝은 테마, 야간 모드(g_darkMode)에서는 어두운 테마.
#include <windows.h>

extern bool g_darkMode;

struct PanelPalette
{
    COLORREF bg;            // 패널 배경
    COLORREF line;          // 구분선
    COLORREF hover;         // 항목 hover 배경
    COLORREF active;        // 현재/선택 항목 배경
    COLORREF accent;        // 강조색 (막대·테두리)
    COLORREF accentIcon;    // 강조색 아이콘·글리프
    COLORREF accentTextHot; // 강조된 버튼 위 글자 (hover)
    COLORREF text;
    COLORREF textHi;        // hover 중인 글자
    COLORREF textActive;    // 현재 항목 글자
    COLORREF textDim;
    COLORREF textMuted;
    COLORREF iconFaint;     // 흐린 아이콘 (빈 상태, 미연결 문서)
    COLORREF field;         // 입력 상자·고스트 버튼 배경
    COLORREF fieldLine;
    COLORREF fieldLineHot;
    COLORREF pill;          // 개수 배지 (패널 배경 위)
    COLORREF pillOnField;   // 개수 배지 (입력 상자 위)
    COLORREF btnHover;      // 아이콘 버튼 hover 배경
    COLORREF closeHot;      // 닫기 X hover 글리프
    COLORREF thumbLine;     // 썸네일 테두리
    COLORREF thumbLineHot;
    COLORREF scroll;        // 스크롤 썸
    COLORREF scrollHot;
    COLORREF warn;
    COLORREF star;
    COLORREF danger;
    COLORREF pageBadge;     // 현재 페이지 번호 배지
    COLORREF pageBadgeText;
    COLORREF card;          // 드롭다운 카드
    BYTE     badgeAlpha;    // 헤더 아이콘 배지 (강조색 투명도)
    BYTE     thumbShadow1;  // 썸네일 그림자 (바깥/안쪽)
    BYTE     thumbShadow2;
    BYTE     cardShadow;    // 드롭다운 그림자 한 겹의 투명도 단위
    BYTE     slideShadow;   // 패널 슬라이드 중 왼쪽 그림자
};

inline const PanelPalette& PanelPal()
{
    static const PanelPalette dark = {
        .bg = RGB(26, 25, 33), .line = RGB(46, 44, 58), .hover = RGB(40, 39, 52),
        .active = RGB(38, 48, 74), .accent = RGB(110, 150, 235), .accentIcon = RGB(150, 182, 250),
        .accentTextHot = RGB(205, 218, 252),
        .text = RGB(238, 237, 245), .textHi = RGB(250, 249, 255), .textActive = RGB(226, 234, 255),
        .textDim = RGB(196, 194, 210), .textMuted = RGB(152, 150, 170), .iconFaint = RGB(92, 90, 110),
        .field = RGB(34, 33, 44), .fieldLine = RGB(58, 56, 72), .fieldLineHot = RGB(78, 76, 96),
        .pill = RGB(44, 42, 56), .pillOnField = RGB(48, 46, 62), .btnHover = RGB(52, 50, 66),
        .closeHot = RGB(235, 233, 245),
        .thumbLine = RGB(52, 50, 64), .thumbLineHot = RGB(90, 88, 110),
        .scroll = RGB(78, 76, 96), .scrollHot = RGB(128, 126, 150),
        .warn = RGB(240, 170, 70), .star = RGB(246, 196, 84), .danger = RGB(240, 90, 90),
        .pageBadge = RGB(178, 192, 245), .pageBadgeText = RGB(22, 26, 46),
        .card = RGB(32, 31, 41),
        .badgeAlpha = 46, .thumbShadow1 = 40, .thumbShadow2 = 70, .cardShadow = 40, .slideShadow = 60,
    };
    static const PanelPalette light = {
        .bg = RGB(249, 249, 251), .line = RGB(225, 225, 230), .hover = RGB(236, 236, 240),
        .active = RGB(224, 234, 250), .accent = RGB(0, 95, 184), .accentIcon = RGB(0, 95, 184),
        .accentTextHot = RGB(0, 74, 150),
        .text = RGB(28, 28, 30), .textHi = RGB(0, 0, 0), .textActive = RGB(0, 62, 138),
        .textDim = RGB(78, 78, 84), .textMuted = RGB(110, 110, 118), .iconFaint = RGB(168, 168, 176),
        .field = RGB(255, 255, 255), .fieldLine = RGB(214, 214, 220), .fieldLineHot = RGB(180, 180, 188),
        .pill = RGB(232, 232, 236), .pillOnField = RGB(236, 236, 240), .btnHover = RGB(226, 226, 232),
        .closeHot = RGB(28, 28, 30),
        .thumbLine = RGB(214, 214, 220), .thumbLineHot = RGB(160, 160, 170),
        .scroll = RGB(196, 196, 202), .scrollHot = RGB(138, 138, 148),
        .warn = RGB(196, 108, 0), .star = RGB(230, 158, 0), .danger = RGB(196, 43, 28),
        .pageBadge = RGB(0, 95, 184), .pageBadgeText = RGB(255, 255, 255),
        .card = RGB(255, 255, 255),
        .badgeAlpha = 30, .thumbShadow1 = 16, .thumbShadow2 = 28, .cardShadow = 12, .slideShadow = 26,
    };
    return g_darkMode ? dark : light;
}
