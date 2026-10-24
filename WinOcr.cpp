// ---------------------------------------------------------------
// WinOcr.cpp - Windows 내장 OCR (Windows.Media.Ocr, C++/WinRT)
// ---------------------------------------------------------------
#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <thread>
#include <cstring>
#include <cmath>
#include "WinOcr.h"

#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Globalization;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Storage::Streams;

namespace
{
	// 호출 스레드의 COM 상태와 무관하게 MTA 전용 스레드에서 실행
	// C++/WinRT가 활성화 팩토리를 전역 캐시하므로, MTA가 호출마다 해제되지 않도록 프로세스 수명 동안 유지
	template <class F>
	void RunMta(F&& fn)
	{
		static CO_MTA_USAGE_COOKIE s_mta = [] { CO_MTA_USAGE_COOKIE c = nullptr; CoIncrementMTAUsage(&c); return c; }();
		(void)s_mta;
		std::thread th([&]
		{
			bool inited = false;
			try { init_apartment(apartment_type::multi_threaded); inited = true; } catch (...) {}
			try { fn(); } catch (...) {}
			if (inited) uninit_apartment();
		});
		th.join();
	}
}

std::vector<WinOcrLang> WinOcr_Languages()
{
	std::vector<WinOcrLang> out;
	RunMta([&]
	{
		for (auto const& lang : OcrEngine::AvailableRecognizerLanguages())
		{
			WinOcrLang l;
			l.tag = lang.LanguageTag().c_str();
			l.name = lang.NativeName().c_str();
			if (l.name.empty()) l.name = lang.DisplayName().c_str();
			out.push_back(std::move(l));
		}
	});
	return out;
}

unsigned WinOcr_MaxDimension()
{
	unsigned v = 0;
	RunMta([&] { v = OcrEngine::MaxImageDimension(); });
	return v;
}

bool WinOcr_Recognize(const std::wstring& tag, const uint8_t* bgra, int w, int h, int stride, std::vector<WinOcrWord>& words)
{
	bool ok = false;
	RunMta([&]
	{
		OcrEngine engine = OcrEngine::TryCreateFromLanguage(Language(tag));
		if (!engine) return;

		const uint32_t row = (uint32_t)w * 4;
		Buffer buf(row * (uint32_t)h);
		buf.Length(row * (uint32_t)h);
		uint8_t* dst = buf.data();
		for (int y = 0; y < h; ++y)
			memcpy(dst + (size_t)y * row, bgra + (size_t)y * stride, row);
		SoftwareBitmap bmp = SoftwareBitmap::CreateCopyFromBuffer(buf, BitmapPixelFormat::Bgra8, w, h, BitmapAlphaMode::Ignore);

		OcrResult res = engine.RecognizeAsync(bmp).get();
		for (auto const& line : res.Lines())
			for (auto const& word : line.Words())
			{
				const auto r = word.BoundingRect();
				WinOcrWord wd;
				wd.left = (int)std::floor(r.X);
				wd.top = (int)std::floor(r.Y);
				wd.w = (int)std::ceil(r.X + r.Width) - wd.left;
				wd.h = (int)std::ceil(r.Y + r.Height) - wd.top;
				wd.text = word.Text().c_str();
				words.push_back(std::move(wd));
			}
		ok = true;
	});
	return ok;
}
