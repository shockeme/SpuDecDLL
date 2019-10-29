/*****************************************************************************
 * spudec.c : SPU decoder thread
 *****************************************************************************
 * Copyright (C) 2000-2001, 2006 VLC authors and VideoLAN
 * $Id: 09aeb376e8c277c5bbfaee7da1a8febcd0e254f0 $
 *
 * Authors: Sam Hocevar <sam@zoy.org>
 *          Laurent Aimar <fenrir@via.ecp.fr>
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

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <wctype.h>
using namespace std;

#include "vlc_plugin.h"
#include "spudec.h"

#include <vlc_common.h>
// including input header cause we are using some functions from input module, the parent of this module
#include <vlc_input.h> 
#include <vlc_aout.h>

//#include <..\libvlc.h>
// Note, future version of vlc (4.0?) has new time macros, probably won't need these locally defined
// this definition taken from libvlc.h
typedef int64_t libvlc_time_t;
// note... these copied from libvlc_internal.h
static inline libvlc_time_t from_mtime(mtime_t time)
{
	return (time + 500ULL) / 1000ULL;
}

static inline mtime_t to_mtime(libvlc_time_t time)
{
	return time * 1000ULL;
}


// i think this is defined in newer version of vlc, but for now, define here:
vlc_object_t * vlc_object_parent(vlc_object_t * myobj)
{
	return myobj->obj.parent;
}




// filter stuff
static bool ParseForWords(std::wstring sentence);

#define SRT_BUF_SIZE 50
// note, srttimebuf must be passed in with size SRT_BUF_SIZE; todo: perhaps better way to pass in buffer?
static void vlctime_to_srttime(char srttimebuf[SRT_BUF_SIZE], libvlc_time_t itime)
{
	//static char srttimebuf[SRT_BUF_SIZE];
	libvlc_time_t n;
	unsigned int milliseconds = (itime) % 1000;
	unsigned int seconds = (((itime)-milliseconds) / 1000) % 60;
	unsigned int minutes = (((((itime)-milliseconds) / 1000) - seconds) / 60) % 60;
	unsigned int hours = ((((((itime)-milliseconds) / 1000) - seconds) / 60) - minutes) / 60;
	n = sprintf_s(srttimebuf, SRT_BUF_SIZE, "%02d:%02d:%02d,%03d", hours, minutes, seconds, milliseconds);
}

static void srttime_to_vlctime(libvlc_time_t &itime, std::wstring srttime)
{
	// format of srttime is:  hh:mm:ss,ms. <== 3 digits of ms
	// might be a better way of doing this... but, for now... just change : and , to space to help with parsing
	srttime[2] = ' ';
	srttime[5] = ' ';
	srttime[8] = ' ';
	std::wstringstream iss(srttime);
	unsigned int hours;
	unsigned int minutes;
	unsigned int seconds;
	unsigned int milliseconds;
	if (!(iss >> hours >> minutes >> seconds >> milliseconds)) { return; } // error
	// stuff below doing reverse of calculations in vlctime_to_srttime
	itime = (((((hours * 60) + minutes) * 60) + seconds) * 1000) + milliseconds;

}


typedef struct
{
	wstring FilterType;
	mtime_t starttime;
	mtime_t endtime;
} FilterFileEntry;
static std::vector<FilterFileEntry> FilterFileArray;



int framenumber = 0;

static vlc_timer_t MyFilterTimer;

static void MyFilterMuteFunc(void *p_this)
{
	input_thread_t *p_input_thread = (input_thread_t *)p_this;
	mtime_t timestamp;
	mtime_t duration;
	mtime_t target_time = 0;
	audio_output_t *p_aout;

	duration = var_GetInteger(p_input_thread, "my_mute_var");
	if (!input_Control(p_input_thread, INPUT_GET_AOUT, &p_aout))
	{
		// mute immediately
		aout_MuteSet(p_aout, TRUE);
		//mwait(my_array_entry.endtime);
		msleep(duration);
		aout_MuteSet(p_aout, FALSE);
		vlc_object_release(p_aout);
	}
	// clear mute flag before leaving
	var_SetInteger(p_input_thread, "my_mute_var", 0);
}

// Will execute the filters loaded in FilterFile.txt at the appropriate time
// NOTE:  this callback is blocking, so cannot delay too long in here
static int MyTimeCallback(vlc_object_t *p_this, char const *psz_cmd,
	vlc_value_t oldval, vlc_value_t newval, void *p_data)
{
	//input_thread_t *p_input_thread = (input_thread_t *)vlc_object_parent(VLC_OBJECT(p_this));
	input_thread_t *p_input_thread = (input_thread_t*)p_this;
	mtime_t timestamp;
	mtime_t duration;
	mtime_t target_time = 0;
	audio_output_t *p_aout;
	VLC_UNUSED(psz_cmd); VLC_UNUSED(oldval); VLC_UNUSED(p_data);
	static bool schedule_skip = false;
	double newposition;
	int64_t length;

	if (newval.i_int == INPUT_EVENT_POSITION)
	{
		input_Control(p_input_thread, INPUT_GET_TIME, &timestamp);

		// there's probably a better way to search, but for now, search in order through all entries until find match.
		for (FilterFileEntry &my_array_entry : FilterFileArray)
		{
			if ((timestamp > my_array_entry.starttime) && (timestamp < my_array_entry.endtime))
			{
				if (my_array_entry.FilterType == L"skip")
				{
					//msg_Info(p_input_thread, "\n\nGoing to set new position... \n\n");
					// todo:  fixme... need to add ~400ms to make sure new time doesn't fall into this same window, it seems not precise
					target_time = my_array_entry.endtime + to_mtime(400); 
					// set time var doesn't work, movie hangs, so setting position for now
					// this may change if moved to demux module
					//input_Control(p_input_thread, INPUT_SET_TIME, target_time);
					input_Control(p_input_thread, INPUT_GET_LENGTH, &length);
					newposition = (double) target_time / (double) length;
					input_Control(p_input_thread, INPUT_SET_POSITION, newposition);
					break; // out of for loop

				}
				//TODO:  figure out how to schedule the mutes and let audio filter take care of this
				if (my_array_entry.FilterType == L"mute")
				{
					//msg_Info(p_input_thread, "\n\nGoing to mute... \n\n");
					if (var_GetInteger(p_input_thread, "my_mute_var") == 0)
					{
						duration = my_array_entry.endtime - my_array_entry.starttime;
						var_SetInteger(p_input_thread, "my_mute_var", duration);
						vlc_timer_schedule(MyFilterTimer, false, 1, 0);  // delay to 1 to initiate as soon as possible
					}
					break; // out of for loop
				}
			}

		}
	}
	return VLC_SUCCESS;
}



/*****************************************************************************
* ParseForWords: parse sentence and return TRUE if a cuss word is found
* TODO: need to get a text file parsed at the start of the movie and then store
* the words into an array for this function
*****************************************************************************/
static std::vector<std::wstring> badwords;

