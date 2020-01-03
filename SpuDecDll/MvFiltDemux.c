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

typedef struct
{
	wstring FilterType;
	mtime_t starttime;
	mtime_t endtime;
	mtime_t duration; // somewhat redundant, but for simplicity when converting between dvd timescale & mpeg timestamps
} FilterFileEntry;
// can this be moved to p_sys?
// i think there are problems due to dynamic size
static std::vector<FilterFileEntry> FilterFileArray;

struct demux_sys_t
{
	demux_t    * p_subdemux;
	bool b_videofilterEnable;
	bool b_useDVDTimeScaleForTimestamps;
	bool SpuES_Enable;
	// potentially problems of declaring this here due to dynamic size
	//std::vector<FilterFileEntry> FilterFileArray;

	int(*OriginalEsOutSend)   (es_out_t *, es_out_id_t *, block_t *);
};

static int Demux(demux_t *);
static int Control(demux_t *, int, va_list);
static bool sortByStart(const FilterFileEntry &lhs, const FilterFileEntry &rhs) { return lhs.starttime < rhs.starttime; }

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
	// First line either ignore or use to define using DVD time scale for timestamps
	// can ignore the result of this getline
	std::getline(infile, cmdline);
	// these are combinations that work:
	//  1.  stream mkv/mp4 with mpeg timestamps
	//  2.  dvd with mpeg timestamps
	//  3.  dvd with dvd time based timestamps
	// the other option of streaming mkv/mp4 with dvd time based timestamps is not supported; there's no conversion without using actual DVD
	if (cmdline == L"DVD_Timescale")
	{
		// TODO:  sanity check, make sure this demux module is using dvdnav, instead of streaming... else, illegal config.
		// if (dvdnav==true)
		{
			p_demux->p_sys->b_useDVDTimeScaleForTimestamps = true;
		}
	}
	else
	{
		p_demux->p_sys->b_useDVDTimeScaleForTimestamps = false;
	}
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

			FilterFileArray.push_back({ cmdline, starttime_mt, endtime_mt, (endtime_mt - starttime_mt) });

		}
	}
	// sort the list by start time
	std::sort(FilterFileArray.begin(), FilterFileArray.end(), sortByStart);
	//for (FilterFileEntry &n : FilterFileArray)
	//{
	//	msg_Info(p_demux, "type: %S, starttime: %lld\n", n.FilterType.c_str(), n.starttime);
	//}

}

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
	bool Local_Enable_Filters;

	Local_Enable_Filters = var_GetBool(p_demux->p_input, "Local_Enable_Filters");

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
			p_demux->p_sys->SpuES_Enable = false;
		}
		var_SetBool(p_demux->p_input, "Local_Enable_Filters", Local_Enable_Filters);
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
			if (p_demux->p_sys->SpuES_Enable == false)
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
				spu_id = 0; // seems spu id of 0 gets the one we want (ie. english); may not always be the case?
				// note: tried enabling ES for spu id directly, but that seems to not work, will rely on changing input var
				//   can maybe figure out how input is doing this later and optimize
				var_SetInteger(p_input_thread, "spu-es", (spu_id + SPU_ID_BASE));
				p_demux->p_sys->SpuES_Enable = true;
			}
		}
	}

	return VLC_SUCCESS;
}

// is there better way to grab this data?  dvd module demuxes & sends without making pointers to data available.
// only doing this to get timestamp data out of packet
static demux_t * p_LocalDemux;
static mtime_t my_es_pts;
static bool pts_changed=false;
static int MyEsOutSend(es_out_t *out, es_out_id_t *es, block_t *p_block)
{
	// cannot determine which stream since es_out_id_t is private
	// so, just grab the pts from any non-zero data, should be close enough?
	pts_changed = false;
	if (p_block->i_pts)
	{
		if (p_block->i_pts != my_es_pts)
		{
			pts_changed = true;
		}
		my_es_pts = p_block->i_pts;
	}
	p_LocalDemux->p_sys->OriginalEsOutSend(out, es, p_block);

	return VLC_SUCCESS;
}
static mtime_t GetRelativeDVDMtime(demux_t * p_demux)
{
	// this value should reflect the most recent timestamp from a stream sent out by dvd demux, so, should be closest approximation from current dvd position/time to relative mtime
	return my_es_pts;
}

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

	// not sure if there may be a better way to do this...
	// hook a custom routine for sending es packets, so we can snarf the data
	p_LocalDemux = p_demux;
	p_sys->OriginalEsOutSend = p_demux->out->pf_send;
	p_demux->out->pf_send = MyEsOutSend;

	msg_Info(p_demux, "\n\nLoading filter file.\n\n");
	LoadFilterFile(p_demux);

	// set up callback on any event change, to take action on different length or whatever needed
	var_AddCallback(p_demux->p_input, "intf-event", EventCallback, p_demux); // pass in pointer to p_demux for es control

	// Create some vars to be used by the filter decode modules
	// all modules have access to input mod, so add these vars to that context
	var_Create(p_demux->p_input, "Local_Enable_Filters", VLC_VAR_BOOL);
	var_Create(p_demux->p_input, "mute_start_time", VLC_VAR_INTEGER);
	var_Create(p_demux->p_input, "mute_end_time", VLC_VAR_INTEGER);
	var_Create(p_demux->p_input, "mute_start_time_absolute", VLC_VAR_INTEGER);
	var_Create(p_demux->p_input, "mute_end_time_absolute", VLC_VAR_INTEGER);
	var_SetBool(p_demux->p_input, "Local_Enable_Filters", false);
	var_SetInteger(p_demux->p_input, "mute_start_time", 0);
	var_SetInteger(p_demux->p_input, "mute_end_time", 0);
	var_SetInteger(p_demux->p_input, "mute_start_time_absolute", LAST_MDATE);
	var_SetInteger(p_demux->p_input, "mute_end_time_absolute", 0);

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

	var_Destroy(p_demux->p_input, "Local_Enable_Filters");
	var_Destroy(p_demux->p_input, "mute_start_time");
	var_Destroy(p_demux->p_input, "mute_end_time");
	var_Destroy(p_demux->p_input, "mute_start_time_absolute");
	var_Destroy(p_demux->p_input, "mute_end_time_absolute");

}

