/*
 * ivykis, an event handling library
 *
 * iv_event_raw for the POSIX core on Windows.
 *
 * POSIX uses a pipe (or eventfd) here; Windows pipes can't be polled
 * with WSAPoll()/AFD, so we use a connected pair of loopback TCP
 * sockets instead, wrapped into CRT fds so they can be registered
 * with iv_fd.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iv.h>
#include <iv_event_raw.h>
#include <ws2tcpip.h>
#include "iv_private.h"

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_family == b->sin_family &&
	       a->sin_port == b->sin_port &&
	       a->sin_addr.s_addr == b->sin_addr.s_addr;
}

/*
 * Create a connected pair of loopback TCP sockets.  Returns 0 on
 * success.  sv[0] is the reading end, sv[1] the writing end.
 */
static int iv_win32_socketpair(SOCKET sv[2])
{
	SOCKET listener = INVALID_SOCKET;
	SOCKET rd = INVALID_SOCKET;
	SOCKET wr = INVALID_SOCKET;
	struct sockaddr_in addr;
	struct sockaddr_in wr_local;
	struct sockaddr_in rd_peer;
	int addrlen;
	int err;
	BOOL yes = TRUE;
	struct linger abortive = { 1, 0 };

	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET)
		goto err;

	/* Prevent other processes from hijacking our port. */
	setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
		   (const char *)&yes, sizeof(yes));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(listener, 1))
		goto err;

	addrlen = sizeof(addr);
	if (getsockname(listener, (struct sockaddr *)&addr, &addrlen))
		goto err;

	wr = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (wr == INVALID_SOCKET)
		goto err;

	if (connect(wr, (struct sockaddr *)&addr, sizeof(addr)))
		goto err;

	addrlen = sizeof(wr_local);
	if (getsockname(wr, (struct sockaddr *)&wr_local, &addrlen))
		goto err;

	/*
	 * Make sure that the connection we accept is the one we made,
	 * and not one from another local process racing us.
	 */
	for (;;) {
		addrlen = sizeof(rd_peer);
		rd = accept(listener, (struct sockaddr *)&rd_peer, &addrlen);
		if (rd == INVALID_SOCKET)
			goto err;
		if (same_addr(&rd_peer, &wr_local))
			break;
		closesocket(rd);
	}

	closesocket(listener);

	/* Single byte writes must not be delayed by Nagle. */
	setsockopt(wr, IPPROTO_TCP, TCP_NODELAY,
		   (const char *)&yes, sizeof(yes));

	/*
	 * Close abortively, so that closed pairs don't linger in
	 * TIME_WAIT and exhaust the ephemeral port range.
	 */
	setsockopt(rd, SOL_SOCKET, SO_LINGER,
		   (const char *)&abortive, sizeof(abortive));
	setsockopt(wr, SOL_SOCKET, SO_LINGER,
		   (const char *)&abortive, sizeof(abortive));

	sv[0] = rd;
	sv[1] = wr;

	return 0;

err:
	err = WSAGetLastError();
	if (listener != INVALID_SOCKET)
		closesocket(listener);
	if (rd != INVALID_SOCKET)
		closesocket(rd);
	if (wr != INVALID_SOCKET)
		closesocket(wr);
	WSASetLastError(err);

	return -1;
}

static void iv_event_raw_got_event(void *_this)
{
	struct iv_event_raw *this = (struct iv_event_raw *)_this;
	SOCKET s = iv_win32_fd_to_socket(this->event_rfd.fd);
	char buf[1024];
	int ret;

	ret = recv(s, buf, sizeof(buf), 0);
	if (ret <= 0) {
		int err = WSAGetLastError();

		if (ret == 0) {
			iv_fatal("iv_event_raw: reading from event socket "
				 "returned zero");
		} else if (err != WSAEWOULDBLOCK) {
			iv_fatal("iv_event_raw: reading from event socket "
				 "returned error %d", err);
		}
		return;
	}

	this->handler(this->cookie);
}

int iv_event_raw_register(struct iv_event_raw *this)
{
	SOCKET sv[2];
	int rfd;
	int wfd;

	if (iv_win32_socketpair(sv) < 0) {
		fprintf(stderr, "iv_event_raw_register: socketpair: %d\n",
			WSAGetLastError());
		return -1;
	}

	rfd = iv_win32_socket_to_fd(sv[0]);
	if (rfd < 0) {
		closesocket(sv[0]);
		closesocket(sv[1]);
		return -1;
	}

	wfd = iv_win32_socket_to_fd(sv[1]);
	if (wfd < 0) {
		iv_win32_close_socket_fd(rfd);
		closesocket(sv[1]);
		return -1;
	}

	IV_FD_INIT(&this->event_rfd);
	this->event_rfd.fd = rfd;
	this->event_rfd.cookie = this;
	this->event_rfd.handler_in = iv_event_raw_got_event;
	iv_fd_register(&this->event_rfd);

	this->event_wfd = wfd;
	iv_fd_set_cloexec(wfd);
	iv_fd_set_nonblock(wfd);

	return 0;
}

void iv_event_raw_unregister(struct iv_event_raw *this)
{
	iv_fd_unregister(&this->event_rfd);
	iv_win32_close_socket_fd(this->event_rfd.fd);
	iv_win32_close_socket_fd(this->event_wfd);
}

void iv_event_raw_post(const struct iv_event_raw *this)
{
	/*
	 * If the socket buffer is full (WSAEWOULDBLOCK), there is
	 * already a wakeup pending, so the error can be ignored.
	 */
	send(iv_win32_fd_to_socket(this->event_wfd), "", 1, 0);
}