// This expects words to be listed 1 on each line with or without wildcard.  With no wildcards, then it will only match on the exact word
// example contents for filter_words.txt:
//  badword_a
//  *badword_b
//  badword_c*
//  *badword_d*
static void LoadWords()
{
	// Todo:  change input file format, or somehow obfuscate the contents
	std::wifstream infile("filter_words.txt");
	std::wstring line;
	while (std::getline(infile, line))
	{
		// todo:  should also check if string is not empty but has only white space, and ignore that line...
		if (!line.empty())
		{
			// each string from subtitle will be parsed and space will be inserted for every non alpha character in the string so we can reliably filter on specific words
			// here we will default to adding space at front/end of word so we match on the exact word
			// however, if first or last char is *, then don't put space at front/end; else put space at front/end so we only search this word
			wstring FirstChar = L" ";
			wstring LastChar = L" ";
			if (line.front() == L'*')
			{
				line.erase(0, 1);
				FirstChar = L"";
			}
			if (line.back() == L'*')
			{
				line.pop_back();
				LastChar = L"";
			}
			badwords.push_back(FirstChar + line + LastChar);
		}
	}
}

// This will return true if it matches a badword in sentence
static bool ParseForWords(std::wstring sentence)
{
	size_t i;
	size_t sentenceindx;
	for (i = 0; i < badwords.size(); i++)
	{
		// replace all non alpha characters, including start & end of line with space
		// todo: is this OK?  it's replacing all non letters, including ', which will split contractions.
		// debug... seems this is failing on a string that doesn't decode properly;  Not sure if we can solve decode problem, but this shouldn't fail
		for (sentenceindx = 0; sentenceindx < sentence.size(); sentenceindx++)
		{
			if (!iswalpha(sentence[sentenceindx]))
			{
				sentence[sentenceindx] = ' ';
			}
		}
		sentence.insert(0, 1, L' ');
		sentence.push_back(L' ');
		if (sentence.find(badwords[i]) != string::npos)
		{
			return TRUE;
		}
	}
	return FALSE;
}


