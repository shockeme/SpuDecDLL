/*****************************************************************************
 * demuxdump.c : Pseudo demux module for vlc (dump raw stream)
 *****************************************************************************
 * Copyright (C) 2001-2004 VLC authors and VideoLAN
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "spudec.h"

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_sout.h>
#include <vlc_modules.h>
#include <vector>
using namespace std;

// ***
// mv to common header file?
typedef struct
{
	wstring FilterType;
	mtime_t starttime;
	mtime_t endtime;
} FilterFileEntry;
// declare as VLC var somehow?
extern std::vector<FilterFileEntry> FilterFileArray;
// ****

struct demux_sys_t
{
	demux_t    * p_subdemux;
	bool b_videofilterEnable;
};

static int Demux( demux_t * );
static int Control( demux_t *, int,va_list );

/**
 * Initializes the raw dump pseudo-demuxer.
 */
int DemuxOpen( vlc_object_t * p_this )
{
    demux_t *p_demux = (demux_t*)p_this;

	demux_sys_t *p_sys = (demux_sys_t *)vlc_obj_malloc((vlc_object_t *)p_demux, sizeof(*p_sys));
	p_demux->p_sys = p_sys;

	if (unlikely(p_sys == NULL))
	{
		msg_Info(p_demux, "No MEM! \n");
		return VLC_ENOMEM;
	}
	p_sys->p_subdemux = (demux_t *)vlc_object_create(p_demux, sizeof(*p_demux));
	if (p_sys->p_subdemux == NULL)
		return VLC_EGENERIC;

	// copy contents of p_dec to subdec, excluding common stuff (in obj), which causes problems if that's overwritten
	memcpy(((char *)p_sys->p_subdemux + sizeof(p_demux->obj)), ((char *)p_demux + sizeof(p_demux->obj)), (sizeof(demux_t) - sizeof(p_demux->obj)));

	// now assign local p_sys;
	p_demux->p_sys = p_sys;

	// inherit vars
	p_sys->b_videofilterEnable = var_InheritBool(p_demux, "dvdsub-video-filter");

	// load module
	//// NEED to fix this to work with other module source than dvdnav (eg. mkv?)
	p_sys->p_subdemux->p_module = module_need(p_sys->p_subdemux, "access_demux", "dvdnav", true);
	if (p_sys->p_subdemux->p_module == NULL)
	{
		msg_Info(p_demux, "No MODULE! \n");
		return VLC_EGENERIC;
	}

	msg_Info(p_demux, "Made it HERE.... \n");

    p_demux->pf_demux = Demux;
    p_demux->pf_control = Control;
    return VLC_SUCCESS;
}

/**
 * Destroys the pseudo-demuxer.
 */
void DemuxClose( vlc_object_t *p_this )
{
	demux_t *p_demux = (demux_t *)p_this;
	demux_sys_t *sys = p_demux->p_sys;

	msg_Info(p_demux, "unloading module.... \n");

	module_unneed(sys->p_subdemux, sys->p_subdemux->p_module);
	vlc_object_release(sys->p_subdemux);
	sys->p_subdemux->p_module = NULL;
	vlc_obj_free((vlc_object_t *)p_demux, sys);
}

static inline int myControl(demux_t *p_demux, int i_query, ...)
{
	va_list args;
	int     i_result;

	va_start(args, i_query);
	i_result = Control(p_demux, i_query, args);
	va_end(args);
	return i_result;
}

#define SRT_BUF_SIZE 50
// note, srttimebuf must be passed in with size SRT_BUF_SIZE; todo: perhaps better way to pass in buffer?
static void mtime_to_srttime(char srttimebuf[SRT_BUF_SIZE], mtime_t itime_in)
{
	mtime_t n;
	mtime_t itime;
	itime = (itime_in + 500ULL) / 1000ULL;
	unsigned int milliseconds = (itime) % 1000;
	unsigned int seconds = (((itime)-milliseconds) / 1000) % 60;
	unsigned int minutes = (((((itime)-milliseconds) / 1000) - seconds) / 60) % 60;
	unsigned int hours = ((((((itime)-milliseconds) / 1000) - seconds) / 60) - minutes) / 60;
	n = sprintf_s(srttimebuf, SRT_BUF_SIZE, "%02d:%02d:%02d,%03d", hours, minutes, seconds, milliseconds);
}

static int Demux( demux_t *p_demux )
{
	mtime_t timestamp;
	mtime_t duration;
	mtime_t target_time = 0;
	audio_output_t *p_aout;
	double newposition;
	int64_t length;

	if (p_demux->p_sys->b_videofilterEnable == true)
	{
		myControl(p_demux, DEMUX_GET_TIME, &timestamp);
		// there's probably a better way to search, but for now, search in order through all entries until find match.
		for (FilterFileEntry &my_array_entry : FilterFileArray)
		{
			if ((timestamp > my_array_entry.starttime) && (timestamp < my_array_entry.endtime))
			{
				if (my_array_entry.FilterType == L"skip")
				{
					// todo:  not sure if added buffer needed anymore... not sure how frequently timer tick is updated
					//    fixme... need to add ~400ms to make sure new time doesn't fall into this same window, it seems not precise
					target_time = my_array_entry.endtime + 100000; // only adding 100ms for now
					// setting position seemed to work better than time
					myControl(p_demux, DEMUX_GET_LENGTH, &length);
					newposition = (double)target_time / (double)length;
					myControl(p_demux, DEMUX_SET_POSITION, newposition);
					return 1; // out of for loop
				}
				//TODO:  figure out how to schedule the mutes and let audio filter take care of this
				if (my_array_entry.FilterType == L"mute")
				{
					// TODO... fix mute request
					// need to map this to mtime somehow so audio filter can be used
					/*
					//msg_Info(p_input_thread, "\n\nGoing to mute... \n\n");
					//mute_status = aout_MuteGet(p_aout);
					if (var_GetInteger(p_input_thread, "my_mute_var") == 0)
					{
						duration = my_array_entry.endtime - my_array_entry.starttime;
						var_SetInteger(p_input_thread, "my_mute_var", duration);
						vlc_timer_schedule(MyFilterTimer, false, 1, 0);  // delay to 1 to initiate as soon as possible
					}
					*/
					break; // out of for loop
				}
			}

		}
	}
	return p_demux->p_sys->p_subdemux->pf_demux(p_demux->p_sys->p_subdemux);
}

static int Control( demux_t *p_demux, int i_query, va_list args )
{
	return p_demux->p_sys->p_subdemux->pf_control(p_demux->p_sys->p_subdemux, i_query, args);
}
