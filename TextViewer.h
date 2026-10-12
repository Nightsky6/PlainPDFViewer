#pragma once
#include "framework.h"
#include <fpdfview.h>
#include <string>

// PlainPDFViewer.cpp 전역 참조
extern HINSTANCE    hInst;
extern FPDF_DOCUMENT g_pdfDoc;
extern int           g_pageCount;

// 컨트롤 ID
#define IDC_TV_COPY         2002
#define IDC_TV_PAGE_FROM    2003
#define IDC_TV_PAGE_TO      2004
#define IDC_TV_APPLY        2005
#define IDC_TV_CLOSE        2006

// PDF 페이지에서 텍스트 추출 (스마트 줄 병합)
std::wstring ExtractSmartText(FPDF_DOCUMENT doc, int pageIndex);

// 텍스트 뷰어 창 표시 (창을 먼저 열고 내부에서 텍스트 로딩)
void ShowTextViewer(HWND hParent, int pageNum);
