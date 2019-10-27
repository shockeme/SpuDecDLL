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
#include <vlc_aout.h>
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

void SaveSoftwareBitmapToFile()
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

void FillBitMap(subpicture_data_t * SubImageData, spu_properties_t * SpuProp, int croppedheight)
{
	byte * mypixeldata = nullptr;
	const uint16_t *p_source = SubImageData->p_data;
	int i_len;
	byte i_color;
	uint16_t i_colorindx;
	UINT32 capacity = 0;
	
	// add some height on top & bottom, seems ocr lib works a little better with more space to figure things out, so, multiply by 3 to pad top & bottom
	// similar issue with width, particularly for short words, so, multiply width by 2
	bitmap = ref new SoftwareBitmap(BitmapPixelFormat::Bgra8, (SpuProp->i_width * 2), (croppedheight * 3), BitmapAlphaMode::Ignore);  

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
			for (int i_y = 0; i_y < bufferLayout.Height; i_y++)
			{
				// only draw RLE part if in middle of pic area
				if ((i_y >= croppedheight) && (i_y < (bufferLayout.Height - croppedheight)))
				{
					int firststop = bufferLayout.Width / 4;
					int secondstop = firststop + SpuProp->i_width;
					// first buffer space to support ocr, black
					for (int i_x = 0; i_x < firststop; i_x++)
					{
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 0] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 1] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 2] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 3] = (byte)0; // alpha ignore anyway
					}

					// draw actual subtitle
					// Draw until we reach the end of the line
					for (int i_x = firststop; i_x < secondstop; i_x += i_len)
					{
						// else...
						// Get the RLE part, then draw the line
						/// Todo... need to understand best color scheme, 
						//  from what i can tell, seems that color indx 0 is background
						//  color indx 1 and/or 2 is possibly inner text
						//  color indx 2 and/or 3 is possibly outline of text
						//  for now, not sure how to determine this reliably, so, using color scheme determined by parserle...
						//  except for background, will always use black.  This might cause a problem if parse rle determines a dark
						//  color for the inner text & outline.  if this is problematic, might need to revisit the way this is done
						// pi_yuv initialized in parserle
						// indx 0 is background, draw black for background
						i_colorindx = (*p_source & 0x3);
						i_color = i_colorindx ? SubImageData->pi_yuv[i_colorindx][0] : 0; // i think only element 0 needed to set color; 
						i_len = *p_source++ >> 2;
						for (int indx = 0; indx < i_len; indx++)
						{
							mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * (i_x + indx)) + 0] = i_color;
							mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * (i_x + indx)) + 1] = i_color;
							mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * (i_x + indx)) + 2] = i_color;
							mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * (i_x + indx)) + 3] = (byte)0; // alpha ignore anyway
						}
					}

					// second buffer space to support ocr, black
					for (int i_x = secondstop; i_x < bufferLayout.Width; i_x++)
					{
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 0] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 1] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 2] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 3] = (byte)0; // alpha ignore anyway
					}
				}
				else
				{
					// if top 3rd or bottom 3rd, then draw black, this buffer space was added to help OCR
					for (int i_x = 0; i_x < bufferLayout.Width; i_x++)
					{
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 0] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 1] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 2] = 0;
						mypixeldata[bufferLayout.StartIndex + (bufferLayout.Stride * i_y) + (4 * i_x) + 3] = (byte)0; // alpha ignore anyway
					}
				}
			}

		}

	}
}

wchar_t * OcrDecodeText(subpicture_data_t * SubImageData, spu_properties_t * SpuProp, decoder_sys_t *p_sys)
{
	OcrEngine^ ocrEngine = nullptr;

	InitOCRDll();
	ocrEngine = OcrEngine::TryCreateFromUserProfileLanguages();

	if (ocrEngine != nullptr)
	{
		MyOcrText = L"No Text Detected!";
		// this is cropped height from spudec
		int croppedheight = SpuProp->i_height - SubImageData->i_y_top_offset - SubImageData->i_y_bottom_offset;
		if (croppedheight > 0)
		{

			// load image
			//LoadSampleImage();
			// or, use passed in image...
			FillBitMap(SubImageData, SpuProp, croppedheight);

			// Recognize text from image.
			recogdone = 0;

			// for debug, can save image to file
			if (p_sys->b_CaptureTextPicsEnable)
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
			while(recogdone==0)
			{
			};
		}
		return (wchar_t *)MyOcrText->Data();
	}
	else
	{
		return (wchar_t *)BadTextVal;
	}
}


