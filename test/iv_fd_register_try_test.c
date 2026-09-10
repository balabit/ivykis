/*
 * ivykis, an event handling library
 * Copyright (C) 2026 Hofi <hofione@gmail.com>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <stdlib.h>
#include <unistd.h>

#include <iv.h>

static void
handle_input(void *cookie)
{
  (void)cookie;
}

int
main(void)
{
  int pipe_fds[2];
  struct iv_fd fd;
  int result;

  if (setenv("IV_SELECT_POLL_METHOD", "kqueue", 1) < 0)
    return 1;

  if (pipe(pipe_fds) < 0)
    return 1;

  iv_init();

  IV_FD_INIT(&fd);
  fd.fd = pipe_fds[0];
  fd.handler_in = handle_input;

  result = iv_fd_register_try(&fd);
  if (result == 0)
    iv_fd_unregister(&fd);

  iv_deinit();
  close(pipe_fds[0]);
  close(pipe_fds[1]);

  return result != 0;
}