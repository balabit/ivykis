/*
 * ivykis, an event handling library
 * Copyright (C) 2026 Hofi <hofione@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License version 2.1 for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License version 2.1 along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/*
 * Regression test for the kqueue backend incorrectly calling iv_fatal()
 * when kevent() returns EV_ERROR on an fd (e.g. EVFILT_WRITE on a pipe
 * whose read end has been closed).  The epoll backend handles the same
 * condition via EPOLLERR/EPOLLHUP.  Both backends must invoke handler_err
 * rather than aborting.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <iv.h>

static struct iv_fd wfd;
static int success;

static void pipe_error(void *_dummy)
{
  fprintf(stdout, "iv_fd_pipe_error_test: handler_err is called\n");
  iv_fd_unregister(&wfd);
  close(wfd.fd);

  success = 1;
  iv_quit();
}

/*
 * Dummy write handler: on some backends (e.g. epoll) the fd may first
 * appear writable before the HUP/error condition is visible.  We do
 * nothing here; the error handler will fire on the same or a subsequent
 * poll iteration.
 */
static void pipe_writable(void *_dummy)
{
}

int main(void)
{
  int pfd[2];

  /* Abort the test after 5 seconds if nothing fires. */
  alarm(5);

  if (pipe(pfd) < 0)
    {
      perror("pipe");
      return 1;
    }

  /*
   * Close the read end before registering the write end.  This
   * causes kqueue to return EV_ERROR (EPIPE) when EVFILT_WRITE is
   * added, and epoll to return EPOLLHUP immediately.  Both should
   * invoke handler_err, not abort.
   */
  close(pfd[0]);

  iv_init();

  IV_FD_INIT(&wfd);
  wfd.fd = pfd[1];
  wfd.handler_out = pipe_writable;  /* ensures EVFILT_WRITE is registered */
  wfd.handler_err = pipe_error;
  iv_fd_register(&wfd);

  iv_main();

  iv_deinit();

  if (!success)
    {
      fprintf(stderr, "iv_fd_pipe_error_test: handler_err was not called\n");
      return 1;
    }

  return 0;
}