// This routine currently hardcoded to load file named:  FilterFile.txt
bool sortByStart(const FilterFileEntry &lhs, const FilterFileEntry &rhs) { return lhs.starttime < rhs.starttime; }

static void LoadFilterFile(decoder_t *p_dec)
{
	std::vector<std::wstring> filterfile;
	std::wstring filterfilename;
	std::wstring cmdline;
	std::wstring srttime;
	libvlc_time_t starttime;
	libvlc_time_t endtime;

	// get name of dvdrom
	WCHAR myDrives[105];
	WCHAR volumeName[MAX_PATH];
	WCHAR fileSystemName[MAX_PATH];
	DWORD serialNumber, maxComponentLen, fileSystemFlags;
	UINT driveType;

	if (!GetLogicalDriveStringsW(ARRAYSIZE(myDrives) - 1, myDrives))
	{
		msg_Info(p_dec, "GetLogicalDrives() failed with error code: %lu\n", GetLastError());
	}
	else
	{
		msg_Info(p_dec, "This machine has the following logical drives:\n");

		for (LPWSTR drive = myDrives; *drive != 0; drive += 4)
		{
			driveType = GetDriveTypeW(drive);
			msg_Info(p_dec, "Drive %s is type %d - ", drive, driveType);

			if (driveType == DRIVE_CDROM)
			{
				if (GetVolumeInformationW(drive, volumeName, ARRAYSIZE(volumeName), &serialNumber, &maxComponentLen, &fileSystemFlags, fileSystemName, ARRAYSIZE(fileSystemName)))
				{
					msg_Info(p_dec, "  There is a CD/DVD in the drive:\n");
					msg_Info(p_dec, "  Volume Name: %S\n", volumeName);
					msg_Info(p_dec, "  Serial Number: %u\n", serialNumber);
					msg_Info(p_dec, "  File System Name: %S\n", fileSystemName);
					msg_Info(p_dec, "  Max Component Length: %lu\n", maxComponentLen);
				}
				else
				{
					msg_Info(p_dec, "  There is NO CD/DVD in the drive");
				}
			}
		}
	}

	// Would prefer to just use volume name directly to identify the movie title and the filter file, but some (older?) movies don't define a useful volume name
	//  sooo, instead, will first use serial number (assuming this is the same for all DVD's of the same movie?)
	//  and will use an index file which will translate from the serial number to the movie title, which will be used to load the filter file.
	//  if none found here, will try using volume name
	std::wifstream indxinfile("FilterFiles\\FilterFile_Indx.txt");
	if (!indxinfile)
	{
		msg_Info(p_dec, "Failed to load indx file. \n");
	}
	else
	{
		msg_Info(p_dec, "Successfully loaded indx file. \n");
	}

	wstring indxline;
	wstring indxserialnumber_str;
	unsigned int indxserialnumber;
	wstring movietitle;
	while (std::getline(indxinfile, indxline))
	{
		std::wstringstream iss(indxline);
		if (!(iss >> indxserialnumber >> movietitle))
		{
			msg_Info(p_dec, " WHAT HAPPENED with indx file load???!!!!\n");
			break;
		} // error
		if (indxserialnumber == serialNumber)
		{
			msg_Info(p_dec, "  IndxSerial Number: %u ; Movie title: %S\n", indxserialnumber, movietitle.c_str());
			filterfilename = movietitle + L".txt";
			break;
		} // found our match
	}
	// if couldn't match serial number, then try setting filename based on volume name
	if (filterfilename.empty())
	{
		filterfilename = volumeName;
		filterfilename.append(L".txt");
		filterfilename.erase(std::find(filterfilename.begin(), filterfilename.end(), L':'));
	}

	// use this folder for loading files
	filterfilename = L"FilterFiles\\" + filterfilename;
	msg_Info(p_dec, "Filter file Name: %S\n", filterfilename.c_str());
	std::wifstream infile(filterfilename);
	if (!infile)
	{
		msg_Info(p_dec, "Failed to load filter file: %S\n", filterfilename);
		// at this point, should probably fail, since filter file was enabled, but file couldn't be loaded.
	}
	else
	{
		msg_Info(p_dec, "Successfully loaded filter file: %S\n", filterfilename);
	}

	mtime_t starttime_mt;
	mtime_t endtime_mt;

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
			srttime_to_vlctime(starttime, srttime);
			starttime_mt = to_mtime(starttime);

			// get & throw away intermediate content on line, then get srt end time
			std::getline(infile, srttime, L' ');
			std::getline(infile, srttime);  // remainder of line should be the srt end time

			// process endtime
			srttime_to_vlctime(endtime, srttime);
			endtime_mt = to_mtime(endtime);

			FilterFileArray.push_back({ cmdline, starttime_mt, endtime_mt });

		}
	}
	// sort the list by start time
	std::sort(FilterFileArray.begin(), FilterFileArray.end(), sortByStart);
	//for (FilterFileEntry &n : FilterFileArray)
	//{
	//	msg_Info(p_dec, "type: %S, starttime: %lld\n", n.FilterType.c_str(), n.starttime);
	//}

}








