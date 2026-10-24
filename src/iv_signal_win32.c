/*
 * ivykis, an event handling library
 *
 * iv_signal for the POSIX core on Windows.
 *
 * Windows has no asynchronous POSIX signals.  This implementation keeps
 * the iv_signal API and semantics (process wide and per-thread
 * interests, IV_SIGNAL_FLAG_EXCLUSIVE, delivery through iv_event_raw),
 * with these signal sources:
 *
 *  - console control events (installed with the first registration):
 *	CTRL_C_EVENT				-> SIGINT
 *	CTRL_BREAK_EVENT			-> SIGHUP
 *	CTRL_CLOSE/LOGOFF/SHUTDOWN_EVENT	-> SIGTERM
 *
 *  - iv_signal_raise(), to be called by the application, e.g. from a
 *    service control handler (SERVICE_CONTROL_STOP -> SIGTERM) or from
 *    a kill(getpid(), signum) emulation.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iv_list.h>
#include <iv_signal.h>
#include <iv_tls.h>
#include "iv_private.h"

static INIT_ONCE sig_init_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION sig_lock;
static int total_num_interests[IV_SIGNAL_NSIG];
static struct iv_avl_tree process_sigs;

static int
iv_signal_compare(const struct iv_avl_node *_a, const struct iv_avl_node *_b)
{
	const struct iv_signal *a =
		iv_container_of(_a, struct iv_signal, an);
	const struct iv_signal *b =
		iv_container_of(_b, struct iv_signal, an);

	if (a->signum < b->signum)
		return -1;
	if (a->signum > b->signum)
		return 1;

	if ((a->flags & IV_SIGNAL_FLAG_EXCLUSIVE) &&
	    !(b->flags & IV_SIGNAL_FLAG_EXCLUSIVE))
		return -1;
	if (!(a->flags & IV_SIGNAL_FLAG_EXCLUSIVE) &&
	    (b->flags & IV_SIGNAL_FLAG_EXCLUSIVE))
		return 1;

	if (a < b)
		return -1;
	if (a > b)
		return 1;

	return 0;
}

struct iv_signal_thr_info {
	struct iv_avl_tree	thr_sigs;
};

static void iv_signal_tls_init_thread(void *_tinfo)
{
	struct iv_signal_thr_info *tinfo = _tinfo;

	INIT_IV_AVL_TREE(&tinfo->thr_sigs, iv_signal_compare);
}

static struct iv_tls_user iv_signal_tls_user = {
	.sizeof_state	= sizeof(struct iv_signal_thr_info),
	.init_thread	= iv_signal_tls_init_thread,
};

static void iv_signal_init(void) __attribute__((constructor));
static void iv_signal_init(void)
{
	InitializeCriticalSection(&sig_lock);
	INIT_IV_AVL_TREE(&process_sigs, iv_signal_compare);
	iv_tls_user_register(&iv_signal_tls_user);
}

static struct iv_avl_node *
__iv_signal_find_first(struct iv_avl_tree *tree, int signum)
{
	struct iv_avl_node *iter;
	struct iv_avl_node *best;

	for (iter = tree->root, best = NULL; iter != NULL; ) {
		struct iv_signal *is;

		is = iv_container_of(iter, struct iv_signal, an);
		if (signum == is->signum)
			best = iter;

		if (signum <= is->signum)
			iter = iter->left;
		else
			iter = iter->right;
	}

	return best;
}

static int __iv_signal_do_wake(struct iv_avl_tree *tree, int signum)
{
	int woken;
	struct iv_avl_node *an;

	woken = 0;

	an = __iv_signal_find_first(tree, signum);
	while (an != NULL) {
		struct iv_signal *is;

		is = iv_container_of(an, struct iv_signal, an);
		if (is->signum != signum)
			break;

		is->active = 1;
		iv_event_raw_post(&is->ev);

		woken++;

		if (is->flags & IV_SIGNAL_FLAG_EXCLUSIVE)
			break;

		an = iv_avl_tree_next(an);
	}

	return woken;
}

int iv_signal_raise(int signum)
{
	struct iv_signal_thr_info *tinfo;
	int woken = 0;

	if (signum <= 0 || signum >= IV_SIGNAL_NSIG)
		return -1;

	EnterCriticalSection(&sig_lock);

	/*
	 * Like a synchronous POSIX signal: per-thread interests of the
	 * calling thread take precedence over process wide ones.
	 */
	tinfo = iv_inited() ? iv_tls_user_ptr(&iv_signal_tls_user) : NULL;
	if (tinfo != NULL)
		woken = __iv_signal_do_wake(&tinfo->thr_sigs, signum);
	if (!woken)
		woken = __iv_signal_do_wake(&process_sigs, signum);

	LeaveCriticalSection(&sig_lock);

	return woken;
}

static BOOL WINAPI iv_signal_console_handler(DWORD ctrl_type)
{
	int signum;

	switch (ctrl_type) {
	case CTRL_C_EVENT:
		signum = SIGINT;
		break;
	case CTRL_BREAK_EVENT:
		signum = SIGHUP;
		break;
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		signum = SIGTERM;
		break;
	default:
		return FALSE;
	}

	/* not handled -> next handler, i.e. default process termination */
	return iv_signal_raise(signum) > 0;
}

static BOOL CALLBACK
iv_signal_install_console_handler(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	SetConsoleCtrlHandler(iv_signal_console_handler, TRUE);
	return TRUE;
}

static void iv_signal_event(void *_this)
{
	struct iv_signal *this = _this;

	if (!(this->flags & IV_SIGNAL_FLAG_THIS_THREAD)) {
		EnterCriticalSection(&sig_lock);
		this->active = 0;
		LeaveCriticalSection(&sig_lock);
	} else {
		this->active = 0;
	}

	this->handler(this->cookie);
}

static struct iv_avl_tree *iv_signal_tree(struct iv_signal *this)
{
	if (this->flags & IV_SIGNAL_FLAG_THIS_THREAD) {
		struct iv_signal_thr_info *tinfo;

		tinfo = iv_tls_user_ptr(&iv_signal_tls_user);
		return &tinfo->thr_sigs;
	} else {
		return &process_sigs;
	}
}

int iv_signal_register(struct iv_signal *this)
{
	if (this->signum <= 0 || this->signum >= IV_SIGNAL_NSIG)
		return -1;

	InitOnceExecuteOnce(&sig_init_once, iv_signal_install_console_handler,
			    NULL, NULL);

	IV_EVENT_RAW_INIT(&this->ev);
	this->ev.cookie = this;
	this->ev.handler = iv_signal_event;
	if (iv_event_raw_register(&this->ev) < 0)
		return -1;

	this->active = 0;

	EnterCriticalSection(&sig_lock);
	total_num_interests[this->signum]++;
	iv_avl_tree_insert(iv_signal_tree(this), &this->an);
	LeaveCriticalSection(&sig_lock);

	return 0;
}

void iv_signal_unregister(struct iv_signal *this)
{
	if (this->signum <= 0 || this->signum >= IV_SIGNAL_NSIG)
		iv_fatal("iv_signal_unregister: signal number out of range");

	EnterCriticalSection(&sig_lock);

	iv_avl_tree_delete(iv_signal_tree(this), &this->an);

	if (--total_num_interests[this->signum] &&
	    (this->flags & IV_SIGNAL_FLAG_EXCLUSIVE) && this->active)
		__iv_signal_do_wake(iv_signal_tree(this), this->signum);

	LeaveCriticalSection(&sig_lock);

	iv_event_raw_unregister(&this->ev);
}
