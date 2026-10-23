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
};

// 포스터 인쇄 시 인접 조각 간 겹침(붙이기 여유) 길이(mm)
constexpr double POSTER_OVERLAP_MM = 10.0;

// 미리보기가 포함된 인쇄 대화상자. 확인 시 true.
// rotations: 페이지별 실효 회전(0~3), 인덱스 = 페이지 번호
bool ShowPrintDialog(HWND parent, FPDF_DOCUMENT doc, int pageCount, int currentPage,
					 const std::vector<int>& rotations, std::mutex& pdfiumMtx, PrintDialogResult& out);
