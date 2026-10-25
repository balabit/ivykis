/*
 * ivykis, an event handling library
 * Copyright (C) 2010 Lennert Buytenhek
 * Dedicated to Marija Kulikova.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License version 2.1 for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License version 2.1 along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __IV_SIGNAL_H
#define __IV_SIGNAL_H

#include <iv.h>
#include <iv_avl.h>
#include <iv_event_raw.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iv_signal {
	int			signum;
	unsigned int		flags;
	void			*cookie;
	void			(*handler)(void *);

	struct iv_avl_node	an;
	uint8_t			active;
	struct iv_event_raw	ev;
};

static inline void IV_SIGNAL_INIT(struct iv_signal *this)
{
}

#define IV_SIGNAL_FLAG_EXCLUSIVE	1
#define IV_SIGNAL_FLAG_THIS_THREAD	2

int iv_signal_register(struct iv_signal *this);
void iv_signal_unregister(struct iv_signal *this);

#ifdef _WIN32
/*
 * There are no asynchronous signals on Windows.  Console
 * control events are mapped to SIGINT/SIGHUP/SIGTERM automatically;
 * anything else (e.g. service control requests) can be delivered with
 * iv_signal_raise().  Returns the number of interests woken up.
 */
#define IV_SIGNAL_NSIG		64
#ifndef SIGHUP
#define SIGHUP			1
#endif
#ifndef SIGQUIT
#define SIGQUIT			3
#endif
#ifndef SIGUSR1
#define SIGUSR1			10
#endif
#ifndef SIGUSR2
#define SIGUSR2			12
#endif
#ifndef SIGPIPE
#define SIGPIPE			13
#endif
#ifndef SIGALRM
#define SIGALRM			14
#endif
#ifndef SIGCHLD
#define SIGCHLD			17
#endif

int iv_signal_raise(int signum);
#endif

#ifdef __cplusplus
}
#endif


#endif
