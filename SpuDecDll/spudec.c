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
#include <vlc_codec.h>
#include <vlc_common.h>
#include <vlc_modules.h>

// ***
// mv to common header file?
typedef struct
{
	wstring FilterType;
	mtime_t starttime;
	mtime_t endtime;
} FilterFileEntry;
// declare as VLC var somehow?
std::vector<FilterFileEntry> FilterFileArray;
// ****

static bool ParseForWords(std::wstring sentence);

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

static void srttime_to_mtime(mtime_t &itime, std::wstring srttime)
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
	// stuff below doing reverse of calculations in mtime_to_srttime
	itime = ((((((hours * 60) + minutes) * 60) + seconds) * 1000) + milliseconds) * 1000ULL;

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

	badwords.clear();

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
	mtime_t starttime_mt;
	mtime_t endtime_mt;

	// get name of dvdrom
	WCHAR myDrives[105];
	WCHAR volumeName[MAX_PATH];
	WCHAR fileSystemName[MAX_PATH];
	DWORD serialNumber, maxComponentLen, fileSystemFlags;
	UINT driveType;

	FilterFileArray.clear();

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
	//	msg_Info(p_dec, "type: %S, starttime: %lld\n", n.FilterType.c_str(), n.starttime);
	//}

}






/*****************************************************************************
 * Module descriptor.
 *****************************************************************************/
// spu decoder
static int  DecoderOpen   ( vlc_object_t * );
static void Close         ( vlc_object_t * );

// audio decoder
int InitAudioDec(vlc_object_t *);
void EndAudioDec(vlc_object_t *);

// input demux
int  DemuxOpen(vlc_object_t *);
void DemuxClose(vlc_object_t *);

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
    set_subcategory(SUBCAT_INPUT_GENERAL)
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

	add_submodule()
	add_shortcut("MovAudDecFlt")
	set_capability("audio decoder", 80)
	set_callbacks(InitAudioDec, EndAudioDec)

	add_submodule()
	add_shortcut("dvd")  // dvd name is required here, else it will only load dvdnav module directly
	set_capability("access_demux", 80)
	set_callbacks(DemuxOpen, DemuxClose)

vlc_module_end ()

struct decoder_sys_t
{
	decoder_t * p_subdec;

	//new
	bool b_videofilterEnable;
	bool b_audiofilterEnable;
	bool b_RenderEnable;
	bool b_DumpTextToFileEnable;
	bool b_CaptureTextPicsEnable;
};


/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int            Decode    ( decoder_t *, block_t * );

static decoder_t * my_local_p_dec = NULL;
mtime_t mute_start_time = 0;
mtime_t mute_end_time = 0;

// helper function for string comparison
void toLower(basic_string<wchar_t>& s) {
	for (basic_string<wchar_t>::iterator p = s.begin();
		p != s.end(); ++p) {
		*p = towlower(*p);
	}
}
// note: this is called from subdecoder, so the p_dec pointer is the subdec pointer, not local one
// need to use local pointer for calling original queue audio
static int MyDecoderQueueSub(decoder_t *p_dec, subpicture_t * p_spu)
{
	std::wstring subtitle_text=L"";
	decoder_sys_t * p_sys = my_local_p_dec->p_sys;

	// this comes from decoder_QueueSub in vlc_codec.h
	assert(p_spu->p_next == NULL);
	assert(my_local_p_dec->pf_queue_sub != NULL);

	// can access pic here
	picture_t * sub_pic;
	subpicture_region_t * sub_region;
	sub_region = p_spu->p_region;
	sub_pic = p_spu->p_region->p_picture;

	// if enabled, let's parts the subtitle pic and convert to string
	subtitle_text.assign(OcrDecodeText(sub_region, p_sys->b_CaptureTextPicsEnable));
	toLower(subtitle_text);

	//msg_Info(p_dec, "subtitle_text: %s\n", FromWide(subtitle_text.c_str()));
	if (p_sys->b_audiofilterEnable)
	{
		if (ParseForWords(subtitle_text) == TRUE)
		{
			// TODO:  change to use vlc vars?
			// queue the mute
			mute_start_time = p_spu->i_start;
			mute_end_time = p_spu->i_stop;
		}
	}

	if (p_sys->b_DumpTextToFileEnable)
	{
		ofstream myfile;
		myfile.open("SubTextOutput.txt", ofstream::out | ofstream::app);
		char starttime[SRT_BUF_SIZE];
		char endtime[SRT_BUF_SIZE];

		mtime_to_srttime(starttime, p_spu->i_start);

		mtime_to_srttime(endtime, p_spu->i_stop);

		myfile << 1 << "\n" << starttime << " --> " << endtime << "\n" << FromWide(subtitle_text.c_str()) << "\n\n";

		myfile.close();
	}
	//TODO: skip calling actual queue routine if don't want to render subtitle
	//msg_Info(p_dec, "in queue sub... \n");
	if (p_sys->b_RenderEnable == true)
	{
		return my_local_p_dec->pf_queue_sub(p_dec, p_spu);
	}
	return 0;
}

