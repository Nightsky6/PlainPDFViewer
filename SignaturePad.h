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
    // bmp는 서명 부분만 잘라 낸 것 → 잘라 낸 영역의 캔버스 좌표 시작점 (획을 bmp 기준으로 맞출 때 사용)
    float cropX = 0.f;
    float cropY = 0.f;
    // 펜 (직접 그린 서명): 색과 캔버스 기준 굵기(px)
    COLORREF penColor = RGB(20, 20, 20);
    float    penWidth = 2.4f;
    bool isImage = false; // 이미지 불러오기로 생성된 서명 여부
};

SignatureResult ShowSignaturePad(HWND parent);
