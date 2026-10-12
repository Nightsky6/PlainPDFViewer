#pragma once
#include <windows.h>
#include <string>

// Ghostscript(AGPL) DLL을 동적으로 로드하여 PDF를 압축한다.
// inPath: 디스크에 저장된 원본 PDF 경로
void ShowCompressPdfDialog(HWND parent, const std::wstring& inPath);
