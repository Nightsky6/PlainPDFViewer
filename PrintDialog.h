#pragma once
#include <windows.h>
#include <mutex>
#include "fpdfview.h"

// 인쇄 대화상자 결과 (페이지 인덱스는 0 기반)
struct PrintDialogResult
{
	HDC  hDC        = nullptr;   // 호출자가 DeleteDC 해야 함
	int  fromPage   = 0;
	int  toPage     = 0;
	int  copies     = 1;
	bool fitToPage  = true;      // false = 실제 크기(100%)
};

// 미리보기가 포함된 인쇄 대화상자. 확인 시 true.
bool ShowPrintDialog(HWND parent, FPDF_DOCUMENT doc, int pageCount, int currentPage,
					 int rotation, std::mutex& pdfiumMtx, PrintDialogResult& out);
