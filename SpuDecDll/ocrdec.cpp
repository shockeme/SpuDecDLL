// functions for performing ocr decode

#include "stdafx.h"
#include "roapi.h"
#include <ppltasks.h>
#include "robuffer.h"
#include <wrl.h>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "spudec.h"
#include <vlc_subpicture.h>
using namespace Windows::Foundation;
using namespace Windows::System;
using namespace Windows::Media::Ocr;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::Storage::Pickers;
using namespace concurrency;
using namespace Microsoft::WRL;

// Bitmap holder of currently loaded image.
static Windows::Graphics::Imaging::SoftwareBitmap^ bitmap;

/* // this stuff for debug only
static bool DoneLoadingBitmap = false;
task<void> LoadBitmapImage(StorageFile^ file)
{	
	return create_task(file->OpenAsync(FileAccessMode::Read)).then([](IRandomAccessStream^ stream)
	{
		return BitmapDecoder::CreateAsync(stream);
	}).then([](BitmapDecoder^ decoder) -> IAsyncOperation<SoftwareBitmap^>^
	{
		return decoder->GetSoftwareBitmapAsync();
	}).then([](SoftwareBitmap^ imageBitmap)
	{
		bitmap = imageBitmap;
		DoneLoadingBitmap = true;
	});
}

void LoadSampleImage()
{
	DoneLoadingBitmap = false;
	auto getFileOp = KnownFolders::PicturesLibrary->GetFileAsync("splash-sdk.png");
	create_task(getFileOp).then([](StorageFile^ storagefile)
	{
		return LoadBitmapImage(storagefile);
	}).then([]() { });
 
	while (!DoneLoadingBitmap) {};
}
*/

static void SaveSoftwareBitmapToFile()
{
	auto getFileOp = KnownFolders::PicturesLibrary->GetFileAsync("subtitledebug.png");
	create_task(getFileOp).then([](StorageFile^ storagefile)
	{
		auto outFile = storagefile->OpenAsync(FileAccessMode::ReadWrite);
		create_task(outFile).then([](IRandomAccessStream^ stream)
		{
			// Create an encoder with the desired format
			auto encodetask = BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId, stream);
			create_task(encodetask).then([](BitmapEncoder^ encoder)
			{

				// Set the software bitmap
				encoder->SetSoftwareBitmap(bitmap);

				encoder->IsThumbnailGenerated = false;
				auto savetask = encoder->FlushAsync();
				create_task(savetask).then([]() {});
			});
		});

	});


}

static bool DllInitialized = false;
static int recogdone = 0;

//extern "C"  __declspec(dllexport) void _cdecl InitOCRDll()
void InitOCRDll() 
{
	if (!DllInitialized)
	{
		Windows::Foundation::Initialize();
		DllInitialized = true;
	}
}

static Platform::String^ MyOcrText;
const wchar_t BadTextVal[] = L"OCR engine failed to load";
MIDL_INTERFACE("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")
IMemoryBufferByteAccess : IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE GetBuffer(
		BYTE   **value,
		UINT32 *capacity
		);
};

// helper functions found to convert from yuv to rgb
static inline int clamp(int x)
{
	if (x > 255) return 255;
	if (x < 0)   return 0;
	return x;
}
#define YUV2R(y, u, v) clamp((298 * ((y)-16) + 409 * ((v)-128) + 128) >> 8)
#define YUV2G(y, u, v) clamp((298 * ((y)-16) - 100 * ((u)-128) - 208 * ((v)-128) + 128) >> 8)
#define YUV2B(y, u, v) clamp((298 * ((y)-16) + 516 * ((u)-128) + 128) >> 8)

