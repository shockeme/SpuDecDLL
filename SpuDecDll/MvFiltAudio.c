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
#include <vlc_codec.h>



/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

static int  DecodeAudio(decoder_t *, block_t *);
static void Flush(decoder_t *);

/*****************************************************************************
 * decoder_sys_t : decoder descriptor
 *****************************************************************************/
struct decoder_sys_t
{
	decoder_t *p_subdec;
	bool b_audiofilterEnable;
	mtime_t saved_start_mute;
	mtime_t saved_end_mute;
};

// need to keeps these routines in this module, cause the p_dec passed in is for this module; from here, we can call the subdec module
static int DecodeAudio(decoder_t *p_dec, block_t *p_block)
{
	return p_dec->p_sys->p_subdec->pf_decode(p_dec->p_sys->p_subdec, p_block);
}

static void Flush(decoder_t *p_dec)
{
	return p_dec->p_sys->p_subdec->pf_flush(p_dec->p_sys->p_subdec);
}

static int MyDecoderQueueAudio(decoder_t *p_dec, block_t *p_aout_buf)
{
	decoder_t * my_local_p_dec = (decoder_t *)p_dec->obj.parent; // local pointer is parent of subdecoder
	mtime_t presentation_mtime;
	mtime_t mute_start_time;
	mtime_t mute_end_time;
	mtime_t mute_start_time_absolute;
	mtime_t mute_end_time_absolute;

	// this comes from decoder_QueueAudio in vlc_codec.h
	assert(p_aout_buf->p_next == NULL);
	assert(my_local_p_dec->pf_queue_audio != NULL);

	// these global vars are how demux & spu dec queue mutes with this audio decoder
	mute_start_time = var_GetInteger(my_local_p_dec->obj.parent, "mute_start_time");
	mute_end_time = var_GetInteger(my_local_p_dec->obj.parent, "mute_end_time");
	mute_start_time_absolute = var_GetInteger(my_local_p_dec->obj.parent, "mute_start_time_absolute");
	mute_end_time_absolute = var_GetInteger(my_local_p_dec->obj.parent, "mute_end_time_absolute");

	// Note:  demux sends in mute requests with absoluate time, spudec with relative time
	// would prefer to just use relative time, but don't know how to determine that in demux, yet
	if (mute_start_time_absolute != LAST_MDATE)
	{
		/*
		// check absoluate time first, and if a match, then convert to relative mtime
		presentation_mtime = decoder_GetDisplayDate(p_dec, p_aout_buf->i_pts);
		if (presentation_mtime > mute_start_time_absolute)
		{
			// convert to mtime instead (might conflict with spudec writing these vars, need to fix)
			// maybe check for overlap and just extend window of mute if there is overlap??
			mute_start_time = p_aout_buf->i_pts;
			mute_end_time = mute_start_time + (mute_end_time_absolute - mute_start_time_absolute);
			// tell demux we're finished queuing the mute
			var_SetInteger(my_local_p_dec->obj.parent, "mute_start_time_absolute", mute_start_time_absolute); 
		}
		*/
		// testing...
		my_local_p_dec->p_sys->saved_start_mute = mute_start_time_absolute;
		my_local_p_dec->p_sys->saved_end_mute = mute_end_time_absolute;
		// tell demux we're finished queuing the mute
		var_SetInteger(my_local_p_dec->obj.parent, "mute_start_time_absolute", LAST_MDATE);

	}

	// modify audio buffer before queueing
	// if time falls within mute range, then clear out buf
	if ((p_aout_buf->i_pts >= mute_start_time) && (p_aout_buf->i_pts < mute_end_time))
	{
		memset(p_aout_buf->p_buffer, 0, p_aout_buf->i_buffer);
	}
	if ((p_aout_buf->i_pts >= my_local_p_dec->p_sys->saved_start_mute) && (p_aout_buf->i_pts < my_local_p_dec->p_sys->saved_end_mute))
	{
		memset(p_aout_buf->p_buffer, 0, p_aout_buf->i_buffer);
	}
	if (p_aout_buf->i_pts > my_local_p_dec->p_sys->saved_end_mute)
	{
		// tell demux we're done muting
		var_SetInteger(my_local_p_dec->obj.parent, "mute_end_time_absolute", LAST_MDATE);
	}
	/*
	else if (mute_end_time_absolute != LAST_MDATE) 
	{
		// tell demux we're done muting
		var_SetInteger(my_local_p_dec->obj.parent, "mute_end_time_absolute", LAST_MDATE);
	}
	*/
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

	if (unlikely(p_sys == NULL))
	{
		msg_Info(p_dec, "No MEM! \n");
		return VLC_ENOMEM;
	}
	p_sys->p_subdec = (decoder_t *) vlc_object_create(p_dec, sizeof(*p_dec));
	if (p_sys->p_subdec == NULL)
		return VLC_EGENERIC;
	
	// copy contents of p_dec to subdec, excluding common stuff (in obj), which causes problems if that's overwritten
	memcpy(((char *)p_sys->p_subdec + sizeof(p_dec->obj)), ((char *)p_dec + sizeof(p_dec->obj)), (sizeof(decoder_t) - sizeof(p_dec->obj)));
	// now assign local p_sys;
	p_dec->p_sys = p_sys;

	// inherit variables
	p_sys->b_audiofilterEnable = var_InheritBool(p_dec, "dvdsub-audio-filter");

	// use local queue audtio function, so we can intercept these calls
	// can determine whether or not to do this based on input var, to enable audio filter
	if (p_sys->b_audiofilterEnable == true)
	{
		p_sys->p_subdec->pf_queue_audio = MyDecoderQueueAudio;
	}

	// load module
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

	return VLC_SUCCESS;
}

