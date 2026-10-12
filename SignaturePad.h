#pragma once
#include "framework.h"
#include <objbase.h>
#include <gdiplus.h>
#include <vector>

struct SigStrokePt { float x, y; };

struct SignatureResult
{
    Gdiplus::Bitmap* bmp = nullptr;
    int bmpW = 0;
    int bmpH = 0;
    // 잉크 어노테이션용 획 데이터 (캔버스 픽셀 좌표)
    std::vector<std::vector<SigStrokePt>> strokes;
    int canvasW = 0;
    int canvasH = 0;
    bool isImage = false; // 이미지 불러오기로 생성된 서명 여부
};

SignatureResult ShowSignaturePad(HWND parent);
