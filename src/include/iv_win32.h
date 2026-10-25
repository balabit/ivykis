/*
 * ivykis, an event handling library
 *
 * Public helpers for using iv_fd with Winsock sockets when
 * ivykis is built with its POSIX core on Windows.
 *
 * iv_fd works on C runtime file descriptors.  Winsock SOCKETs are not
 * CRT fds, so a SOCKET has to be attached to a CRT fd before it can be
 * registered with iv_fd:
 *
 *	int fd = iv_win32_socket_to_fd(socket(AF_INET, SOCK_STREAM, 0));
 *	...
 *	send(iv_win32_fd_to_socket(fd), buf, len, 0);
 *	...
 *	iv_win32_close_socket_fd(fd);
 *
 * The CRT fd wraps a placeholder handle, and the fd -> SOCKET mapping
 * is kept in a process wide table, so that the CRT and Winsock each
 * close only the handle they own.  Sockets attached this way must be
 * closed with iv_win32_close_socket_fd(), never with close().
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 */

#ifndef __IV_WIN32_H
#define __IV_WIN32_H

#include <winsock2.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a new CRT fd for @s, or -1 (errno set) on failure. */
int iv_win32_socket_to_fd(SOCKET s);

/* Returns the SOCKET attached to @fd, or INVALID_SOCKET. */
SOCKET iv_win32_fd_to_socket(int fd);

/* Returns nonzero if @fd has a SOCKET attached. */
int iv_win32_fd_is_socket(int fd);

/* Closes both the CRT fd and the attached SOCKET. */
int iv_win32_close_socket_fd(int fd);

#ifdef __cplusplus
}
#endif

#endif
