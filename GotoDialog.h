#pragma once
#include "framework.h"
#include <fpdfview.h>

// PlainPDFViewer.cpp 전역 참조
extern HINSTANCE     hInst;
extern FPDF_DOCUMENT g_pdfDoc;
extern int           g_currentPage;
extern int           g_pageCount;
extern bool          g_gotoModalDone;

// PlainPDFViewer.cpp 함수 참조
void ScrollToPage(HWND hWnd, int pageIndex);

// 페이지 이동 다이얼로그 표시 (Ctrl+G)
void ShowGotoPageDialog(HWND hWnd);
