#pragma once
#include <windows.h>

// 현재 문서를 Tesseract(tesseract.exe)로 OCR 하여 검색 가능한 새 PDF로 저장한다 (원본 유지).
void OcrCurrentDocument(HWND parent);
