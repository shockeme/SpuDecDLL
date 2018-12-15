/*****************************************************************************
* parse.c: SPU parser
*****************************************************************************
* Copyright (C) 2000-2001, 2005, 2006 VLC authors and VideoLAN
* $Id: 275864f187aa62119914038186d4d7ea7b23bf03 $
*
* Authors: Sam Hocevar <sam@zoy.org>
*          Laurent Aimar <fenrir@via.ecp.fr>
*          Gildas Bazin <gbazin@videolan.org>
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
#include "stdio.h"
#include "time.h"

/*****************************************************************************
* Preamble
*****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "spudec.h"
#include "..\libvlc.h"  // this is in 1 level above the plugins include area; could change include paths for this
#include <vlc_common.h>
#include <vlc_aout.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <wctype.h>
#include <map>
using namespace std;

// This stuff is included to give us visibility into private structures that we'll use to help with getting time & muting
#include <vlc_atomic.h>
#include <vlc_input.h> /* FIXME Needed for input_clock_t */



typedef struct input_clock_t input_clock_t;

// Taken from src\input\decoder.c
struct decoder_owner_sys_t
{
	input_thread_t  *p_input;
	input_resource_t*p_resource;
	input_clock_t   *p_clock;
	int             i_last_rate;

	vout_thread_t   *p_spu_vout;
	int              i_spu_channel;
	int64_t          i_spu_order;

	sout_instance_t         *p_sout;
	sout_packetizer_input_t *p_sout_input;

	vlc_thread_t     thread;

	void(*pf_update_stat)(decoder_owner_sys_t *, unsigned decoded, unsigned lost);

	/* Some decoders require already packetized data (ie. not truncated) */
	decoder_t *p_packetizer;
	bool b_packetizer;

	/* Current format in use by the output */
	es_format_t    fmt;

	/* */
	bool           b_fmt_description;
	vlc_meta_t     *p_description;
	atomic_int     reload;

	/* fifo */
	block_fifo_t *p_fifo;

	/* Lock for communication with decoder thread */
	vlc_mutex_t lock;
	vlc_cond_t  wait_request;
	vlc_cond_t  wait_acknowledge;
	vlc_cond_t  wait_fifo; /* TODO: merge with wait_acknowledge */
	vlc_cond_t  wait_timed;

	/* -- These variables need locking on write(only) -- */
	audio_output_t *p_aout;

	vout_thread_t   *p_vout;

	/* -- Theses variables need locking on read *and* write -- */
	/* Preroll */
	int64_t i_preroll_end;
	/* Pause */
	mtime_t pause_date;
	unsigned frames_countdown;
	bool paused;

	bool error;

	/* Waiting */
	bool b_waiting;
	bool b_first;
	bool b_has_data;

	/* Flushing */
	bool flushing;
	bool b_draining;
	atomic_bool drained;
	bool b_idle;

	/* CC */
#define MAX_CC_DECODERS 64 /* The es_out only creates one type of es */
	struct
	{
		bool b_supported;
		decoder_cc_desc_t desc;
		decoder_t *pp_decoder[MAX_CC_DECODERS];
	} cc;

	/* Delay */
	mtime_t i_ts_delay;
};

/// END stuff needed for visibility into private structures

// note... these copied from libvlc_internal.h
static inline libvlc_time_t from_mtime(mtime_t time)
{
	return (time + 500ULL) / 1000ULL;
}

static inline mtime_t to_mtime(libvlc_time_t time)
{
	return time * 1000ULL;
}


/*****************************************************************************
* Local prototypes.
*****************************************************************************/



static int  ParseControlSeq(decoder_t *, subpicture_t *, subpicture_data_t *,
	spu_properties_t *, mtime_t i_pts);
static int  ParseRLE(decoder_t *, subpicture_data_t *,
	const spu_properties_t *);
static void Render(decoder_t *, subpicture_t *, subpicture_data_t *,
	const spu_properties_t *);
static bool ParseForWords(std::wstring sentence);

/*****************************************************************************
* AddNibble: read a nibble from a source packet and add it to our integer.
*****************************************************************************/
static inline unsigned int AddNibble(unsigned int i_code,
	const uint8_t *p_src, unsigned int *pi_index)
{
	if (*pi_index & 0x1)
	{
		return(i_code << 4 | (p_src[(*pi_index)++ >> 1] & 0xf));
	}
	else
	{
		return(i_code << 4 | p_src[(*pi_index)++ >> 1] >> 4);
	}
}

/*****************************************************************************
* ParseForWords: parse sentence and return TRUE if a cuss word is found
* TODO: need to get a text file parsed at the start of the movie and then store 
* the words into an array for this function
*****************************************************************************/
std::vector<std::wstring> badwords;
static std::map<std::wstring, int> ConfigOptionMap;
// default values now defined in map; may eventually remove these vars if map is used instead
static int RenderEnable;
static int DumpTextToFileEnabled;
static int FilterOnTheFlyEnabled;

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