/*****************************************************************************
 * Module descriptor.
 *****************************************************************************/
static int  DecoderOpen   ( vlc_object_t * );
static int  PacketizerOpen( vlc_object_t * );
static void Close         ( vlc_object_t * );

#define DVDSUBTRANS_DISABLE_TEXT N_("Disable DVD subtitle transparency")
#define DVDSUBTRANS_DISABLE_LONGTEXT N_("Removes all transparency effects " \
                                        "used in DVD subtitles.")

#define DVDSUBVID_FLT_DISABLE_TEXT N_("DVD Video filter")
#define DVDSUBVID_FLT_DISABLE_LONGTEXT N_("Enables or Disables DVD Video filter.")

#define DVDSUBAUDIO_FLT_DISABLE_TEXT N_("DVD Audio filter")
#define DVDSUBAUDIO_FLT_DISABLE_LONGTEXT N_("Enables or Disables DVD Audio filter")

#define DVDSUBAUDIO_RENDER_TEXT N_("Enabling rendering of subtitles")
#define DVDSUBAUDIO_SUB_TO_FILE_TEXT N_("Save subtitle text to file")
#define DVDSUBAUDIO_SAVE_SUB_PIC_TEXT N_("Save pic of subtitle")

vlc_module_begin ()
    set_description( N_("Movie filter") )
    set_shortname( N_("Movie filter") )
    set_capability( "spu decoder", 100 )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_SCODEC )
    set_callbacks( DecoderOpen, Close )

    add_bool( "dvdsub-transparency", false,
              DVDSUBTRANS_DISABLE_TEXT, DVDSUBTRANS_DISABLE_LONGTEXT, true )
	add_bool("dvdsub-video-filter", true,
		DVDSUBVID_FLT_DISABLE_TEXT, DVDSUBVID_FLT_DISABLE_LONGTEXT, true)
	add_bool("dvdsub-audio-filter", true,
		DVDSUBAUDIO_FLT_DISABLE_TEXT, DVDSUBAUDIO_FLT_DISABLE_LONGTEXT, true)
	add_bool("dvdsub-render-enable", false,
		DVDSUBAUDIO_RENDER_TEXT, DVDSUBAUDIO_RENDER_TEXT, true)
	add_bool("dvdsub-text-to-file-enable", false,
		DVDSUBAUDIO_SUB_TO_FILE_TEXT, DVDSUBAUDIO_SUB_TO_FILE_TEXT, true)
	add_bool("dvdsub-save-text-pic-enable", false,
		DVDSUBAUDIO_SAVE_SUB_PIC_TEXT, DVDSUBAUDIO_SAVE_SUB_PIC_TEXT, true)
	add_submodule ()
    set_description( N_("DVD subtitles packetizer") )
    set_capability( "packetizer", 50 )
    set_callbacks( PacketizerOpen, Close )
