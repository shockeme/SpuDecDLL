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
#include <vlc_variables.h>

// tmp
extern mtime_t mute_start_time_absolute;
extern mtime_t mute_end_time_absolute;
bool Local_Enable_Filters=false;


struct demux_sys_t
{
	demux_t    * p_subdemux;
	bool b_videofilterEnable;
};

// for experimenting with accessing dvdnav p_sys
#define PS_TK_COUNT (256+256+256+8 - 0xc0)
typedef struct
{
	bool        b_configured;
	bool        b_seen;
	int         i_skip;
	int         i_id;
	int         i_next_block_flags;
	es_out_id_t *es;
	es_format_t fmt;
	mtime_t     i_first_pts;
	mtime_t     i_last_pts;

} ps_track_t;
struct dvdnav_demux_sys_t
{
	void    *dvdnav;  // pointer to dvdnav_t, which we don't have definition for

	/* */
	bool        b_reset_pcr;
	bool        b_readahead;

	struct
	{
		bool         b_created;
		bool         b_enabled;
		vlc_mutex_t  lock;
		vlc_timer_t  timer;
	} still;

	/* track */
	ps_track_t  tk[PS_TK_COUNT];
};
static inline int ps_id_to_tk(unsigned i_id)
{
	if (i_id <= 0xff)
		return i_id - 0xc0;
	else if ((i_id & 0xff00) == 0xbd00)
		return 256 - 0xC0 + (i_id & 0xff);
	else if ((i_id & 0xff00) == 0xfd00)
		return 512 - 0xc0 + (i_id & 0xff);
	else
		return 768 - 0xc0 + (i_id & 0x07);
}
// end dvdnav p_sys stuff

// filter file stuff
typedef struct
{
	wstring FilterType;
	mtime_t starttime;
	mtime_t endtime;
} FilterFileEntry;
// declare as VLC var somehow?
std::vector<FilterFileEntry> FilterFileArray;
// This routine currently hardcoded to load file named:  FilterFile.txt
bool sortByStart(const FilterFileEntry &lhs, const FilterFileEntry &rhs) { return lhs.starttime < rhs.starttime; }