// QUESTION:  Should we merge with bad word file?  Just have 1 file that has the options & words to filter?
// text file = config.txt put in the same directory as the vlc.exe file. 
// the format is very simple where each line represents the setting
// config.txt:
// RenderEnable 1
// DumpTextToFileEnabled 0
// FilterOnTheFlyEnabled 1
// VideoFilterEnabled 0
//
// this means that RenderEnable is true (=1), DumpTextToFileEnabled = false (=0), and FilterOnTheFlyEnabled = true (=1)
static void LoadConfig()
{
	std::wifstream infile("config.txt");
	std::wstring line;
	// init some default values, since these will be used
	ConfigOptionMap[L"RenderEnable"] = 1;
	ConfigOptionMap[L"DumpTextToFileEnabled"] = 1;
	ConfigOptionMap[L"FilterOnTheFlyEnabled"] = 1;
	ConfigOptionMap[L"VideoFilterEnabled"] = 0;

	while (std::getline(infile, line))
	{
		std::wstringstream iss(line);
		int ConfigValue;
		std::wstring ConfigOption;
		if (!(iss >> ConfigOption >> ConfigValue)) { break; } // error
		ConfigOptionMap[ConfigOption] = ConfigValue;
	}
	// can change to just use the map directly in other functions, rather than assigning to these vars
	RenderEnable = ConfigOptionMap[L"RenderEnable"];
	DumpTextToFileEnabled = ConfigOptionMap[L"DumpTextToFileEnabled"];
	FilterOnTheFlyEnabled = ConfigOptionMap[L"FilterOnTheFlyEnabled"];
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

#define SRT_BUF_SIZE 50
// note, srttimebuf must be passed in with size SRT_BUF_SIZE; todo: perhaps better way to pass in buffer?
void vlctime_to_srttime(char srttimebuf[SRT_BUF_SIZE], libvlc_time_t itime)
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
	libvlc_time_t starttime;
	libvlc_time_t endtime;
} FilterFileEntry;
std::vector<FilterFileEntry> FilterFileArray;

// This routine currently hardcoded to load file named:  FilterFile.txt
static int FilterFileLoaded = 0;
static void LoadFilterFile()
{
	std::vector<std::wstring> filterfile;
	std::wifstream infile("FilterFile.txt"); // How do we allow user to provide input to determine filter file name??
	std::wstring cmdline;
	std::wstring srttime;
	libvlc_time_t starttime;
	libvlc_time_t endtime;

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

			// get & throw away intermediate content on line, then get srt end time
			std::getline(infile, srttime, L' ');
			std::getline(infile, srttime);  // remainder of line should be the srt end time

			// process endtime
			srttime_to_vlctime(endtime, srttime);

			FilterFileArray.push_back({ cmdline, starttime, endtime });
		}
	}
	FilterFileLoaded = 1;
	
	// debug dump of array... 
	//ofstream myfile;
	//myfile.open("RawFilterOutput.txt", ofstream::out);
	//myfile << "dumping array...\n";
	//for (int i = 0; i < FilterFileArray.size(); i++)
	//{
	//	myfile << FromWide(FilterFileArray[i].FilterType.c_str()) << ", " << FilterFileArray[i].starttime << ", " << FilterFileArray[i].endtime << "\n";
	//}
	//myfile.close();
}

// helper function for string comparison
void toLower(basic_string<wchar_t>& s) {
	for (basic_string<wchar_t>::iterator p = s.begin();
		p != s.end(); ++p) {
		*p = towlower(*p);
	}
}



/*****************************************************************************
* ParsePacket: parse an SPU packet and send it to the video output
*****************************************************************************
* This function parses the SPU packet and, if valid, sends it to the
* video output.
*****************************************************************************/
int framenumber=0;
static int WordListLoaded = 0;
static int ConfigFileLoaded = 0;