static int Demux( demux_t *p_demux )
{
	mtime_t timestamp;
	mtime_t target_time = 0;
	double newposition;
	int64_t length;
	es_out_t * myesout = p_demux->out;
	bool Local_Enable_Filters;
	mtime_t mute_start_time_absolute;
	mtime_t mute_end_time_absolute;
	mtime_t relative_mtime;
	mtime_t compare_value;
	bool b_empty=false;
	int returnval;
	static bool wait_for_pts_change=false;

	// call demux first
	returnval = p_demux->p_sys->p_subdemux->pf_demux(p_demux->p_sys->p_subdemux);

	// then do whatever other actions are necessary
	if (returnval != VLC_EGENERIC)
	{
		mute_start_time_absolute = var_GetInteger(p_demux->p_input, "mute_start_time_absolute");
		mute_end_time_absolute = var_GetInteger(p_demux->p_input, "mute_end_time_absolute");
		Local_Enable_Filters = var_GetBool(p_demux->p_input, "Local_Enable_Filters");

		if ((p_demux->p_sys->b_videofilterEnable == true) && (Local_Enable_Filters == true) && (wait_for_pts_change == false))
		{
			demux_Control(p_demux, DEMUX_GET_LENGTH, &length);
			demux_Control(p_demux, DEMUX_GET_TIME, &timestamp);
			relative_mtime = GetRelativeDVDMtime(p_demux);
			compare_value = timestamp;
			if (p_demux->p_sys->b_useDVDTimeScaleForTimestamps == false)
			{
				// in this case, want to compare against the relative mtime, as those values should match filter file values
				compare_value = relative_mtime;
			}
			// there's probably a better way to search, but for now, search in order through all entries until find match.
			for (FilterFileEntry &my_array_entry : FilterFileArray)
			{
				if ((compare_value > my_array_entry.starttime) && (compare_value < my_array_entry.endtime))
				{
					if (my_array_entry.FilterType == L"skip")
					{
						// stop demuxing and wait until es is empty, then jump to target
						es_out_Control(p_demux->out, ES_OUT_GET_EMPTY, &b_empty);
						if (b_empty == false)
						{
							// seems this sleep might be necessary; got this value from dvdnav DVDNAV_WAIT case
							msleep(40000);
							return returnval;  // return here and skip actual demuxing
						}

						// maybe better way to do this
						if (p_demux->p_sys->b_useDVDTimeScaleForTimestamps == false)
						{
							wait_for_pts_change = true;
						}

						// todo:  not sure if added buffer needed anymore... not sure how frequently timer tick is updated
						//    fixme... need to add ~400ms to make sure new time doesn't fall into this same window, it seems not precise
						//// also note:  duration for dvd timescale will not exactly match mpeg timestamps, in particular for longer skips
						//// might need to compensate for this
						target_time = timestamp + my_array_entry.duration + 100000; // only adding 100ms for now
						msg_Info(p_demux, "Skipping... timestamp: %lld, relativemtime: %lld, targettime: %lld, starttime: %lld, duration: %lld\n", timestamp, relative_mtime, target_time, my_array_entry.starttime, my_array_entry.duration);
						// setting position seemed to work better than time
						newposition = (double)target_time / (double)length;
						demux_Control(p_demux, DEMUX_SET_POSITION, newposition);
					}
					else if (my_array_entry.FilterType == L"blur")
					{
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
	//						input_Control(p_input_thread, INPUT_GET_PCR_SYSTEM, &pi_system, &pi_delay);
	//						mtime_time = mdate();
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
	//						mute_end_time_absolute = mtime_time + pi_delay + (my_array_entry.endtime - timestamp);
	//						mute_start_time_absolute = mtime_time + pi_delay; // writing to this var triggers audio filter to queue mute
							mute_end_time_absolute = relative_mtime + my_array_entry.duration;
							mute_start_time_absolute = relative_mtime; // writing to this var triggers audio filter to queue mute
							// must set end, first
							var_SetInteger(p_demux->p_input, "mute_end_time_absolute", mute_end_time_absolute);
							var_SetInteger(p_demux->p_input, "mute_start_time_absolute", mute_start_time_absolute);
							msg_Info(p_demux, "Muting... timestamp: %lld, relativemtime: %lld, targettime: %lld, starttime: %lld, duration: %lld\n", timestamp, relative_mtime, mute_start_time_absolute, my_array_entry.starttime, my_array_entry.duration);
						}
					}
					break; // out of for loop
				}
			}
		}
		if (pts_changed == true)
		{
			wait_for_pts_change == false;
		}
	}

	return returnval;

}

static int Control( demux_t *p_demux, int i_query, va_list args )
{
	return p_demux->p_sys->p_subdemux->pf_control(p_demux->p_sys->p_subdemux, i_query, args);
}