static void LoadFilterFile(demux_t * p_demux)
{
	std::vector<std::wstring> filterfile;
	std::wstring filterfilename;
	std::wstring cmdline;
	std::wstring srttime;
	mtime_t starttime_mt;
	mtime_t endtime_mt;

	// get name of dvdrom
	WCHAR myDrives[105];
	WCHAR volumeName[MAX_PATH];
	WCHAR fileSystemName[MAX_PATH];
	DWORD serialNumber, maxComponentLen, fileSystemFlags;
	UINT driveType;

	FilterFileArray.clear();

	// todo:  don't bother with index file...unless get to a very large number of filter files
	// maybe just have the serial number in the filter files, for those movies
	// where dvd/filename doesn't match, can scan through filter files looking for serial number matches
	// Would prefer to just use volume name directly to identify the movie title and the filter file, but some (older?) movies don't define a useful volume name
	//  sooo, instead, will first use serial number (assuming this is the same for all DVD's of the same movie?)
	//  and will use an index file which will translate from the serial number to the movie title, which will be used to load the filter file.
	//  if none found here, will try using volume name

	// if couldn't match serial number, then try setting filename based on volume name
	if (GetVolumeInformationW(ToWide(p_demux->psz_file), volumeName, ARRAYSIZE(volumeName), &serialNumber, &maxComponentLen, &fileSystemFlags, fileSystemName, ARRAYSIZE(fileSystemName)))
	{
		msg_Info(p_demux, "  There is a CD/DVD in the drive:\n");
		msg_Info(p_demux, "  Volume Name: %S\n", volumeName);
		msg_Info(p_demux, "  Serial Number: %u\n", serialNumber);
		msg_Info(p_demux, "  File System Name: %S\n", fileSystemName);
		msg_Info(p_demux, "  Max Component Length: %lu\n", maxComponentLen);
		filterfilename = volumeName;
		filterfilename.append(L".txt");
		filterfilename.erase(std::find(filterfilename.begin(), filterfilename.end(), L':'));
	}

	// use this folder for loading files
	filterfilename = L"FilterFiles\\" + filterfilename;
	msg_Info(p_demux, "Filter file Name: %S\n", filterfilename.c_str());
	std::wifstream infile(filterfilename);
	if (!infile)
	{
		msg_Info(p_demux, "Failed to load filter file: %S\n", filterfilename);
		// at this point, should probably fail, since filter file was enabled, but file couldn't be loaded.
	}
	else
	{
		msg_Info(p_demux, "Successfully loaded filter file: %S\n", filterfilename);
	}
	
	// there's probably a better way to parse this...
	// first line is chapter num?  Is this only line formatted like this?
	// can ignore the result of this getline
	std::getline(infile, cmdline);
	// first getline gets command (mute/skip)
	while (std::getline(infile, cmdline, L';'))
	{
		if ((cmdline == L"mute") || (cmdline == L"skip"))
		{
			// next getline gets srt start time
			std::getline(infile, srttime, L' ');
			// process start time
			srttime_to_mtime(starttime_mt, srttime);

			// get & throw away intermediate content on line, then get srt end time
			std::getline(infile, srttime, L' ');
			std::getline(infile, srttime);  // remainder of line should be the srt end time

			// process endtime
			srttime_to_mtime(endtime_mt, srttime);

			FilterFileArray.push_back({ cmdline, starttime_mt, endtime_mt });

		}
	}
	// sort the list by start time
	std::sort(FilterFileArray.begin(), FilterFileArray.end(), sortByStart);
	//for (FilterFileEntry &n : FilterFileArray)
	//{
	//	msg_Info(p_demux, "type: %S, starttime: %lld\n", n.FilterType.c_str(), n.starttime);
	//}

}