// Will execute the filters loaded in FilterFile.txt at the appropriate time
void * ParseFilters(void * input_data)
{
	decoder_owner_sys_t *p_owner = (decoder_owner_sys_t *)input_data;
	input_thread_t *p_input_thread = p_owner->p_input;
	libvlc_time_t timestamp;
	mtime_t mdateval;
	mtime_t inputtimeval;
	libvlc_time_t duration;
	ofstream myfile;
	audio_output_t *p_aout;

	// do this loop forever... 
	// todo:  well, should be executed while main movie event is playing, can optimize later
	while (1)
	{
		// only do below if ptr is valid; assuming this ptr stays valid for duration of movie playing...
		if (p_input_thread)
		{
			// NOTE:  This 'time' var doesn't seem to line up with the start/stop values of mute, not sure how these time values relate to filterfile values right now
			vlc_object_hold(p_input_thread);
			timestamp = from_mtime(var_GetInteger(p_input_thread, "time"));
			vlc_object_release(p_input_thread);

			// Todo: optimize this scheme, better to periodically check?  or to put in wait until specific time based
			//  on filter times?
			// current method here is to just periodically check, will always wait ~100ms between each check.
			// look for entry that timestamp is greater than start time and less than end time
			for (int i = 0; i < FilterFileArray.size(); i++)
			{
				if ((timestamp > FilterFileArray[i].starttime) && (timestamp < FilterFileArray[i].endtime))
				{
					// debug
					//myfile.open("SubTextOutput.txt", ofstream::out | ofstream::app);
					//myfile << "ParseFilters: Detected match. timestamp: " << timestamp << " start: " << FilterFileArray[i].starttime << " end: " << FilterFileArray[i].endtime << "\n";
					//myfile.close();
					if (FilterFileArray[i].FilterType == L"skip")
					{
						// debug message
						//myfile.open("SubTextOutput.txt", ofstream::out | ofstream::app);
						//myfile << "ParseFilters: Skipping video...\n";
						//myfile.close();
						// set new time 
						vlc_object_hold(p_input_thread);
						var_SetInteger(p_input_thread, "time", to_mtime(FilterFileArray[i].endtime + 300)); // todo:  fixme... need to add ~300ms to make sure new time doesn't fall into this same window, it seems not precise
						vlc_object_release(p_input_thread);
						break; // out of the for loop
					} 
					// TODO:  fix... muting is broken, seems domute function breaks, maybe bad ptrs
					else if (FilterFileArray[i].FilterType == L"mute")
					{
						duration = FilterFileArray[i].endtime - FilterFileArray[i].starttime;
						// debug message
						//myfile.open("SubTextOutput.txt", ofstream::out | ofstream::app);
						//myfile << "ParseFilters: Muting audio... duration: " << duration << "\n";
						//myfile.close();
						// mute
						// get aout, return 0 means successful
						if (!input_Control(p_input_thread, INPUT_GET_AOUT, &p_aout))
						{
							DoMute(1, duration, p_aout, true);
						}
						//msleep(to_mtime(duration * 1000)); // todo:  find better way to handle this, but for now, sleep while muting
						//myfile.open("SubTextOutput.txt", ofstream::out | ofstream::app);
						//myfile << "ParseFilters: done sleeping...\n ";
						//myfile.close();
						break; // out of the for loop
					}
					
				}
			}
		}
		// wait for ~100ms (msleep uses usec)
		msleep(100000);
	}

	return nullptr;
}

