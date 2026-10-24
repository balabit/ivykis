/*
 * ivykis, an event handling library
 *
 * Test-only compatibility shims, force-included into the
 * test programs when building the POSIX core on Windows.
 *
 * The tests use alarm() as a watchdog against hangs.  Emulate it with
 * a timer queue timer that terminates the process, which is what the
 * default SIGALRM action would do.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 */

#ifndef __IV_TEST_WIN32_COMPAT_H
#define __IV_TEST_WIN32_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static HANDLE __iv_test_alarm_timer;

static VOID CALLBACK __iv_test_alarm_fired(PVOID arg, BOOLEAN fired)
{
	fprintf(stderr, "alarm: watchdog timer expired\n");
	fflush(stderr);
	_exit(128 + 14);
}

static inline unsigned int alarm(unsigned int seconds)
{
	if (__iv_test_alarm_timer != NULL) {
		DeleteTimerQueueTimer(NULL, __iv_test_alarm_timer, NULL);
		__iv_test_alarm_timer = NULL;
	}

	if (seconds) {
		CreateTimerQueueTimer(&__iv_test_alarm_timer, NULL,
				      __iv_test_alarm_fired, NULL,
				      seconds * 1000, 0,
				      WT_EXECUTEONLYONCE);
	}

	return 0;
}

#endif