static bool SpuES_Enable = false;
static int EventCallback(vlc_object_t *p_this, char const *psz_cmd,
	vlc_value_t oldval, vlc_value_t newval, void *p_data)
{
	mtime_t length;
	input_thread_t *p_input_thread = (input_thread_t*)p_this;
	demux_t * p_demux = (demux_t *)p_data;
	vlc_value_t val, count;
	VLC_UNUSED(oldval); VLC_UNUSED(p_data);
	int64_t input_event = var_GetInteger(p_input_thread, "intf-event");
	int spu_id;

	// ES pointer will change once actual movie starts
// spu id 0 seems to be the desired one (english)
// can read current one using p_input spu-es var
// can read enable disable status from 'spu' var
// seems input has ability to set es (ie. subtitle?)...or demux/interface?

	// TODO:  find better way to determine if main movie title playing?  Use this as global enable/disable for filters
	// Seems dvdnav doesn't always update title... so, will use change in length to check on change in playing main event
	if (input_event == INPUT_EVENT_LENGTH)
	{
		length = var_GetInteger(p_input_thread, "length");
		msg_Info(p_demux, "length changed: %lld", length);
		// if longer than ~15 minutes or so... then assume we've started actual movie
		if (length > 900000000)
		{
			msg_Info(p_demux, "main event!\n");
			Local_Enable_Filters = true;  // set global var to enable filters; spudec uses this
		}
		else
		{
			Local_Enable_Filters = false;  // clear global var
			SpuES_Enable = false;
		}
	}
	// ES event happens too many times prior to movie start, end up getting module loaded out of order
//	if (input_event == INPUT_EVENT_ES)
//	{
		//msg_Info(p_demux, "es changed: %lld, state: %lld", (var_GetInteger(p_input_thread, "spu-es") - SPU_ID_BASE), var_GetInteger(p_input_thread, "state"));
//	}

	// not sure better way to do this... seems position is last thing we can trigger on when movie actually starts playing?
	// maybe there's some other state variable that changes that we could trigger on instead?
	if (Local_Enable_Filters)	// this means we've started main event
	{
		// now, on first movement of position, then enable the spu filter
		if (input_event == INPUT_EVENT_POSITION)
		{
			if (SpuES_Enable == false)
			{
				//msg_Info(p_demux, "position changed.");
				// if default spu id is not off, then need to enable multiple spu
				spu_id = (var_GetInteger(p_input_thread, "spu-es") - SPU_ID_BASE);
				// if spu id not disabled (-1 - SPU_ID_BASE) and not the normal english track (0)
				if (spu_id > 0)
				{
					// can this simultaneous var be set once, at demux open?  or should be set on change to es?
					msg_Info(p_demux, "trying to enable multiple spu");
					es_out_Control(p_demux->out, ES_OUT_SET_ES_CAT_POLICY, SPU_ES, ES_OUT_ES_POLICY_SIMULTANEOUS);  // may not be helpful
				}
				// else, just enable the spu filter
				msg_Info(p_demux, "spu id: 0x%x", spu_id);
				dvdnav_demux_sys_t * dvdnav_demux_sys = (dvdnav_demux_sys_t *)p_demux->p_sys->p_subdemux->p_sys;
				spu_id = 0; // seems spu id of 0 gets the one we want (ie. english); may not always be the case?
				var_SetInteger(p_input_thread, "spu-es", (spu_id + SPU_ID_BASE));
				// below doesn't seem to work, need to set var above?
				//es_out_Control(p_demux->out, ES_OUT_SET_ES, dvdnav_demux_sys->tk[ps_id_to_tk(SPU_ID_BASE + spu_id)].es, true);
				SpuES_Enable = true;
			}
		}
	}

	return VLC_SUCCESS;
}



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

	// try changing spu 
	/// these vars are just dvd drive letter
	//msg_Info(p_demux, "psz_location: %s\n", p_demux->psz_location);
	//msg_Info(p_demux, "psz_file: %s\n", p_demux->psz_file);
	//dvdnav_demux_sys_t * dvdnav_demux_sys = (dvdnav_demux_sys_t *)p_sys->p_subdemux->p_sys;
	//es_out_Control(p_demux->out, ES_OUT_SET_ES, dvdnav_demux_sys->tk[ps_id_to_tk(0xbd20 + 2)], true);

	msg_Info(p_demux, "\n\nLoading filter file.\n\n");
	LoadFilterFile(p_demux);

	// set up callback on any change to title
	var_AddCallback(p_demux->p_input, "intf-event", EventCallback, p_demux); // pass in pointer to p_demux for es control

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
	FilterFileArray.clear();
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

	if ((p_demux->p_sys->b_videofilterEnable == true) && (Local_Enable_Filters == true))
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
				else if (my_array_entry.FilterType == L"blur")
				{
					// todo: fix this...
					// wait until input time hits the start time, otherwise it skips too early
					input_Control(p_input_thread, INPUT_GET_TIME, &inputtime);
					if (inputtime > my_array_entry.starttime)
					{
						// load blur/erase module
						//// NEED to fix this to work with other module source than dvdnav (eg. mkv?)
						// create new filter object to pass in...
					//	p_sys->p_subdemux->p_module = module_need(p_sys->p_filter, "video filter", "erase", true);
					//	if (p_sys->p_subdemux->p_module == NULL)
					//	{
					//		msg_Info(p_demux, "No MODULE! \n");
					//		return VLC_EGENERIC;
					//	}

						// unload module when finished?
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
	/*
	if ((i_query == DEMUX_SET_ES) )
	{
		 msg_Info(p_demux, "in demux control... DEMUX_SET_ES.\n");
		 if (p_demux->info.i_update)
		 {
			 msg_Info(p_demux, "in demux control.  update: %d, title: %d, seek: %d\n", p_demux->info.i_update, p_demux->info.i_title, p_demux->info.i_seekpoint);
			 }
	}
	*/
	return p_demux->p_sys->p_subdemux->pf_control(p_demux->p_sys->p_subdemux, i_query, args);
}