subpicture_t * ParsePacket(decoder_t *p_dec)
{
	decoder_sys_t *p_sys = p_dec->p_sys;
	subpicture_t *p_spu;
	subpicture_data_t spu_data;
	spu_properties_t spu_properties;

	/* Allocate the subpicture internal data. */
	p_spu = decoder_NewSubpicture(p_dec, NULL);
	if (!p_spu) return NULL;

	p_spu->i_original_picture_width =
		p_dec->fmt_in.subs.spu.i_original_frame_width;
	p_spu->i_original_picture_height =
		p_dec->fmt_in.subs.spu.i_original_frame_height;

	/* Getting the control part */
	if (ParseControlSeq(p_dec, p_spu, &spu_data, &spu_properties, p_sys->i_pts))
	{
		/* There was a parse error, delete the subpicture */
		subpicture_Delete(p_spu);
		return NULL;
	}

	/* we are going to expand the RLE stuff so that we won't need to read
	* nibbles later on. This will speed things up a lot. Plus, we'll only
	* need to do this stupid interlacing stuff once.
	*
	* Rationale for the "p_spudec->i_rle_size * 4*sizeof(*spu_data.p_data)":
	*  one byte gaves two nibbles and may be used twice (once per field)
	* generating 4 codes.
	*/
	spu_data.p_data = (uint16_t *)vlc_alloc(p_sys->i_rle_size, sizeof(*spu_data.p_data) * 2 * 2);

	/* We try to display it */
	if (ParseRLE(p_dec, &spu_data, &spu_properties))
	{
		/* There was a parse error, delete the subpicture */
		subpicture_Delete(p_spu);
		free(spu_data.p_data);
		return NULL;
	}

#ifdef DEBUG_SPUDEC
	msg_Dbg(p_dec, "total size: 0x%x, RLE offsets: 0x%x 0x%x",
		p_sys->i_spu_size,
		spu_data.pi_offset[0], spu_data.pi_offset[1]);
#endif

	// TODO... Makes these parameters configurable by user
	// Todo:  Enable this only once the movie has started, else it impacts dvd menus
	// Todo:  should maybe move this outside of parsepackets
	if (WordListLoaded == 0)
	{
		LoadWords();
		WordListLoaded = 1;
	}

	if (ConfigFileLoaded == 0)
	{
		LoadConfig();
		ConfigFileLoaded = 1;
	}

	decoder_owner_sys_t *p_owner = p_dec->p_owner;
	if (FilterOnTheFlyEnabled || DumpTextToFileEnabled)
	{
		ofstream myfile;
		std::wstring decodedtxt(OcrDecodeText(&spu_data, &spu_properties));
		toLower(decodedtxt);
		if (FilterOnTheFlyEnabled)
		{
			libvlc_time_t duration = 0;

			// TODO:  might want to add a safe-mode here, where it will still mute when ocr fail to detect any text
			//   This situation can occur either because OCR failed to detect any text or when it detects text incorrectly
			//   In the meantime, can put "No Text Detected" in the filter word file to handle case where it fails to detect any text
			//   seems decoder has particular issues with words 4 letters or less, i've seen problems on multiple dvds
			if (ParseForWords(decodedtxt) == TRUE)
			{
				mtime_t getdateresult = decoder_GetDisplayDate(p_dec, p_spu->i_start);
				mtime_t getcurrentdate = mdate();
				mtime_t datediff = getdateresult - getcurrentdate;
				duration = from_mtime(p_spu->i_stop - p_spu->i_start);
				// TODO:  mute timeframe should stay in sync with movie, so, pause in movie should keep mute on until unpause and hit end duration
				//        is there a way to figure out when spu un-renders the subtitle?  then could use that to trigger unmute?
				// for debug... 
				//input_thread_t *p_input_thread = p_owner->p_input;
				//myfile.open("SubTextOutput.txt", ofstream::out | ofstream::app);
				//vlc_object_hold(p_input_thread);
				//myfile << "Calling mute... start: " << p_spu->i_start << " stop: " << p_spu->i_stop << " ipts: " << p_sys->i_pts << " mdate: " << getcurrentdate << " getdateresult: " << getdateresult << " datediff: " << datediff << " time var: " << var_GetInteger(p_input_thread, "time") << "\n";
				//vlc_object_release(p_input_thread);
				//myfile.close();

				DoMute(from_mtime(datediff), duration, input_resource_HoldAout(p_owner->p_resource), false);
			}
		}
		if (DumpTextToFileEnabled)
		{
			myfile.open("SubTextOutput.txt", ofstream::out | ofstream::app);
			libvlc_time_t timestamp, n;
			char starttime[SRT_BUF_SIZE];
			char endtime[SRT_BUF_SIZE];

			timestamp = from_mtime(p_spu->i_start);
			vlctime_to_srttime(starttime, timestamp);

			timestamp = from_mtime(p_spu->i_stop);
			vlctime_to_srttime(endtime, timestamp);

			framenumber++;
			myfile << framenumber << "\n" << starttime << " --> " << endtime << "\n" << FromWide(decodedtxt.c_str()) << "\n\n";

			myfile.close();
		}
	}

	if (RenderEnable)
	{
		Render(p_dec, p_spu, &spu_data, &spu_properties);
	}

	free(spu_data.p_data);

	// NOTE.. this should not be in spudec... need to move at some point, but, for initial debug/enabling
	if (ConfigOptionMap[L"VideoFilterEnabled"])
	{
		ofstream myfile;
		libvlc_time_t timestamp, n;
		vlc_thread_t newthread;
		char * starttime;
		// load filter file
		if (!FilterFileLoaded)
		{
			LoadFilterFile();
			// need to spawn another thread to loop on executing the filters
			if (vlc_clone(&newthread, &ParseFilters, (void *)p_owner, 0) != 0)
			{
				//error, exit(?)
				msg_Err(p_dec, "Failed to spawn filter thread.");
			}
		}
	}

	return p_spu;
}