/*****************************************************************************
 * DecoderOpen
 *****************************************************************************
 * Tries to launch a decoder and return score so that the interface is able
 * to chose.
 *****************************************************************************/

static int DecoderOpen( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t*)p_this;
	decoder_sys_t *p_sys = (decoder_sys_t *)vlc_obj_malloc(p_this, sizeof(*p_sys));


	if (unlikely(p_sys == NULL))
	{
		msg_Info(p_dec, "No MEM! \n");
		return VLC_ENOMEM;
	}
	p_sys->p_subdec = (decoder_t *)vlc_object_create(p_dec, sizeof(*p_dec));
	if (p_sys->p_subdec == NULL)
		return VLC_EGENERIC;

	// copy contents of p_dec to subdec, excluding common stuff (in obj), which causes problems if that's overwritten
	memcpy(((char *)p_sys->p_subdec + sizeof(p_dec->obj)), ((char *)p_dec + sizeof(p_dec->obj)), (sizeof(decoder_t) - sizeof(p_dec->obj)));

	// now assign local p_sys;
	p_dec->p_sys = p_sys;
	// load some config vars
	p_sys->b_videofilterEnable = var_InheritBool(p_dec, "dvdsub-video-filter");
	p_sys->b_audiofilterEnable = var_InheritBool(p_dec, "dvdsub-audio-filter");
	p_sys->b_RenderEnable = var_InheritBool(p_dec, "dvdsub-render-enable");
	p_sys->b_DumpTextToFileEnable = var_InheritBool(p_dec, "dvdsub-text-to-file-enable");
	p_sys->b_CaptureTextPicsEnable = var_InheritBool(p_dec, "dvdsub-save-text-pic-enable");

	// intercept the decoder queue routine
	if (p_sys->b_audiofilterEnable)
	{
		p_sys->p_subdec->pf_queue_sub = MyDecoderQueueSub;
	}
	
	// load module
	p_sys->p_subdec->p_module = module_need(p_sys->p_subdec, "spu decoder", "spudec", true);
	if (p_sys->p_subdec->p_module == NULL)
	{
		msg_Info(p_dec, "Subtitle dec: No MODULE! \n");
		return VLC_EGENERIC;
	}

	msg_Info(p_dec, "Subtitle dec: Made it HERE.... \n");

	p_dec->pf_decode = Decode;
	p_dec->pf_packetize = NULL;
	// this gets initialized in submodule, so let's reuse value here
	p_dec->fmt_out = p_sys->p_subdec->fmt_out;
	p_dec->fmt_in = p_sys->p_subdec->fmt_in;


	// save pointer for use later
	my_local_p_dec = p_dec;

	// MOVE somewhere else
	msg_Info(p_dec, "\n\nLoading words file.\n\n");
	LoadWords();

	msg_Info(p_dec, "\n\nLoading filter file.\n\n");
	LoadFilterFile(p_dec);
		
	
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t*)p_this;
    decoder_sys_t *sys = p_dec->p_sys;

	msg_Info(p_dec, "Subtitle dec: unloading module.... \n");

	module_unneed(sys->p_subdec, sys->p_subdec->p_module);
	sys->p_subdec->p_module = NULL;
	vlc_object_release(sys->p_subdec);
	vlc_obj_free((vlc_object_t *)p_dec, sys);
//	var_Destroy(p_dec, "start_mute");
//	var_Destroy(p_dec, "end_mute");

	// filter cleanup stuff
	badwords.clear();
	FilterFileArray.clear();
	my_local_p_dec = NULL;
}

/*****************************************************************************
 * Decode:
 *****************************************************************************/

static int Decode( decoder_t *p_dec, block_t *p_block )
{
	return p_dec->p_sys->p_subdec->pf_decode(p_dec->p_sys->p_subdec, p_block);
}


