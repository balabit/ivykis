/*
 * ivykis, an event handling library
 * Copyright (C) 2026 Laszlo Varady <laszlo.varady@axoflow.com>
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
 * Regression test for the kqueue backend aborting via iv_fatal() when
 * tearing down an fd whose peer has closed.  The forked child closes
 * both ends of the pipe, which causes EVFILT_WRITE to fire with EV_EOF
 * on the parent's write end.  The handler then writes (returning EPIPE)
 * and unregisters the fd; the subsequent EV_DELETE may target a filter
 * the kernel has already auto-removed, returning ENOENT/EBADF.  The
 * kqueue backend must tolerate this rather than aborting the process.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <iv.h>

/*
 * This test is based on the work of Laszlo Varady <laszlo.varady@axoflow.com>
 * created for reproducing the issue described in https://github.com/buytenh/ivykis/issues/26
 */

static void fatal(const char *msg)
{
  perror(msg);
  exit(1);
}

static void write_fd(void *c)
{
  struct iv_fd *fd = (struct iv_fd *)c;
  const char *buf = "hello\n";
  int rc = write(fd->fd, buf, strlen(buf) + 1);

  if (rc < 0)
    iv_fd_unregister(fd);
}

int main(void)
{
  int pp[2];
  pid_t pid;
  struct iv_fd fd;
  int rc;

  /* Abort the test after 10 seconds if nothing fires. */
  alarm(10);

  iv_init();

  if (pipe(pp) < 0)
    fatal("pipe()");

  pid = fork();
  if (pid < 0)
    fatal("fork()");

  if (pid == 0)
    {
      close(pp[1]);
      close(pp[0]);
      return 0;
    }

  close(pp[0]);

  IV_FD_INIT(&fd);
  fd.fd = pp[1];
  fd.cookie = &fd;
  fd.handler_out = write_fd;
  iv_fd_register(&fd);

  iv_main();
  iv_deinit();

  close(pp[1]);

  if (wait(&rc) < 0)
    fatal("wait()");

  return 0;
}