/*****************************************************************************
* ParseControlSeq: parse all SPU control sequences
*****************************************************************************
* This is the most important part in SPU decoding. We get dates, palette
* information, coordinates, and so on. For more information on the
* subtitles format, see http://sam.zoy.org/doc/dvd/subtitles/index.html
*****************************************************************************/
static int ParseControlSeq(decoder_t *p_dec, subpicture_t *p_spu,
	subpicture_data_t *p_spu_data, spu_properties_t *p_spu_properties, mtime_t i_pts)
{
	decoder_sys_t *p_sys = p_dec->p_sys;

	/* Our current index in the SPU packet */
	unsigned int i_index;

	/* The next start-of-control-sequence index and the previous one */
	unsigned int i_next_seq = 0, i_cur_seq = 0;

	/* Command and date */
	uint8_t i_command = SPU_CMD_END;
	mtime_t date = 0;
	bool b_cmd_offset = false;
	bool b_cmd_alpha = false;
	subpicture_data_t spu_data_cmd;

	if (!p_spu || !p_spu_data)
		return VLC_EGENERIC;

	/* Create working space for spu data */
	memset(&spu_data_cmd, 0, sizeof(spu_data_cmd));
	spu_data_cmd.pi_offset[0] = -1;
	spu_data_cmd.pi_offset[1] = -1;
	spu_data_cmd.p_data = NULL;
	spu_data_cmd.b_palette = false;
	spu_data_cmd.b_auto_crop = false;
	spu_data_cmd.i_y_top_offset = 0;
	spu_data_cmd.i_y_bottom_offset = 0;
	spu_data_cmd.pi_alpha[0] = 0x00;
	spu_data_cmd.pi_alpha[1] = 0x0f;
	spu_data_cmd.pi_alpha[2] = 0x0f;
	spu_data_cmd.pi_alpha[3] = 0x0f;

	/* Initialize the structure */
	p_spu->i_start = p_spu->i_stop = 0;
	p_spu->b_ephemer = false;

	memset(p_spu_properties, 0, sizeof(*p_spu_properties));

	/* */
	*p_spu_data = spu_data_cmd;

	for (i_index = 4 + p_sys->i_rle_size; i_index < p_sys->i_spu_size; )
	{
		/* If we just read a command sequence, read the next one;
		* otherwise, go on with the commands of the current sequence. */
		if (i_command == SPU_CMD_END)
		{
			if (i_index + 4 > p_sys->i_spu_size)
			{
				msg_Err(p_dec, "overflow in SPU command sequence");
				return VLC_EGENERIC;
			}

			/* */
			b_cmd_offset = false;
			b_cmd_alpha = false;
			/* Get the control sequence date */
			date = (mtime_t)GetWBE(&p_sys->buffer[i_index]) * 11000;

			/* Next offset */
			i_cur_seq = i_index;
			i_next_seq = GetWBE(&p_sys->buffer[i_index + 2]);

			if (i_next_seq > p_sys->i_spu_size)
			{
				msg_Err(p_dec, "overflow in SPU next command sequence");
				return VLC_EGENERIC;
			}

			/* Skip what we just read */
			i_index += 4;
		}

		i_command = p_sys->buffer[i_index];

		switch (i_command)
		{
		case SPU_CMD_FORCE_DISPLAY: /* 00 (force displaying) */
			p_spu->i_start = i_pts + date;
			p_spu->b_ephemer = true;
			/* ignores picture date as display start time
			* works around non displayable (offset by few ms)
			* spu menu over still frame in SPU_Select */
			p_spu->b_subtitle = false;
			i_index += 1;
			break;

			/* Convert the dates in seconds to PTS values */
		case SPU_CMD_START_DISPLAY: /* 01 (start displaying) */
			p_spu->i_start = i_pts + date;
			i_index += 1;
			break;

		case SPU_CMD_STOP_DISPLAY: /* 02 (stop displaying) */
			p_spu->i_stop = i_pts + date;
			i_index += 1;
			break;

		case SPU_CMD_SET_PALETTE:
			/* 03xxxx (palette) */
			if (i_index + 3 > p_sys->i_spu_size)
			{
				msg_Err(p_dec, "overflow in SPU command");
				return VLC_EGENERIC;
			}

			if (p_dec->fmt_in.subs.spu.palette[0] == SPU_PALETTE_DEFINED)
			{
				unsigned int idx[4];
				int i;

				spu_data_cmd.b_palette = true;

				idx[0] = (p_sys->buffer[i_index + 1] >> 4) & 0x0f;
				idx[1] = (p_sys->buffer[i_index + 1]) & 0x0f;
				idx[2] = (p_sys->buffer[i_index + 2] >> 4) & 0x0f;
				idx[3] = (p_sys->buffer[i_index + 2]) & 0x0f;

				for (i = 0; i < 4; i++)
				{
					uint32_t i_color = p_dec->fmt_in.subs.spu.palette[1 + idx[i]];

					/* FIXME: this job should be done sooner */
					spu_data_cmd.pi_yuv[3 - i][0] = (i_color >> 16) & 0xff;
					spu_data_cmd.pi_yuv[3 - i][1] = (i_color >> 0) & 0xff;
					spu_data_cmd.pi_yuv[3 - i][2] = (i_color >> 8) & 0xff;
				}
			}

			i_index += 3;
			break;

		case SPU_CMD_SET_ALPHACHANNEL: /* 04xxxx (alpha channel) */
			if (i_index + 3 > p_sys->i_spu_size)
			{
				msg_Err(p_dec, "overflow in SPU command");
				return VLC_EGENERIC;
			}

			if (!p_sys->b_disabletrans)
			{ /* If we want to use original transparency values */
				b_cmd_alpha = true;
				spu_data_cmd.pi_alpha[3] = (p_sys->buffer[i_index + 1] >> 4) & 0x0f;
				spu_data_cmd.pi_alpha[2] = (p_sys->buffer[i_index + 1]) & 0x0f;
				spu_data_cmd.pi_alpha[1] = (p_sys->buffer[i_index + 2] >> 4) & 0x0f;
				spu_data_cmd.pi_alpha[0] = (p_sys->buffer[i_index + 2]) & 0x0f;
			}

			i_index += 3;
			break;

		case SPU_CMD_SET_COORDINATES: /* 05xxxyyyxxxyyy (coordinates) */
			if (i_index + 7 > p_sys->i_spu_size)
			{
				msg_Err(p_dec, "overflow in SPU command");
				return VLC_EGENERIC;
			}

			p_spu_properties->i_x = (p_sys->buffer[i_index + 1] << 4) |
				((p_sys->buffer[i_index + 2] >> 4) & 0x0f);
			p_spu_properties->i_width = (((p_sys->buffer[i_index + 2] & 0x0f) << 8) |
				p_sys->buffer[i_index + 3]) - p_spu_properties->i_x + 1;

			p_spu_properties->i_y = (p_sys->buffer[i_index + 4] << 4) |
				((p_sys->buffer[i_index + 5] >> 4) & 0x0f);
			p_spu_properties->i_height = (((p_sys->buffer[i_index + 5] & 0x0f) << 8) |
				p_sys->buffer[i_index + 6]) - p_spu_properties->i_y + 1;

			/* Auto crop fullscreen subtitles */
			if (p_spu_properties->i_height > 250)
				p_spu_data->b_auto_crop = true;

			i_index += 7;
			break;

		case SPU_CMD_SET_OFFSETS: /* 06xxxxyyyy (byte offsets) */
			if (i_index + 5 > p_sys->i_spu_size)
			{
				msg_Err(p_dec, "overflow in SPU command");
				return VLC_EGENERIC;
			}

			b_cmd_offset = true;
			p_spu_data->pi_offset[0] = GetWBE(&p_sys->buffer[i_index + 1]) - 4;
			p_spu_data->pi_offset[1] = GetWBE(&p_sys->buffer[i_index + 3]) - 4;
			i_index += 5;
			break;

		case SPU_CMD_END: /* ff (end) */
			if (b_cmd_offset)
			{
				/* It seems that palette and alpha from the block having
				* the cmd offset have to be used
				* XXX is it all ? */
				p_spu_data->b_palette = spu_data_cmd.b_palette;
				if (spu_data_cmd.b_palette)
					memcpy(p_spu_data->pi_yuv, spu_data_cmd.pi_yuv, sizeof(spu_data_cmd.pi_yuv));
				if (b_cmd_alpha)
					memcpy(p_spu_data->pi_alpha, spu_data_cmd.pi_alpha, sizeof(spu_data_cmd.pi_alpha));
			}

			i_index += 1;
			break;

		default: /* xx (unknown command) */
			msg_Warn(p_dec, "unknown SPU command 0x%.2x", i_command);
			if (i_index + 1 < i_next_seq)
			{
				/* There is at least one other command sequence */
				if (p_sys->buffer[i_next_seq - 1] == SPU_CMD_END)
				{
					/* This is consistent. Skip to that command sequence. */
					i_index = i_next_seq;
				}
				else
				{
					/* There were other commands. */
					msg_Warn(p_dec, "cannot recover, dropping subtitle");
					return VLC_EGENERIC;
				}
			}
			else
			{
				/* We were in the last command sequence. Stop parsing by
				* pretending we met an SPU_CMD_END command. */
				i_command = SPU_CMD_END;
				i_index++;
			}
		}

		/* */
		if (i_command == SPU_CMD_END && i_index != i_next_seq)
			break;
	}

	/* Check that the next sequence index matches the current one */
	if (i_next_seq != i_cur_seq)
	{
		msg_Err(p_dec, "index mismatch (0x%.4x != 0x%.4x)",
			i_next_seq, i_cur_seq);
		return VLC_EGENERIC;
	}

	if (i_index > p_sys->i_spu_size)
	{
		msg_Err(p_dec, "uh-oh, we went too far (0x%.4x > 0x%.4x)",
			i_index, p_sys->i_spu_size);
		return VLC_EGENERIC;
	}

	const int i_spu_size = p_sys->i_spu - 4;
	if (p_spu_data->pi_offset[0] < 0 || p_spu_data->pi_offset[0] >= i_spu_size ||
		p_spu_data->pi_offset[1] < 0 || p_spu_data->pi_offset[1] >= i_spu_size)
	{
		msg_Err(p_dec, "invalid offset values");
		return VLC_EGENERIC;
	}

	if (!p_spu->i_start)
	{
		msg_Err(p_dec, "no `start display' command");
		return VLC_EGENERIC;
	}

	if (p_spu->i_stop <= p_spu->i_start && !p_spu->b_ephemer)
	{
		/* This subtitle will live for 5 seconds or until the next subtitle */
		p_spu->i_stop = p_spu->i_start + (mtime_t)500 * 11000;
		p_spu->b_ephemer = true;
	}

	/* Get rid of padding bytes */
	if (p_sys->i_spu_size > i_index + 1)
	{
		/* Zero or one padding byte are quite usual
		* More than one padding byte - this is very strange, but
		* we can ignore them. */
		msg_Warn(p_dec, "%i padding bytes, we usually get 0 or 1 of them",
			p_sys->i_spu_size - i_index);
	}

	/* Successfully parsed ! */
	return VLC_SUCCESS;
}

