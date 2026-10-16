#pragma once
#include <windows.h>

// 현재 문서의 지정한 페이지 범위를 PNG/JPG 이미지로 내보낸다 (PDFium 렌더링).
void ExportPagesAsImages(HWND parent, int initialPage = 0);
