/*
 * ivykis, an event handling library
 *
 * Winsock specific helpers for the POSIX core on Windows.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "iv_private.h"

void iv_fd_win32_init(void)
{
	WSADATA wsa;
	int ret;

	/*
	 * WSAStartup() is reference counted, so it is harmless if the
	 * application has already called it.  We never call
	 * WSACleanup(), as sockets may outlive the ivykis state.
	 */
	ret = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (ret != 0)
		iv_fatal("iv_fd_win32_init: WSAStartup failed: %d", ret);
}


/*
 * CRT fd <-> SOCKET table.
 *
 * A SOCKET can't simply be wrapped into a CRT fd with
 * _open_osfhandle(), because then both _close() and closesocket()
 * would close the same handle value, and whichever runs second could
 * close an unrelated handle that reused the value in the meantime
 * (protecting the handle from close isn't an option either, as that
 * raises an exception under a debugger).
 *
 * Instead the CRT fd wraps a placeholder handle (to the NUL device),
 * which is owned and closed by the CRT, while the SOCKET is kept in
 * this table, indexed by fd, and is owned and closed by Winsock.
 */
static SRWLOCK socket_table_lock = SRWLOCK_INIT;
static SOCKET *socket_table;
static int socket_table_size;

static int socket_table_set(int fd, SOCKET s)
{
	AcquireSRWLockExclusive(&socket_table_lock);

	if (fd >= socket_table_size) {
		int size = socket_table_size ? socket_table_size : 64;
		SOCKET *table;
		int i;

		while (size <= fd)
			size *= 2;

		table = realloc(socket_table, size * sizeof(*table));
		if (table == NULL) {
			ReleaseSRWLockExclusive(&socket_table_lock);
			return -1;
		}

		for (i = socket_table_size; i < size; i++)
			table[i] = INVALID_SOCKET;

		socket_table = table;
		socket_table_size = size;
	}

	socket_table[fd] = s;

	ReleaseSRWLockExclusive(&socket_table_lock);

	return 0;
}

SOCKET iv_win32_fd_to_socket(int fd)
{
	SOCKET s = INVALID_SOCKET;

	AcquireSRWLockShared(&socket_table_lock);
	if (fd >= 0 && fd < socket_table_size)
		s = socket_table[fd];
	ReleaseSRWLockShared(&socket_table_lock);

	return s;
}

int iv_win32_fd_is_socket(int fd)
{
	return iv_win32_fd_to_socket(fd) != INVALID_SOCKET;
}

int iv_win32_socket_to_fd(SOCKET s)
{
	HANDLE placeholder;
	int fd;

	if (s == INVALID_SOCKET) {
		errno = EBADF;
		return -1;
	}

	placeholder = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
				  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
				  OPEN_EXISTING, 0, NULL);
	if (placeholder == INVALID_HANDLE_VALUE) {
		errno = EMFILE;
		return -1;
	}

	fd = _open_osfhandle((intptr_t)placeholder, 0);
	if (fd < 0) {
		CloseHandle(placeholder);
		return -1;
	}

	if (socket_table_set(fd, s) < 0) {
		_close(fd);
		errno = ENOMEM;
		return -1;
	}

	return fd;
}

int iv_win32_close_socket_fd(int fd)
{
	SOCKET s = iv_win32_fd_to_socket(fd);
	int ret;

	if (s == INVALID_SOCKET) {
		errno = EBADF;
		return -1;
	}

	/*
	 * Detach the socket before releasing the fd number, so that a
	 * concurrent iv_win32_socket_to_fd() reusing the same number
	 * can't have its entry clobbered.
	 */
	socket_table_set(fd, INVALID_SOCKET);
	_close(fd);

	ret = closesocket(s);
	if (ret != 0) {
		errno = EIO;
		return -1;
	}

	return 0;
}


void iv_fd_set_cloexec(int fd)
{
	SOCKET s = iv_win32_fd_to_socket(fd);
	HANDLE h = (HANDLE)_get_osfhandle(fd);

	if (h != INVALID_HANDLE_VALUE)
		SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
	if (s != INVALID_SOCKET)
		SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
}

void iv_fd_set_nonblock(int fd)
{
	SOCKET s = iv_win32_fd_to_socket(fd);
	u_long yes = 1;
	BOOL oobinline = TRUE;

	/*
	 * Only sockets can be polled on Windows; anything else is
	 * rejected by the poll method at registration time.
	 */
	if (s == INVALID_SOCKET)
		return;

	ioctlsocket(s, FIONBIO, &yes);
	setsockopt(s, SOL_SOCKET, SO_OOBINLINE,
		   (const char *)&oobinline, sizeof(oobinline));
}