vlc_module_end ()

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static block_t *      Reassemble( decoder_t *, block_t * );
static int            Decode    ( decoder_t *, block_t * );
static block_t *      Packetize ( decoder_t *, block_t ** );


/*****************************************************************************
 * DecoderOpen
 *****************************************************************************
 * Tries to launch a decoder and return score so that the interface is able
 * to chose.
 *****************************************************************************/

#define FILTER_TIME_PERIOD 100000
static int DecoderOpen( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys;

	if( p_dec->fmt_in.i_codec != VLC_CODEC_SPU )
        return VLC_EGENERIC;

    p_dec->p_sys = p_sys = (decoder_sys_t *) malloc( sizeof( decoder_sys_t ) );

    p_sys->b_packetizer = false;
    p_sys->b_disabletrans = var_InheritBool( p_dec, "dvdsub-transparency" );
	p_sys->b_videofilterEnable = var_InheritBool(p_dec, "dvdsub-video-filter");
	p_sys->b_audiofilterEnable = var_InheritBool(p_dec, "dvdsub-audio-filter");
	p_sys->b_RenderEnable = var_InheritBool(p_dec, "dvdsub-render-enable");
	p_sys->b_DumpTextToFileEnable = var_InheritBool(p_dec, "dvdsub-text-to-file-enable");
	p_sys->b_CaptureTextPicsEnable = var_InheritBool(p_dec, "dvdsub-save-text-pic-enable");

	p_sys->i_spu_size = 0;
    p_sys->i_spu      = 0;
    p_sys->p_block    = NULL;

    p_dec->fmt_out.i_codec = VLC_CODEC_SPU;

    p_dec->pf_decode    = Decode;
    p_dec->pf_packetize = NULL;

	// filter stuff
	// Todo:  Enable this only once the movie has started, else it impacts dvd menus
	// Todo:  should maybe move this outside of parsepackets
	if (p_sys->b_audiofilterEnable)
	{
		msg_Info(p_dec, "\n\nLoading words file.\n\n");
		LoadWords();
	}
	
	input_thread_t *p_input_thread = (input_thread_t *)vlc_object_parent(VLC_OBJECT(p_dec));
	// this is for muting, perhaps will eventually be defined in audio filter module?
	var_Create(p_input_thread, "my_mute_var", VLC_VAR_INTEGER);
	var_SetInteger(p_input_thread, "my_mute_var", 0);
	// timer will be used to spawn mute activity in callback; should eventually be done with audio filter?
	vlc_timer_create(&MyFilterTimer, &MyFilterMuteFunc, p_input_thread);
	if (p_sys->b_videofilterEnable)
	{
		msg_Info(p_dec, "\n\nLoading filter file.\n\n");
		LoadFilterFile(p_dec);
		
		msg_Info(p_dec, "\n\nAdding filter callback\n\n");
		var_AddCallback(p_input_thread, "intf-event", MyTimeCallback, NULL);
	}

    return VLC_SUCCESS;
}