/*****************************************************************************
* ParseRLE: parse the RLE part of the subtitle
*****************************************************************************
* This part parses the subtitle graphical data and stores it in a more
* convenient structure for later decoding. For more information on the
* subtitles format, see http://sam.zoy.org/doc/dvd/subtitles/index.html
*****************************************************************************/
static int ParseRLE(decoder_t *p_dec,
	subpicture_data_t *p_spu_data,
	const spu_properties_t *p_spu_properties)
{
	decoder_sys_t *p_sys = p_dec->p_sys;

	const unsigned int i_width = p_spu_properties->i_width;
	const unsigned int i_height = p_spu_properties->i_height;
	unsigned int i_x, i_y;

	uint16_t *p_dest = p_spu_data->p_data;

	/* The subtitles are interlaced, we need two offsets */
	unsigned int  i_id = 0;                   /* Start on the even SPU layer */
	unsigned int  pi_table[2];
	unsigned int *pi_offset;

	/* Cropping */
	bool b_empty_top = true;
	unsigned int i_skipped_top = 0, i_skipped_bottom = 0;
	unsigned int i_transparent_code = 0;

	/* Colormap statistics */
	int i_border = -1;
	int stats[4]; stats[0] = stats[1] = stats[2] = stats[3] = 0;

	pi_table[0] = p_spu_data->pi_offset[0] << 1;
	pi_table[1] = p_spu_data->pi_offset[1] << 1;

	for (i_y = 0; i_y < i_height; i_y++)
	{
		unsigned int i_code;
		pi_offset = pi_table + i_id;

		for (i_x = 0; i_x < i_width; i_x += i_code >> 2)
		{
			i_code = 0;
			for (unsigned int i_min = 1; i_min <= 0x40 && i_code < i_min; i_min <<= 2)
			{
				if ((*pi_offset >> 1) >= p_sys->i_spu_size)
				{
					msg_Err(p_dec, "out of bounds while reading rle");
					return VLC_EGENERIC;
				}
				i_code = AddNibble(i_code, &p_sys->buffer[4], pi_offset);
			}
			if (i_code < 0x0004)
			{
				/* If the 14 first bits are set to 0, then it's a
				* new line. We emulate it. */
				i_code |= (i_width - i_x) << 2;
			}

			if (((i_code >> 2) + i_x + i_y * i_width) > i_height * i_width)
			{
				msg_Err(p_dec, "out of bounds, %i at (%i,%i) is out of %ix%i",
					i_code >> 2, i_x, i_y, i_width, i_height);
				return VLC_EGENERIC;
			}

			/* Try to find the border color */
			if (p_spu_data->pi_alpha[i_code & 0x3] != 0x00)
			{
				i_border = i_code & 0x3;
				stats[i_border] += i_code >> 2;
			}

			/* Auto crop subtitles (a lot more optimized) */
			if (p_spu_data->b_auto_crop)
			{
				if (!i_y)
				{
					/* We assume that if the first line is transparent, then
					* it is using the palette index for the
					* (background) transparent color */
					if ((i_code >> 2) == i_width &&
						p_spu_data->pi_alpha[i_code & 0x3] == 0x00)
					{
						i_transparent_code = i_code;
					}
					else
					{
						p_spu_data->b_auto_crop = false;
					}
				}

				if (i_code == i_transparent_code)
				{
					if (b_empty_top)
					{
						/* This is a blank top line, we skip it */
						i_skipped_top++;
					}
					else
					{
						/* We can't be sure the current lines will be skipped,
						* so we store the code just in case. */
						*p_dest++ = i_code;
						i_skipped_bottom++;
					}
				}
				else
				{
					/* We got a valid code, store it */
					*p_dest++ = i_code;

					/* Valid code means no blank line */
					b_empty_top = false;
					i_skipped_bottom = 0;
				}
			}
			else
			{
				*p_dest++ = i_code;
			}
		}

		/* Check that we didn't go too far */
		if (i_x > i_width)
		{
			msg_Err(p_dec, "i_x overflowed, %i > %i", i_x, i_width);
			return VLC_EGENERIC;
		}

		/* Byte-align the stream */
		if (*pi_offset & 0x1)
		{
			(*pi_offset)++;
		}

		/* Swap fields */
		i_id = ~i_id & 0x1;
	}

	/* We shouldn't get any padding bytes */
	if (i_y < i_height)
	{
		msg_Err(p_dec, "padding bytes found in RLE sequence");
		msg_Err(p_dec, "send mail to <sam@zoy.org> if you "
			"want to help debugging this");

		/* Skip them just in case */
		while (i_y < i_height)
		{
			*p_dest++ = i_width << 2;
			i_y++;
		}

		return VLC_EGENERIC;
	}

#ifdef DEBUG_SPUDEC
	msg_Dbg(p_dec, "valid subtitle, size: %ix%i, position: %i,%i",
		p_spu_properties->i_width, p_spu_properties->i_height, p_spu_properties->i_x, p_spu_properties->i_y);
#endif

	/* Crop if necessary */
	if (i_skipped_top || i_skipped_bottom)
	{
#ifdef DEBUG_SPUDEC
		int i_y = p_spu_properties->i_y + i_skipped_top;
		int i_height = p_spu_properties->i_height - (i_skipped_top + i_skipped_bottom);
#endif
		p_spu_data->i_y_top_offset = i_skipped_top;
		p_spu_data->i_y_bottom_offset = i_skipped_bottom;
#ifdef DEBUG_SPUDEC
		msg_Dbg(p_dec, "cropped to: %ix%i, position: %i,%i",
			p_spu_properties->i_width, i_height, p_spu_properties->i_x, i_y);
#endif
	}

	/* Handle color if no palette was found */
	if (!p_spu_data->b_palette)
	{
		int i, i_inner = -1, i_shade = -1;

		/* Set the border color */
		if (i_border != -1)
		{
			p_spu_data->pi_yuv[i_border][0] = 0x00;
			p_spu_data->pi_yuv[i_border][1] = 0x80;
			p_spu_data->pi_yuv[i_border][2] = 0x80;
			stats[i_border] = 0;
		}

		/* Find the inner colors */
		for (i = 0; i < 4 && i_inner == -1; i++)
		{
			if (stats[i])
			{
				i_inner = i;
			}
		}

		for (; i < 4 && i_shade == -1; i++)
		{
			if (stats[i])
			{
				if (stats[i] > stats[i_inner])
				{
					i_shade = i_inner;
					i_inner = i;
				}
				else
				{
					i_shade = i;
				}
			}
		}

		/* Set the inner color */
		if (i_inner != -1)
		{
			p_spu_data->pi_yuv[i_inner][0] = 0xff;
			p_spu_data->pi_yuv[i_inner][1] = 0x80;
			p_spu_data->pi_yuv[i_inner][2] = 0x80;
		}

		/* Set the anti-aliasing color */
		if (i_shade != -1)
		{
			p_spu_data->pi_yuv[i_shade][0] = 0x80;
			p_spu_data->pi_yuv[i_shade][1] = 0x80;
			p_spu_data->pi_yuv[i_shade][2] = 0x80;
		}

#ifdef DEBUG_SPUDEC
		msg_Dbg(p_dec, "using custom palette (border %i, inner %i, shade %i)",
			i_border, i_inner, i_shade);
#endif
	}

	return VLC_SUCCESS;
}