// TODO:  is there a way to improve ocr to detect subtitle that consists of only 1 short word?
static void FillBitMap(subpicture_region_t * SpuProp, int AlternatePalette)
{
	byte * mypixeldata = nullptr;
	const uint8_t *p_source = SpuProp->p_picture->p->p_pixels;
	byte i_color;
	uint8_t i_colorindx;
	UINT32 capacity = 0;
	video_format_t *SubImageData = &SpuProp->fmt;

	// add some height on top & bottom, seems ocr lib works a little better with more space to figure things out, so, multiply by 3 to pad top & bottom
	// similar issue with width, particularly for short words, so, multiply width by 2
	bitmap = ref new SoftwareBitmap(BitmapPixelFormat::Bgra8, (SubImageData->i_width * 2), (SubImageData->i_height * 3), BitmapAlphaMode::Ignore);

	BitmapBuffer^ MyBitBuf = bitmap->LockBuffer(BitmapBufferAccessMode::ReadWrite);
	
	auto reference = MyBitBuf->CreateReference();
	ComPtr<IMemoryBufferByteAccess> bufferByteAccess;
	HRESULT result = reinterpret_cast<IInspectable*>(reference)->QueryInterface(IID_PPV_ARGS(&bufferByteAccess));

	if (result == S_OK)
	{
		result = bufferByteAccess->GetBuffer(&mypixeldata, &capacity);

		if (result == S_OK)
		{
			BitmapPlaneDescription bufferLayout = MyBitBuf->GetPlaneDescription(0);
			// init entire region with black.
			for (int i_y = 0; i_y < bufferLayout.Height; i_y++)
			{
				for (int i_x = 0; i_x < bufferLayout.Width; i_x++)
				{
					mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 0] = 0;
					mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 1] = 0;
					mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 2] = 0;
					mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 3] = (byte)0xFF; // alpha ignore anyway
				}
			}
			// write in subtitle in middle of black image
			for (int i_y = SubImageData->i_height; i_y < (bufferLayout.Height - SubImageData->i_height); i_y++)
			{
				int xstart = bufferLayout.Width / 4;
				int xstop = xstart + SubImageData->i_width;

				// draw actual subtitle
				for (int i_x = xstart, source_x = 0; i_x < xstop; i_x++, source_x++)
				{
					int source_y = i_y - SubImageData->i_height;
					unsigned char red, green, blue;
					unsigned char y, u, v;
					i_colorindx = (*(p_source + source_x + (source_y * SpuProp->p_picture->p->i_pitch)) & 0x3);
					i_color = (i_colorindx == (AlternatePalette - 1)) ? 0xFF : 0; // 0xFF is white, 0 is black
					if (SubImageData->p_palette->palette[i_colorindx][3] > 10) // 10 kinda arbitrary, just saying if mostly transparent, then skip this
					{
						y = SubImageData->p_palette->palette[i_colorindx][0];
						u = SubImageData->p_palette->palette[i_colorindx][1];
						v = SubImageData->p_palette->palette[i_colorindx][2];
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 0] = AlternatePalette ? i_color : YUV2R(y, u, v);
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 1] = AlternatePalette ? i_color : YUV2G(y, u, v);
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 2] = AlternatePalette ? i_color : YUV2B(y, u, v);
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 3] = 0xff; // alpha ignored anyway						
					}
				}
			}			
		}
	}
}

wchar_t * OcrDecodeText(subpicture_region_t * SpuProp, bool SavePicToFile)
{
	OcrEngine^ ocrEngine = nullptr;
	static int PaletteIndex = 0;
	InitOCRDll();
	ocrEngine = OcrEngine::TryCreateFromUserProfileLanguages();

	if (ocrEngine != nullptr)
	{
		MyOcrText = L"No Text Detected!";
		// some movie color palettes don't quite work with vlc scheme, maybe not yuv or something?
		// try this multiple times, with different palette options, since some combos work better than others
		// 0 uses the palette decoded from spu stream
		// 1-4 hard codes color scheme
		for (int DecodeAttempt = 0; DecodeAttempt < 5; DecodeAttempt++)
		{
			// load image, for debug only, instead of fillbitmap
			//LoadSampleImage();
			FillBitMap(SpuProp, PaletteIndex);

			// Recognize text from image.
			recogdone = 0;

			// for debug, can save image to file
			if (SavePicToFile == true)
			{
				SaveSoftwareBitmapToFile();
			}
			auto recognizeOp = ocrEngine->RecognizeAsync(bitmap);
			create_task(recognizeOp).then([ocrEngine](OcrResult^ ocrResult)
			{
				if (ocrResult->Text)
				{
					MyOcrText = ocrResult->Text;
				}
				recogdone = 1;
			});
			while (recogdone == 0)
			{
			};
			if(MyOcrText != L"No Text Detected!")
			{
				break;
			}
			// using this scheme to optimize, so it will always try decode with last successful palette indx
			PaletteIndex = (PaletteIndex+1) % 5; // indx value never higher than 5
		}
		return (wchar_t *)MyOcrText->Data();
	}
	else
	{
		return (wchar_t *)BadTextVal;
	}
}


