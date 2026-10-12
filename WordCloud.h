#pragma once
#include <windows.h>
#include "fpdfview.h"

// Word Cloud dialog
// hParent  : parent window handle
// doc      : currently open PDFium document (shows error if nullptr)
// pageCount: total page count
void ShowWordCloudDialog(HWND hParent, FPDF_DOCUMENT doc, int pageCount);
