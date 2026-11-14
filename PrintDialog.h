#pragma once
#include <windows.h>
#include <mutex>
#include <vector>
#include "fpdfview.h"

// 인쇄 대화상자 결과 (페이지 인덱스는 0 기반)
struct PrintDialogResult
{
	HDC  hDC        = nullptr;   // 호출자가 DeleteDC 해야 함
	int  fromPage   = 0;
	int  toPage     = 0;
	int  copies     = 1;
	bool fitToPage  = true;      // false = 실제 크기(100%)
	bool poster     = false;     // true = 한 페이지를 여러 장에 나눠 인쇄(포스터)
	int  posterCols = 2;
	int  posterRows = 2;
	bool twoUp      = false;     // true = 한 장에 두 페이지씩 (2쪽 모아찍기)
};

// 포스터 인쇄 시 인접 조각 간 겹침(붙이기 여유) 길이(mm)
constexpr double POSTER_OVERLAP_MM = 10.0;

// 2쪽 모아찍기 배치: 인쇄 가능 영역(areaW×areaH)을 긴 쪽으로 반씩 나눈 칸(slot 0 = 왼쪽/위,
// 1 = 오른쪽/아래)에 페이지(pageW×pageH, 영역과 같은 단위)를 비율을 지켜 가운데 맞춘 위치.
// 미리보기와 실제 인쇄가 같은 계산을 쓴다.
inline void TwoUpCell(int slot, double areaW, double areaH, double pageW, double pageH,
					  double& x, double& y, double& w, double& h)
{
	const bool sideBySide = areaW >= areaH;
	const double gap = (areaW < areaH ? areaW : areaH) * 0.02;
	double cx = 0, cy = 0, cw = areaW, ch = areaH;
	if (sideBySide) { cw = (areaW - gap) / 2; cx = slot ? cw + gap : 0; }
	else            { ch = (areaH - gap) / 2; cy = slot ? ch + gap : 0; }
	const double sx = cw / pageW, sy = ch / pageH;
	const double sc = sx < sy ? sx : sy;
	w = pageW * sc; h = pageH * sc;
	x = cx + (cw - w) / 2; y = cy + (ch - h) / 2;
}

// 미리보기가 포함된 인쇄 대화상자. 확인 시 true.
// rotations: 페이지별 실효 회전(0~3), 인덱스 = 페이지 번호
bool ShowPrintDialog(HWND parent, FPDF_DOCUMENT doc, int pageCount, int currentPage,
					 const std::vector<int>& rotations, std::mutex& pdfiumMtx, PrintDialogResult& out);
