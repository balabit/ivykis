/*
 * ivykis, an event handling library
 * Copyright (C) 2002, 2003, 2009 Lennert Buytenhek
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <iv_inotify.h>
#include "iv_private.h"

#define IV_FD_POLL_MAXFD	65536

static int iv_fd_poll_init(struct iv_state *st)
{
	st->u.poll.pfds = malloc(IV_FD_POLL_MAXFD * sizeof(struct pollfd));
	if (st->u.poll.pfds == NULL)
		return -1;

	st->u.poll.fds = malloc(IV_FD_POLL_MAXFD * sizeof(struct iv_fd_ *));
	if (st->u.poll.fds == NULL) {
		free(st->u.poll.pfds);
		return -1;
	}

	st->u.poll.num_regd_fds = 0;

#ifdef HAVE_INOTIFY_INIT


ok, pertially working, should
	- check why the poll is not always sihnals file modifications (at least not at realtime)
	- decide if use
		- 1 inotify objects with mani watches, or
		- 1 inotify object and 1 watch per file


	st->u.poll.inotify.fd = inotify_init1(IN_NONBLOCK); //inotify_init();
	if (st->u.poll.inotify.fd == -1) {
		free(st->u.poll.pfds);
		free(st->u.poll.fds);
		return -1;
	}
#endif

	return 0;
}

#ifdef HAVE_INOTIFY_INIT
static void read_regular_file_events(struct iv_state *st,
	struct iv_fd_ *fd, struct iv_list_head *active)
{
	/* Some systems cannot read integer variables if they are not
	 * properly aligned. On other systems, incorrect alignment may
	 * decrease performance. Hence, the buffer used for reading from
	 * the inotify file descriptor should have the same alignment as
	 * struct inotify_event.
	 */
	char buf[4096]
			__attribute__ ((aligned(__alignof__(struct inotify_event))));
	const struct inotify_event *event;
	ssize_t size;

	/* Loop while events can be read from inotify file descriptor. */
	for (;;) {
		size = read(st->u.poll.inotify.fd, buf, sizeof(buf));
		if (size == -1 && errno != EAGAIN) {
			iv_fatal("read_regular_file_events: read got error %d[%s]", errno,
				 strerror(errno));
		}

		/* If the nonblocking read() found no events to read, then
			it returns -1 with errno set to EAGAIN. In that case,
			we exit the loop. */
		if (size <= 0)
				break;

		/* Loop over all events in the buffer. */
		for (char *ptr = buf;
			ptr < buf + size;
			ptr += sizeof(struct inotify_event) + event->len) {

			event = (const struct inotify_event *) ptr;
			if (event->mask & (IN_ATTRIB | IN_CLOSE_NOWRITE | IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_OPEN))
				iv_fd_make_ready(active, fd, MASKIN);

			if (event->mask & (IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MODIFY | IN_MOVE_SELF | IN_OPEN))
				iv_fd_make_ready(active, fd, MASKOUT);

			if (event->mask & (IN_Q_OVERFLOW | IN_IGNORED | IN_UNMOUNT))
				iv_fd_make_ready(active, fd, MASKERR);
		}
	}
}
#endif

static void
iv_fd_poll_activate_fds(struct iv_state *st, struct iv_list_head *active)
{
	int i;

	for (i = 0; i < st->u.poll.num_regd_fds; i++) {
		struct iv_fd_ *fd;
		int revents;

		fd = st->u.poll.fds[i];
		revents = st->u.poll.pfds[i].revents;

#ifdef HAVE_INOTIFY_INIT
		if (fd->wd != -1)
			read_regular_file_events(st, fd, active);
#endif
		else {
			if (revents & (POLLIN | POLLERR | POLLHUP))
				iv_fd_make_ready(active, fd, MASKIN);

			if (revents & (POLLOUT | POLLERR | POLLHUP))
				iv_fd_make_ready(active, fd, MASKOUT);

			if (revents & (POLLERR | POLLHUP))
				iv_fd_make_ready(active, fd, MASKERR);
		}
	}
}

static int iv_fd_poll_poll(struct iv_state *st,
			   struct iv_list_head *active,
			   const struct timespec *abs)
{
#if _AIX
	/*
	 * AIX sometimes leaves errno uninitialized even if poll
	 * returns -1.
	 */
	errno = EINTR;
#endif

	int ret = poll(st->u.poll.pfds, st->u.poll.num_regd_fds, to_msec(st, abs));

	__iv_invalidate_now(st);

	if (ret < 0) {
		if (errno == EINTR)
			return 1;

		iv_fatal("iv_fd_poll_poll: got error %d[%s]", errno,
			 strerror(errno));
	}

	iv_fd_poll_activate_fds(st, active);

	return 1;
}

#ifdef HAVE_INOTIFY_INIT
static int is_regular_file(int fd)
{
	struct stat st;
	if (fstat(fd, &st) == -1) {
			return 0;
	}
	return S_ISREG(st.st_mode);
}
#endif

