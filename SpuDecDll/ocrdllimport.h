#pragma once
typedef struct
{
	int   pi_offset[2];                              /* byte offsets to data */
	uint16_t *p_data;

	/* Color information */
	bool b_palette;
	uint8_t    pi_alpha[4];
	uint8_t    pi_yuv[4][3];

	/* Auto crop fullscreen subtitles */
	bool b_auto_crop;
	int i_y_top_offset;
	int i_y_bottom_offset;

} subpicture_data_t;
typedef struct
{
	int i_width;
	int i_height;
	int i_x;
	int i_y;
} spu_properties_t;

extern "C"  __declspec(dllimport) wchar_t * _cdecl TestFunc(subpicture_data_t * SubImageData, spu_properties_t * SpuProp);
