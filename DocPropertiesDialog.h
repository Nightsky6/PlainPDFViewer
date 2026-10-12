#pragma once
// ---------------------------------------------------------------
// DocPropertiesDialog - PDF 문서 속성 표시 (Fluent 스타일)
// ---------------------------------------------------------------
#include <windows.h>
#include <string>
#include <fpdfview.h>

namespace DocPropertiesDialog
{
    // 팝업 표시 (g_hInstance 로 초기화된 CleanPopup::Init 이후 호출)
    void Show(HWND parent, FPDF_DOCUMENT pdfDoc, const std::wstring& filePath, int pageCount);
}