/*****************************************************************************
 * PacketizerOpen
 *****************************************************************************
 * Tries to launch a decoder and return score so that the interface is able
 * to chose.
 *****************************************************************************/
static int PacketizerOpen( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;

    if( DecoderOpen( p_this ) )
    {
        return VLC_EGENERIC;
    }
    p_dec->pf_packetize  = Packetize;
    p_dec->p_sys->b_packetizer = true;
    es_format_Copy( &p_dec->fmt_out, &p_dec->fmt_in );
    p_dec->fmt_out.i_codec = VLC_CODEC_SPU;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

	// filter cleanup stuff
	input_thread_t *p_input_thread = (input_thread_t *)vlc_object_parent(VLC_OBJECT(p_dec));
	if (p_sys->b_videofilterEnable)
	{
		msg_Info(p_dec, "\n\nremoving filter callback\n\n");
		var_DelCallback(p_input_thread, "intf-event", MyTimeCallback, NULL);
		FilterFileArray.clear();
	}

    if( p_sys->p_block )
    {
        block_ChainRelease( p_sys->p_block );
    }

    free( p_sys );
}

/*****************************************************************************
 * Decode:
 *****************************************************************************/


static mtime_t mdateprevious=0;
static mtime_t timeprevious=0;
static int Decode( decoder_t *p_dec, block_t *p_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t       *p_spu_block;
    subpicture_t  *p_spu;
	std::wstring subtitle_text;
	audio_output_t *p_aout;
	input_thread_t *p_input_thread = (input_thread_t *)vlc_object_parent(VLC_OBJECT(p_dec));
	
	mtime_t mute_start, mute_stop;
	mtime_t duration;


    if( p_block == NULL ) /* No Drain */
        return VLCDEC_SUCCESS;
    p_spu_block = Reassemble( p_dec, p_block );

    if( ! p_spu_block )
    {
        return VLCDEC_SUCCESS;
    }

    /* FIXME: what the, we shouldn’t need to allocate 64k of buffer --sam. */
    p_sys->i_spu = block_ChainExtract( p_spu_block, p_sys->buffer, 65536 );
    p_sys->i_pts = p_spu_block->i_pts;
    block_ChainRelease( p_spu_block );

	/* Parse and decode */
    p_spu = ParsePacket( p_dec , &subtitle_text);

    /* reinit context */
    p_sys->i_spu_size = 0;
    p_sys->i_rle_size = 0;
    p_sys->i_spu      = 0;
    p_sys->p_block    = NULL;

	// save for filter, later
	mute_start = p_spu->i_start;
	mute_stop = p_spu->i_stop;
	// end filter saving stuff

    if( p_spu != NULL )
        decoder_QueueSub( p_dec, p_spu );

	// More filter stuff...
	//msg_Info(p_dec, "subtitle_text: %s\n", FromWide(subtitle_text.c_str()));
	if (p_sys->b_audiofilterEnable)
	{
		if (ParseForWords(subtitle_text) == TRUE)
		{
			//msg_Info(p_dec, "Detected forbidden word!\n");
			// after subtitle has been queued, then can go through mute routine...
			// this mute should be scheduled/queued somehow, then processed by audio filter, along with mutes from filter file
			// TODO:  mute timeframe should stay in sync with movie, so, pause in movie should keep mute on until unpause and hit end duration
			//        is there a way to figure out when spu un-renders the subtitle?  then could use that to trigger unmute?
			// can we use this to queue a mute?
			//block_t * 	decoder_NewAudioBuffer(decoder_t *, int i_nb_samples)
			//static void 	decoder_QueueAudio(decoder_t *dec, block_t *p_aout_buf)
			// for now, use this func to queue the mute, ideally would have atomic rmw for setting this var, but oh well
			// this should change anyway, if implemented in an audio filter
			if (var_GetInteger(p_input_thread, "my_mute_var") == 0)
			{
				duration = mute_stop - mute_start;
				var_SetInteger(p_input_thread, "my_mute_var", duration);
				vlc_timer_schedule(MyFilterTimer, true, decoder_GetDisplayDate(p_dec, mute_start), 0);  // delay to 1 to initiate as soon as possible
			}
		}
	}
	ofstream myfile;

	if (p_sys->b_DumpTextToFileEnable)
	{
		myfile.open("SubTextOutput.txt", ofstream::out | ofstream::app);
		libvlc_time_t timestamp;
		char starttime[SRT_BUF_SIZE];
		char endtime[SRT_BUF_SIZE];

		timestamp = from_mtime(p_spu->i_start);
		vlctime_to_srttime(starttime, timestamp);

		timestamp = from_mtime(p_spu->i_stop);
		vlctime_to_srttime(endtime, timestamp);

		framenumber++;
		myfile << framenumber << "\n" << starttime << " --> " << endtime << "\n" << FromWide(subtitle_text.c_str()) << "\n\n";

		myfile.close();
	}

    return VLCDEC_SUCCESS;
}

