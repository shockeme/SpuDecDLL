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
 // redefine restrict to MS friendly just for include of vlc_charset (otherwise will cause problems)
#define restrict __restrict
#include <vlc_charset.h>
#undef restrict

// windows header files
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <wctype.h>
#include <vector>
using namespace std;

/*****************************************************************************
 * Prototypes
 *****************************************************************************/
#define SPU_ID_BASE 0xbd20

wchar_t * OcrDecodeText(subpicture_region_t * SpuProp, bool SavePicToFile);

static inline void srttime_to_mtime(mtime_t &itime, std::wstring srttime)
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
