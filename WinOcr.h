// ---------------------------------------------------------------
// WinOcr.h - Windows 내장 OCR (Windows.Media.Ocr) 래퍼
// ---------------------------------------------------------------
#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct WinOcrLang
{
	std::wstring tag;  // BCP-47 (예: "ko", "en-US")
	std::wstring name; // 표시 이름 (해당 언어 고유 이름)
};

struct WinOcrWord
{
	int left = 0, top = 0, w = 0, h = 0;
	std::wstring text;
};

// 이 PC에 설치된 OCR 인식 언어 (없으면 빈 목록)
std::vector<WinOcrLang> WinOcr_Languages();

// 엔진이 받을 수 있는 이미지의 최대 가로/세로 픽셀 (실패 시 0)
unsigned WinOcr_MaxDimension();

// BGRA 32bpp 이미지 인식 (완료까지 대기)
bool WinOcr_Recognize(const std::wstring& tag, const uint8_t* bgra, int w, int h, int stride, std::vector<WinOcrWord>& words);