/*****************************************************************************
 * Packetize:
 *****************************************************************************/
static block_t *Packetize( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( pp_block == NULL ) /* No Drain */
        return NULL;
    block_t *p_block = *pp_block; *pp_block = NULL;
    if( p_block == NULL )
        return NULL;

    block_t *p_spu = Reassemble( p_dec, p_block );

    if( ! p_spu )
    {
        return NULL;
    }

    p_spu->i_dts = p_spu->i_pts;
    p_spu->i_length = 0;

    /* reinit context */
    p_sys->i_spu_size = 0;
    p_sys->i_rle_size = 0;
    p_sys->i_spu      = 0;
    p_sys->p_block    = NULL;

    return block_ChainGather( p_spu );
}

/*****************************************************************************
 * Reassemble:
 *****************************************************************************/
static block_t *Reassemble( decoder_t *p_dec, block_t *p_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( p_block->i_flags & BLOCK_FLAG_CORRUPTED )
    {
        block_Release( p_block );
        return NULL;
    }

    if( p_sys->i_spu_size <= 0 &&
        ( p_block->i_pts <= VLC_TS_INVALID || p_block->i_buffer < 4 ) )
    {
        msg_Dbg( p_dec, "invalid starting packet (size < 4 or pts <=0)" );
        //msg_Dbg( p_dec, "spu size: %d, i_pts: %"PRId64" i_buffer: %zu", p_sys->i_spu_size, p_block->i_pts, p_block->i_buffer );
		msg_Dbg(p_dec, "spu size: %d, i_pts: %lld i_buffer: %zu", p_sys->i_spu_size, p_block->i_pts, p_block->i_buffer);
		block_Release( p_block );
        return NULL;
    }

    block_ChainAppend( &p_sys->p_block, p_block );
    p_sys->i_spu += p_block->i_buffer;

    if( p_sys->i_spu_size <= 0 )
    {
        p_sys->i_spu_size = ( p_block->p_buffer[0] << 8 )|
            p_block->p_buffer[1];
        p_sys->i_rle_size = ( ( p_block->p_buffer[2] << 8 )|
            p_block->p_buffer[3] ) - 4;

        /* msg_Dbg( p_dec, "i_spu_size=%d i_rle=%d",
                    p_sys->i_spu_size, p_sys->i_rle_size ); */

        if( p_sys->i_spu_size <= 0 || p_sys->i_rle_size >= p_sys->i_spu_size )
        {
            p_sys->i_spu_size = 0;
            p_sys->i_rle_size = 0;
            p_sys->i_spu      = 0;
            p_sys->p_block    = NULL;

            block_Release( p_block );
            return NULL;
        }
    }

    if( p_sys->i_spu >= p_sys->i_spu_size )
    {
        /* We have a complete sub */
        if( p_sys->i_spu > p_sys->i_spu_size )
            msg_Dbg( p_dec, "SPU packets size=%d should be %d",
                     p_sys->i_spu, p_sys->i_spu_size );

        return p_sys->p_block;
    }
    return NULL;
}


