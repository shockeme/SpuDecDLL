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
#include <vlc_input.h>
#include <vector>
using namespace std;

// tmp
extern mtime_t mute_start_time_absolute;
extern mtime_t mute_end_time_absolute;

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
	mtime_t target_time = 0;
	double newposition;
	int64_t length;
	input_thread_t * p_input_thread = p_demux->p_input;
	mtime_t pi_system, pi_delay;
	mtime_t pts_delay;
	mtime_t mtime_time;
	mtime_t inputtime;
	mtime_t timevartime;
	es_out_t * myesout = p_demux->out;
	mtime_t estime1, estime2;

	if (p_demux->p_sys->b_videofilterEnable == true)
	{
		// this generally works, but time is not lined up with presentation time
		// if buffering gets large, then skip triggers too early, though target time on a skip seems to work as desired
		//   seems setting demux position takes effect immediately, and all the buffered data is dropped?
		// need to understanding the timing from demux/input/mdate better
		demux_Control(p_demux, DEMUX_GET_TIME, &timestamp);
		// there's probably a better way to search, but for now, search in order through all entries until find match.
		for (FilterFileEntry &my_array_entry : FilterFileArray)
		{
			if ((timestamp > my_array_entry.starttime) && (timestamp < my_array_entry.endtime))
			{
				if (my_array_entry.FilterType == L"skip")
				{
					// todo: fix this...
					// wait until input time hits the start time, otherwise it skips too early
					input_Control(p_input_thread, INPUT_GET_TIME, &inputtime);
					if (inputtime > my_array_entry.starttime)
					{
						// todo:  not sure if added buffer needed anymore... not sure how frequently timer tick is updated
						//    fixme... need to add ~400ms to make sure new time doesn't fall into this same window, it seems not precise
						target_time = my_array_entry.endtime + 100000; // only adding 100ms for now
						// setting position seemed to work better than time
						demux_Control(p_demux, DEMUX_GET_LENGTH, &length);
						newposition = (double)target_time / (double)length;
						demux_Control(p_demux, DEMUX_SET_POSITION, newposition);
					}
				}
				else if (my_array_entry.FilterType == L"mute")
				{
					// 2 var handshake with audio decoder: start indicates to queue start and end indicates mute is finished
					if ((mute_start_time_absolute == LAST_MDATE) && (mute_end_time_absolute == LAST_MDATE))
					{
						// TODO:  Need to get proper conversion from time to mtime.  for now, treating delta time same as delta mtime
						// get current mtime (assuming need to mute soon) using input thread... not sure if better way to do this
						// only get absolute time from PCR SYSTEM; don't have origin, not sure how to calculate offset
						//demux_Control(p_demux, DEMUX_GET_PTS_DELAY, &pts_delay);
						input_Control(p_input_thread, INPUT_GET_PCR_SYSTEM, &pi_system, &pi_delay);
						mtime_time = mdate();
						//input_Control(p_input_thread, INPUT_GET_TIME, &inputtime);
						//timevartime = var_GetInteger(p_input_thread, "time");
						//es_out_Control(myesout, ES_OUT_GET_PCR_SYSTEM, &estime1, &estime2);
						// here's what i can see from time vars:
						///  demux time:  relative time, from perspective of demux (ahead of input thread)
						///  demux pts:  presentation time delay from demux, this seems constant @ 300ms
						///  mdate:  absolute time value
						///  pi_system:  also absolute time value, but seems seconds different than mdate, where the diff is not constant.  don't understand...
						///  pi_delay:  delay from demux time to input thread time
						///  input time:  relative time as seen from input thread
						///  timevar:  same as input time
						///  estime1:  same as pi_system
						///  estime2:  same as pi_delay
						// possible that diff between mdate & pi_system is how to convert between time & mtime?
						//msg_Info(p_demux, "demux_time: %lld, demux_pts: %lld, mdate: %lld, pi_system: %lld, pi_delay: %lld, inputtime: %lld, timevar: %lld, estime1: %lld, estime2: %lld", timestamp, pts_delay, mtime_time, pi_system, pi_delay, inputtime, timevartime, estime1, estime2);
						// pi_system is absoluate time, note that it does not match mdate value
						mute_end_time_absolute = mtime_time + pi_delay + (my_array_entry.endtime - timestamp);
						mute_start_time_absolute = mtime_time + pi_delay; // writing to this var triggers audio filter to queue mute
					}
				}
				break; // out of for loop
			}
		}
	}
	return p_demux->p_sys->p_subdemux->pf_demux(p_demux->p_sys->p_subdemux);
}

static int Control( demux_t *p_demux, int i_query, va_list args )
{
	return p_demux->p_sys->p_subdemux->pf_control(p_demux->p_sys->p_subdemux, i_query, args);
}