static void iv_fd_poll_register_fd(struct iv_state *st, struct iv_fd_ *fd)
{
	if (fd->fd >= IV_FD_POLL_MAXFD) {
		iv_fatal("iv_fd_poll_register_fd: called with fd %d "
			 "(maxfd = %d)", fd->fd, IV_FD_POLL_MAXFD);
	}

	fd->u.index = -1;

#ifdef HAVE_INOTIFY_INIT
	fd->wd = -1;

	if (is_regular_file(fd->fd)) {
		char proc_path[PATH_MAX];
		snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd->fd);

		char resolved_path[PATH_MAX];
		ssize_t path_len = readlink(proc_path, resolved_path, sizeof(resolved_path) - 1);
		if (path_len != -1)
				resolved_path[path_len] = '\0';
		else
			iv_fatal("iv_fd_inotify_register_fd: readlink got error "
				"%d[%s] for fd %d", errno,
				strerror(errno), fd->fd);

		fd->wd = inotify_add_watch(st->u.poll.inotify.fd, resolved_path, IN_ALL_EVENTS);
		if (fd->wd == -1)
			if (errno != ENOENT) // might be deleted meanwhile, it can happen
				iv_fatal("iv_fd_inotify_register_fd: inotify_add_watch got error "
					"%d[%s] for fd %d", errno,
					strerror(errno), fd->fd);
	}
#endif
}

static void iv_fd_poll_unregister_fd(struct iv_state *st, struct iv_fd_ *fd)
{
#ifdef HAVE_INOTIFY_INIT
	if (fd->wd != -1)
		inotify_rm_watch(st->u.poll.inotify.fd, fd->wd);
#endif
}

static int bits_to_poll_mask(int bits)
{
	int mask;

	mask = 0;
	if (bits & MASKIN)
		mask |= POLLIN | POLLHUP;
	if (bits & MASKOUT)
		mask |= POLLOUT | POLLHUP;
	if (bits & MASKERR)
		mask |= POLLHUP;

	return mask;
}

static void iv_fd_poll_notify_fd(struct iv_state *st, struct iv_fd_ *fd)
{
	if (fd->u.index == -1 && fd->wanted_bands) {
		fd->u.index = st->u.poll.num_regd_fds++;
#ifdef HAVE_INOTIFY_INIT
		st->u.poll.pfds[fd->u.index].fd = (fd->wd != -1 ? st->u.poll.inotify.fd : fd->fd);
#endif
		st->u.poll.pfds[fd->u.index].events =
			bits_to_poll_mask(fd->wanted_bands);
		st->u.poll.fds[fd->u.index] = fd;
	}
	else if (fd->u.index != -1 && !fd->wanted_bands) {
		st->u.poll.num_regd_fds--;
		if (fd->u.index != st->u.poll.num_regd_fds) {
			struct iv_fd_ *last;

			st->u.poll.pfds[fd->u.index] =
				st->u.poll.pfds[st->u.poll.num_regd_fds];

			last = st->u.poll.fds[st->u.poll.num_regd_fds];
			last->u.index = fd->u.index;
			st->u.poll.fds[fd->u.index] = last;
		}

		fd->u.index = -1;
	}
	else {
		st->u.poll.pfds[fd->u.index].events =
			bits_to_poll_mask(fd->wanted_bands);
	}
}

static int iv_fd_poll_notify_fd_sync(struct iv_state *st, struct iv_fd_ *fd)
{
	struct pollfd pfd;
	int ret;

	pfd.fd = fd->fd;
	pfd.events = POLLIN | POLLOUT | POLLHUP;

	do {
		ret = poll(&pfd, 1, 0);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0 || (pfd.revents & POLLNVAL))
		return -1;

	iv_fd_poll_notify_fd(st, fd);

	return 0;
}

static void iv_fd_poll_deinit(struct iv_state *st)
{
	free(st->u.poll.fds);
	free(st->u.poll.pfds);
#ifdef HAVE_INOTIFY_INIT
	close(st->u.poll.inotify.fd);
#endif
}

const struct iv_fd_poll_method iv_fd_poll_method_poll = {
	.name		= "poll",
	.init		= iv_fd_poll_init,
	.poll		= iv_fd_poll_poll,
	.register_fd	= iv_fd_poll_register_fd,
	.unregister_fd	= iv_fd_poll_unregister_fd,
	.notify_fd	= iv_fd_poll_notify_fd,
	.notify_fd_sync	= iv_fd_poll_notify_fd_sync,
	.deinit		= iv_fd_poll_deinit,
};


#ifdef HAVE_PPOLL
static int iv_fd_poll_ppoll(struct iv_state *st,
			    struct iv_list_head *active,
			    const struct timespec *abs)
{
	struct pollfd *pfds = st->u.poll.pfds;
	int npfds = st->u.poll.num_regd_fds;
	struct timespec rel;
	int ret;

	ret = ppoll(pfds, npfds, to_relative(st, &rel, abs), NULL);

	__iv_invalidate_now(st);

	if (ret < 0) {
		if (errno == EINTR)
			return 1;

		if (errno == ENOSYS) {
			method = &iv_fd_poll_method_poll;
			return iv_fd_poll_poll(st, active, abs);
		}

		iv_fatal("iv_fd_poll_ppoll: got error %d[%s]", errno,
			 strerror(errno));
	}

	iv_fd_poll_activate_fds(st, active);

	return 1;
}

const struct iv_fd_poll_method iv_fd_poll_method_ppoll = {
	.name		= "ppoll",
	.init		= iv_fd_poll_init,
	.poll		= iv_fd_poll_ppoll,
	.register_fd	= iv_fd_poll_register_fd,
	.unregister_fd	= iv_fd_poll_unregister_fd,
	.notify_fd	= iv_fd_poll_notify_fd,
	.notify_fd_sync	= iv_fd_poll_notify_fd_sync,
	.deinit		= iv_fd_poll_deinit,
};
#endif
