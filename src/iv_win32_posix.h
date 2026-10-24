/*
 * ivykis, an event handling library
 *
 * Private helpers for running the POSIX core (iv_fd & co.)
 * on Windows on top of Winsock.
 *
 * Convention: an iv_fd's ->fd is a C runtime file descriptor with a
 * Winsock SOCKET attached to it (see iv_win32.h).  The poll method
 * converts it back to a SOCKET with iv_win32_fd_to_socket().
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 */

#ifndef __IV_WIN32_POSIX_H
#define __IV_WIN32_POSIX_H

#include <winsock2.h>
#include <io.h>
#include "iv_win32.h"

/* There are no POSIX user ids on Windows. */
static inline int geteuid(void)
{
	return 0;
}

static inline int getuid(void)
{
	return 0;
}

/* iv_fd_win32.c */
void iv_fd_win32_init(void);

#endif
