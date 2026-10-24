/*
 * ivykis, an event handling library
 *
 * iv_signal test, signals raised from another thread with
 * iv_signal_raise(), checking EXCLUSIVE semantics and unregistration.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <iv.h>
#include <iv_signal.h>
#include <iv_thread.h>

static struct iv_signal shared_a;
static struct iv_signal shared_b;
static struct iv_signal excl_a;
static struct iv_signal excl_b;
static struct iv_signal term;
static int shared_hits;
static int excl_hits;

static void got_shared(void *cookie)
{
	shared_hits++;
}

static void got_excl(void *cookie)
{
	excl_hits++;
}

static void got_term(void *cookie)
{
	iv_signal_unregister(&shared_a);
	iv_signal_unregister(&shared_b);
	iv_signal_unregister(&excl_a);
	iv_signal_unregister(&excl_b);
	iv_signal_unregister(&term);
}

static void raiser(void *cookie)
{
	Sleep(100);
	if (iv_signal_raise(SIGHUP) != 2)
		exit(2);
	Sleep(100);
	if (iv_signal_raise(SIGUSR1) != 1)
		exit(3);
	Sleep(100);
	iv_signal_raise(SIGTERM);
}

static void reg(struct iv_signal *is, int signum, unsigned flags,
		void (*handler)(void *))
{
	IV_SIGNAL_INIT(is);
	is->signum = signum;
	is->flags = flags;
	is->handler = handler;
	if (iv_signal_register(is) < 0)
		exit(4);
}

int main()
{
	alarm(10);

	iv_init();

	reg(&shared_a, SIGHUP, 0, got_shared);
	reg(&shared_b, SIGHUP, 0, got_shared);
	reg(&excl_a, SIGUSR1, IV_SIGNAL_FLAG_EXCLUSIVE, got_excl);
	reg(&excl_b, SIGUSR1, IV_SIGNAL_FLAG_EXCLUSIVE, got_excl);
	reg(&term, SIGTERM, 0, got_term);

	iv_thread_create("raiser", raiser, NULL);

	iv_main();

	iv_deinit();

	printf("shared_hits=%d excl_hits=%d\n", shared_hits, excl_hits);

	return !(shared_hits == 2 && excl_hits == 1);
}
