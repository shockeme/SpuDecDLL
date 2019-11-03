#pragma once

#include <BaseTsd.h>
int strcasecmp(const char *, const char *);
typedef SSIZE_T ssize_t;
//#define restrict    __restrict      
#define strdup _strdup
#define MODULE_STRING "MvFlt"
#define N_(str) (str)

//#define __cplusplus
#define __PLUGIN__

// from vlc_fixups.h
#ifndef HAVE_STRUCT_POLLFD
enum
{
	POLLERR = 0x1,
	POLLHUP = 0x2,
	POLLNVAL = 0x4,
	POLLWRNORM = 0x10,
	POLLWRBAND = 0x20,
	POLLRDNORM = 0x100,
	POLLRDBAND = 0x200,
	POLLPRI = 0x400,
};
#define POLLIN  (POLLRDNORM|POLLRDBAND)
#define POLLOUT (POLLWRNORM|POLLWRBAND)

struct pollfd
{
	int fd;
	unsigned events;
	unsigned revents;
};
#endif
#ifndef HAVE_POLL
struct pollfd;
int poll(struct pollfd *, unsigned, int);
#endif
