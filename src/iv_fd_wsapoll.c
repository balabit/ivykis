/*
 * ivykis, an event handling library
 *
 * WSAPoll() based poll method.
 *
 * This is a simple, O(n) per poll, readiness based backend intended as
 * the first working backend for the POSIX core on Windows.  It is
 * modelled on iv_fd_poll.c, with these Winsock specific differences:
 *
 *  - pollfd.fd is a SOCKET, converted from the CRT fd stored in iv_fd;
 *  - WSAPoll() rejects POLLHUP/POLLERR/POLLPRI in ->events;
 *  - WSAPoll() fails with zero sockets, so we sleep instead;
 *  - errors are reported through WSAGetLastError(), not errno;
 *  - the arrays grow dynamically, as there is no fd number bound.
 *
 * A scalable (AFD/IOCP, wepoll style) backend is expected to replace
 * this one later.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iv_private.h"

#define IV_FD_WSAPOLL_INITIAL_SIZE	64

static int iv_fd_wsapoll_grow(struct iv_state *st)
{
	int size = st->u.wsapoll.size ? 2 * st->u.wsapoll.size :
					IV_FD_WSAPOLL_INITIAL_SIZE;
	struct pollfd *pfds;
	struct iv_fd_ **fds;

	pfds = realloc(st->u.wsapoll.pfds, size * sizeof(*pfds));
	if (pfds == NULL)
		return -1;
	st->u.wsapoll.pfds = pfds;

	fds = realloc(st->u.wsapoll.fds, size * sizeof(*fds));
	if (fds == NULL)
		return -1;
	st->u.wsapoll.fds = fds;

	st->u.wsapoll.size = size;

	return 0;
}

static int iv_fd_wsapoll_init(struct iv_state *st)
{
	st->u.wsapoll.pfds = NULL;
	st->u.wsapoll.fds = NULL;
	st->u.wsapoll.num_regd_fds = 0;
	st->u.wsapoll.size = 0;

	return iv_fd_wsapoll_grow(st);
}

static void
iv_fd_wsapoll_activate_fds(struct iv_state *st, struct iv_list_head *active)
{
	int i;

	for (i = 0; i < st->u.wsapoll.num_regd_fds; i++) {
		struct iv_fd_ *fd;
		int revents;

		fd = st->u.wsapoll.fds[i];
		revents = st->u.wsapoll.pfds[i].revents;

		if (revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))
			iv_fd_make_ready(active, fd, MASKIN);

		if (revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))
			iv_fd_make_ready(active, fd, MASKOUT);

		if (revents & (POLLERR | POLLHUP | POLLNVAL))
			iv_fd_make_ready(active, fd, MASKERR);
	}
}

static int iv_fd_wsapoll_poll(struct iv_state *st,
			      struct iv_list_head *active,
			      const struct timespec *abs)
{
	int msec = to_msec(st, abs);
	int ret;

	if (st->u.wsapoll.num_regd_fds == 0) {
		Sleep(msec < 0 ? INFINITE : (DWORD)msec);
		__iv_invalidate_now(st);
		return 1;
	}

	ret = WSAPoll(st->u.wsapoll.pfds, st->u.wsapoll.num_regd_fds, msec);

	__iv_invalidate_now(st);

	if (ret == SOCKET_ERROR) {
		int err = WSAGetLastError();

		if (err == WSAEINTR)
			return 1;

		iv_fatal("iv_fd_wsapoll_poll: got error %d", err);
	}

	iv_fd_wsapoll_activate_fds(st, active);

	return 1;
}

static void iv_fd_wsapoll_register_fd(struct iv_state *st, struct iv_fd_ *fd)
{
	/*
	 * A single invalid entry makes WSAPoll() fail as a whole, so
	 * reject non-sockets up front.
	 */
	if (!iv_win32_fd_is_socket(fd->fd)) {
		iv_fatal("iv_fd_wsapoll_register_fd: fd %d is not a socket "
			 "(see iv_win32_socket_to_fd())", fd->fd);
	}

	fd->u.index = -1;
}

static short bits_to_poll_mask(int bits)
{
	short mask;

	/*
	 * POLLERR and POLLHUP are always reported by WSAPoll(), and
	 * must not be requested explicitly.
	 */
	mask = 0;
	if (bits & MASKIN)
		mask |= POLLRDNORM;
	if (bits & MASKOUT)
		mask |= POLLWRNORM;

	return mask;
}

static void iv_fd_wsapoll_notify_fd(struct iv_state *st, struct iv_fd_ *fd)
{
	if (fd->u.index == -1 && fd->wanted_bands) {
		if (st->u.wsapoll.num_regd_fds == st->u.wsapoll.size &&
		    iv_fd_wsapoll_grow(st) < 0)
			iv_fatal("iv_fd_wsapoll_notify_fd: out of memory");

		fd->u.index = st->u.wsapoll.num_regd_fds++;
		st->u.wsapoll.pfds[fd->u.index].fd =
			iv_win32_fd_to_socket(fd->fd);
		st->u.wsapoll.pfds[fd->u.index].events =
			bits_to_poll_mask(fd->wanted_bands);
		st->u.wsapoll.pfds[fd->u.index].revents = 0;
		st->u.wsapoll.fds[fd->u.index] = fd;
	} else if (fd->u.index != -1 && !fd->wanted_bands) {
		st->u.wsapoll.num_regd_fds--;
		if (fd->u.index != st->u.wsapoll.num_regd_fds) {
			struct iv_fd_ *last;

			st->u.wsapoll.pfds[fd->u.index] =
				st->u.wsapoll.pfds[st->u.wsapoll.num_regd_fds];

			last = st->u.wsapoll.fds[st->u.wsapoll.num_regd_fds];
			last->u.index = fd->u.index;
			st->u.wsapoll.fds[fd->u.index] = last;
		}

		fd->u.index = -1;
	} else if (fd->u.index != -1) {
		st->u.wsapoll.pfds[fd->u.index].events =
			bits_to_poll_mask(fd->wanted_bands);
	}
}

static int iv_fd_wsapoll_notify_fd_sync(struct iv_state *st, struct iv_fd_ *fd)
{
	struct pollfd pfd;
	int ret;

	pfd.fd = iv_win32_fd_to_socket(fd->fd);
	if (pfd.fd == INVALID_SOCKET)
		return -1;
	pfd.events = POLLRDNORM | POLLWRNORM;
	pfd.revents = 0;

	ret = WSAPoll(&pfd, 1, 0);
	if (ret == SOCKET_ERROR || (pfd.revents & POLLNVAL))
		return -1;

	iv_fd_wsapoll_notify_fd(st, fd);

	return 0;
}

static void iv_fd_wsapoll_deinit(struct iv_state *st)
{
	free(st->u.wsapoll.fds);
	free(st->u.wsapoll.pfds);
}

const struct iv_fd_poll_method iv_fd_poll_method_wsapoll = {
	.name		= "wsapoll",
	.init		= iv_fd_wsapoll_init,
	.poll		= iv_fd_wsapoll_poll,
	.register_fd	= iv_fd_wsapoll_register_fd,
	.notify_fd	= iv_fd_wsapoll_notify_fd,
	.notify_fd_sync	= iv_fd_wsapoll_notify_fd_sync,
	.deinit		= iv_fd_wsapoll_deinit,
};
