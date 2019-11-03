/*****************************************************************************
 * normvol.c: volume normalizer
 *****************************************************************************
 * Copyright (C) 2001, 2006 VLC authors and VideoLAN
 * $Id: 428529f146f5d1aa88cc40fc167d8a77eec49ef9 $
 *
 * Authors: Clément Stenac <zorglub@videolan.org>
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
#include "stdafx.h"

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_modules.h>

#include <vlc_aout.h>

#include <assert.h>

#include <vlc_codec.h>

#include "..\libvlc.h"

// tmp
extern mtime_t mute_start_time;
extern mtime_t mute_end_time;


/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

static void SetupOutputFormat(decoder_t *p_dec, bool b_trust);
static int  DecodeAudio(decoder_t *, block_t *);
static void Flush(decoder_t *);

/*****************************************************************************
 * decoder_sys_t : decoder descriptor
 *****************************************************************************/
struct decoder_sys_t
{
	decoder_t *p_subdec;
	bool b_audiofilterEnable;
};
static decoder_t * my_local_p_dec=NULL;

// need to keeps these routines in this module, cause the p_dec passed in is for this module; from here, we can call the subdec module
static int DecodeAudio(decoder_t *p_dec, block_t *p_block)
{
	//msg_Info(p_dec, "DecodeAudio... \n");
	return p_dec->p_sys->p_subdec->pf_decode(p_dec->p_sys->p_subdec, p_block);
}

static void Flush(decoder_t *p_dec)
{
	//msg_Info(p_dec, "Flush... \n");
	return p_dec->p_sys->p_subdec->pf_flush(p_dec->p_sys->p_subdec);
}

// note: this is called from subdecoder, so the p_dec pointer is the subdec pointer, not local one
// need to use local pointer for calling original queue audio
static int MyDecoderQueueAudio(decoder_t *p_dec, block_t *p_aout_buf)
{
	// this comes from decoder_QueueAudio in vlc_codec.h
	assert(p_aout_buf->p_next == NULL);
	assert(my_local_p_dec->pf_queue_audio != NULL);

	mtime_t starttime = var_GetInteger(my_local_p_dec, "start_mute");
	mtime_t endtime = var_GetInteger(my_local_p_dec, "end_mute");

	// modify audio buffer before queueing
	// if time falls within mute range, then clear out buf
	if ((p_aout_buf->i_pts > mute_start_time) && (p_aout_buf->i_pts < mute_end_time))
	{
		for (int indx = 0; indx < p_aout_buf->i_buffer; indx++)
		{
			p_aout_buf->p_buffer[indx] = 0;
		}
	}

	return my_local_p_dec->pf_queue_audio(p_dec, p_aout_buf);
}

/*****************************************************************************
 * EndAudio: decoder destruction
 *****************************************************************************
 * This function is called when the thread ends after a successful
 * initialization.
 *****************************************************************************/
void EndAudioDec(vlc_object_t *obj)
{
	decoder_t *p_dec = (decoder_t *)obj;
	decoder_sys_t *sys = p_dec->p_sys;
	
	msg_Info(p_dec, "unloading module.... \n");
	
	module_unneed(sys->p_subdec, sys->p_subdec->p_module);
	vlc_object_release(sys->p_subdec);
	sys->p_subdec->p_module = NULL;
	vlc_obj_free((vlc_object_t *)p_dec, sys);
	my_local_p_dec = NULL;
	var_Destroy(p_dec, "start_mute");
	var_Destroy(p_dec, "end_mute");

}

/*****************************************************************************
 * InitAudioDec: initialize audio decoder
 *****************************************************************************
 * The avcodec codec will be opened, some memory allocated.
 *****************************************************************************/
int InitAudioDec(vlc_object_t *obj)
{
	decoder_t *p_dec = (decoder_t *)obj;
	decoder_sys_t *p_sys = (decoder_sys_t *)vlc_obj_malloc((vlc_object_t *)p_dec, sizeof(*p_sys));
	p_dec->p_sys = p_sys;

	my_local_p_dec = p_dec;

	if (unlikely(p_sys == NULL))
	{
		msg_Info(p_dec, "No MEM! \n");
		return VLC_ENOMEM;
	}
	p_sys->p_subdec = (decoder_t *) vlc_object_create(p_dec, sizeof(*p_dec));
	if (p_sys->p_subdec == NULL)
		return VLC_EGENERIC;
	
	p_sys->p_subdec->fmt_in = p_dec->fmt_in;
	p_sys->p_subdec->fmt_out = p_dec->fmt_out;

	p_sys->p_subdec->p_description = p_dec->p_description;
	p_sys->p_subdec->p_owner = p_dec->p_owner;
	
	// use local queue audtio function, so we can intercept these calls
	// can determine whether or not to do this based on input var, to enable audio filter
	p_sys->b_audiofilterEnable = var_InheritBool(p_dec, "dvdsub-audio-filter");
	if (p_sys->b_audiofilterEnable == true)
	{
		p_sys->p_subdec->pf_queue_audio = MyDecoderQueueAudio;
	}
	else
	{
		p_sys->p_subdec->pf_queue_audio = p_dec->pf_queue_audio;
	}

	p_sys->p_subdec->pf_decode = p_dec->pf_decode;
	p_sys->p_subdec->pf_flush = p_dec->pf_flush;
	p_sys->p_subdec->p_queue_ctx = p_dec->p_queue_ctx;
	p_sys->p_subdec->pf_aout_format_update = p_dec->pf_aout_format_update;
	p_sys->p_subdec->b_frame_drop_allowed = p_dec->b_frame_drop_allowed;
	p_sys->p_subdec->i_extra_picture_buffers = p_dec->i_extra_picture_buffers;

	p_sys->p_subdec->pf_packetize = p_dec->pf_packetize;

	p_sys->p_subdec->pf_get_attachments = p_dec->pf_get_attachments;
	p_sys->p_subdec->pf_get_display_date = p_dec->pf_get_display_date;
	p_sys->p_subdec->pf_get_display_rate = p_dec->pf_get_display_rate;


	p_sys->p_subdec->p_module = module_need(p_sys->p_subdec, "audio decoder", "avcodec", true);
	if (p_sys->p_subdec->p_module == NULL)
	{
		msg_Info(p_dec, "No MODULE! \n");
		return VLC_EGENERIC;
	}

	msg_Info(p_dec, "Made it HERE.... \n");

	p_dec->pf_decode = DecodeAudio;
	p_dec->pf_flush = Flush;
	// this gets initialized in submodule, so let's reuse value here
	p_dec->fmt_out = p_sys->p_subdec->fmt_out;
	p_dec->fmt_in = p_sys->p_subdec->fmt_in;

	var_Create(p_dec, "start_mute", VLC_VAR_INTEGER);
	var_Create(p_dec, "end_mute", VLC_VAR_INTEGER);
	var_SetInteger(p_dec, "start_mute", 40000000);
	var_SetInteger(p_dec, "end_mute", 50000000);


	return VLC_SUCCESS;
}