static void Render(decoder_t *p_dec, subpicture_t *p_spu,
	subpicture_data_t *p_spu_data,
	const spu_properties_t *p_spu_properties)
{
	uint8_t *p_p;
	int i_x, i_y, i_len, i_color, i_pitch;
	const uint16_t *p_source = p_spu_data->p_data;
	video_format_t fmt;
	video_palette_t palette;

	/* Create a new subpicture region */
	video_format_Init(&fmt, VLC_CODEC_YUVP);
	fmt.i_sar_num = 0; /* 0 means use aspect ratio of background video */
	fmt.i_sar_den = 1;
	fmt.i_width = fmt.i_visible_width = p_spu_properties->i_width;
	fmt.i_height = fmt.i_visible_height = p_spu_properties->i_height -
		p_spu_data->i_y_top_offset - p_spu_data->i_y_bottom_offset;
	fmt.i_x_offset = fmt.i_y_offset = 0;
	fmt.p_palette = &palette;
	fmt.p_palette->i_entries = 4;
	for (i_x = 0; i_x < fmt.p_palette->i_entries; i_x++)
	{
		fmt.p_palette->palette[i_x][0] = p_spu_data->pi_yuv[i_x][0];
		fmt.p_palette->palette[i_x][1] = p_spu_data->pi_yuv[i_x][1];
		fmt.p_palette->palette[i_x][2] = p_spu_data->pi_yuv[i_x][2];
		fmt.p_palette->palette[i_x][3] = p_spu_data->pi_alpha[i_x] * 0x11;
	}

	p_spu->p_region = subpicture_region_New(&fmt);
	if (!p_spu->p_region)
	{
		fmt.p_palette = NULL;
		video_format_Clean(&fmt);
		msg_Err(p_dec, "cannot allocate SPU region");
		return;
	}

	p_spu->p_region->i_x = p_spu_properties->i_x;
	p_spu->p_region->i_y = p_spu_properties->i_y + p_spu_data->i_y_top_offset;
	p_p = p_spu->p_region->p_picture->p->p_pixels;
	i_pitch = p_spu->p_region->p_picture->p->i_pitch;

	/* Draw until we reach the bottom of the subtitle */
	for (i_y = 0; i_y < (int)fmt.i_height * i_pitch; i_y += i_pitch)
	{
		/* Draw until we reach the end of the line */
		for (i_x = 0; i_x < (int)fmt.i_width; i_x += i_len)
		{
			/* Get the RLE part, then draw the line */
			i_color = *p_source & 0x3;
			i_len = *p_source++ >> 2;
			memset(p_p + i_x + i_y, i_color, i_len);
		}
	}

	fmt.p_palette = NULL;
	video_format_Clean(&fmt);
}

