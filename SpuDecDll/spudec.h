/*****************************************************************************
 * spudec.h : sub picture unit decoder thread interface
 *****************************************************************************
 * Copyright (C) 1999, 2000, 2006 VLC authors and VideoLAN
 * $Id: ab53413bbc8625ecb173b804916797dfea579cee $
 *
 * Authors: Sam Hocevar <sam@zoy.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

// Include vlc headers
#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_input.h>
 // redefine restrict to MS friendly just for include of vlc_charset (otherwise will cause problems)
#define restrict __restrict
#include <vlc_charset.h>
#undef restrict

struct decoder_sys_t
{
    bool b_packetizer;
    bool b_disabletrans;

    mtime_t i_pts;
    unsigned int i_spu_size;
    unsigned int i_rle_size;
    unsigned int i_spu;

    block_t *p_block;

    /* We will never overflow */
    uint8_t buffer[65536];
};

/*****************************************************************************
 * Amount of bytes we GetChunk() in one go
 *****************************************************************************/
#define SPU_CHUNK_SIZE              0x200

/*****************************************************************************
 * SPU commands
 *****************************************************************************/
#define SPU_CMD_FORCE_DISPLAY       0x00
#define SPU_CMD_START_DISPLAY       0x01
#define SPU_CMD_STOP_DISPLAY        0x02
#define SPU_CMD_SET_PALETTE         0x03
#define SPU_CMD_SET_ALPHACHANNEL    0x04
#define SPU_CMD_SET_COORDINATES     0x05
#define SPU_CMD_SET_OFFSETS         0x06
#define SPU_CMD_END                 0xff

/*****************************************************************************
 * Prototypes
 *****************************************************************************/
subpicture_t * ParsePacket( decoder_t * );

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
extern wchar_t * OcrDecodeText(subpicture_data_t * SubImageData, spu_properties_t * SpuProp);
