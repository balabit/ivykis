/*
 * ivykis, an event handling library
 * Copyright (C) 2002, 2003, 2009, 2012, 2013 Lennert Buytenhek
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
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include "iv_private.h"

#ifndef EVFILT_USER
#define EVFILT_USER (-11)
#endif

#ifndef NOTE_TRIGGER
#define NOTE_TRIGGER  0x01000000
#endif


#define UPLOAD_BATCH  1024

static int iv_fd_kqueue_init(struct iv_state *st)
{
  int kqueue_fd;

  kqueue_fd = kqueue();
  if (kqueue_fd < 0)
    return -1;

  iv_fd_set_cloexec(kqueue_fd);

  INIT_IV_LIST_HEAD(&st->u.kqueue.notify);
  st->u.kqueue.kqueue_fd = kqueue_fd;
  st->u.kqueue.timeout_pending = 0;

  return 0;
}

static void
iv_fd_kqueue_queue_one(struct kevent *kev, int *_num, struct iv_fd_ *fd)
{
  int num;
  int wanted;
  int regd;

  iv_list_del_init(&fd->list_notify);

  num = *_num;
  wanted = fd->wanted_bands;
  regd = fd->registered_bands;

  if (!(wanted & MASKIN) && (regd & MASKIN))
    {
      EV_SET(&kev[num], fd->fd, EVFILT_READ, EV_DELETE,
             0, 0, (void *)(intptr_t)fd);
      num++;
    }
  else if ((wanted & MASKIN) && !(regd & MASKIN))
    {
      EV_SET(&kev[num], fd->fd, EVFILT_READ, EV_ADD | EV_ENABLE,
             0, 0, (void *)(intptr_t)fd);
      num++;
    }

  if (!(wanted & MASKOUT) && (regd & MASKOUT))
    {
      EV_SET(&kev[num], fd->fd, EVFILT_WRITE, EV_DELETE,
             0, 0, (void *)(intptr_t)fd);
      num++;
    }
  else if ((wanted & MASKOUT) && !(regd & MASKOUT))
    {
      EV_SET(&kev[num], fd->fd, EVFILT_WRITE, EV_ADD | EV_ENABLE,
             0, 0, (void *)(intptr_t)fd);
      num++;
    }

  *_num = num;
}

/*
 * Apply a changelist with EV_RECEIPT so kqueue reports a result for
 * every change.  Without an eventlist, kevent() stops at the first
 * failure and returns -1.
 *
 * Tolerate ENOENT/EBADF for EV_DELETE: the kernel may already have
 * auto-removed that filter (e.g. pipe EOF/close race).  Other errors
 * remain fatal.
 */
static int __kevent_retry(int kq, const struct kevent *changelist, int nchanges)
{
  if (nchanges <= 0)
    return 0;

  struct kevent changes[nchanges];
  for (int i = 0; i < nchanges; i++)
    {
      changes[i] = changelist[i];
      changes[i].flags |= EV_RECEIPT;
    }

  struct timespec to = { 0, 0 };
  int ret;
  do
    {
      ret = kevent(kq, changes, nchanges, changes, nchanges, &to);
    }
  while (ret < 0 && errno == EINTR);

  if (ret < 0)
    return ret;

  for (int i = 0; i < ret; i++)
    {
      if (!(changes[i].flags & EV_ERROR))
        continue;
      if (changes[i].data == 0)
        continue;

      /*
       * Tolerate "filter not present" results for EV_DELETE: the
       * kernel may have auto-removed the filter (e.g. on pipe EOF)
       * between our last upload and this teardown change.
       */
      if ((changelist[i].flags & EV_DELETE) &&
          (changes[i].data == ENOENT || changes[i].data == EBADF))
        continue;

      errno = changes[i].data;
      return -1;
    }

  /* Preserve the helper's zero-on-success contract; EV_RECEIPT makes kevent() return receipt count. */
  return 0;
}

static void kevent_retry(char *name, struct iv_state *st,
                         const struct kevent *changelist, int nchanges)
{
  if (__kevent_retry(st->u.kqueue.kqueue_fd, changelist, nchanges) < 0)
    iv_fatal("%s: got error %d[%s]", name, errno, strerror(errno));
}

static void
iv_fd_kqueue_upload(struct iv_state *st, struct kevent *kev, int size, int *num)
{
  while (!iv_list_empty(&st->u.kqueue.notify))
    {
      struct iv_fd_ *fd;

      if (*num > size - 2)
        {
          kevent_retry("iv_fd_kqueue_upload", st, kev, *num);
          *num = 0;
        }

      fd = iv_list_entry(st->u.kqueue.notify.next,
                         struct iv_fd_, list_notify);

      iv_fd_kqueue_queue_one(kev, num, fd);
      fd->registered_bands = fd->wanted_bands;
    }
}

static int
iv_fd_kqueue_set_poll_timeout(struct iv_state *st, const struct timespec *abs)
{
  st->u.kqueue.timeout_pending = 1;
  st->u.kqueue.timeout = abs;

  return 1;
}

static void iv_fd_kqueue_clear_poll_timeout(struct iv_state *st)
{
  st->u.kqueue.timeout_pending = 1;
  st->u.kqueue.timeout = NULL;
}

static int iv_fd_kqueue_poll(struct iv_state *st,
                             struct iv_list_head *active,
                             const struct timespec *abs)
{
  int num;
  struct kevent kev[UPLOAD_BATCH];
  int run_timers;
  struct kevent batch[2 * st->numfds + 1];
  struct timespec rel;
  int ret;
  int run_events;
  int i;

  num = 0;

  if (st->u.kqueue.timeout_pending)
    {
      st->u.kqueue.timeout_pending = 0;

      if (st->u.kqueue.timeout != NULL)
        {
          EV_SET(&kev[num], (intptr_t)&st->u.kqueue.timeout,
                 EVFILT_TIMER,
                 EV_ADD | EV_ENABLE | EV_ONESHOT | EV_CLEAR, 0,
                 to_msec(st, st->u.kqueue.timeout), NULL);
        }
      else
        {
          EV_SET(&kev[num], (intptr_t)&st->u.kqueue.timeout,
                 EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        }
      num++;
    }

  iv_fd_kqueue_upload(st, kev, UPLOAD_BATCH, &num);

  run_timers = (abs != NULL) ? 1 : 0;

  ret = kevent(st->u.kqueue.kqueue_fd, kev, num,
               batch, ARRAY_SIZE(batch), to_relative(st, &rel, abs));

  __iv_invalidate_now(st);

  if (ret < 0)
    {
      if (errno == EINTR)
        return run_timers;

      iv_fatal("iv_fd_kqueue_poll: got error %d[%s]", errno,
               strerror(errno));
    }

  run_events = 0;
  for (i = 0; i < ret; i++)
    {
      struct iv_fd_ *fd;

      if (batch[i].filter == EVFILT_TIMER)
        {
          run_timers = 1;
          continue;
        }

      if (batch[i].filter == EVFILT_USER)
        {
          run_events = 1;
          continue;
        }

      /*
       * Per-fd error conditions are application-level, not internal
       * kqueue failures.  Mark the fd ready with MASKERR so the registered
       * error handler is dispatched by normal ready processing,
       * rather than bailing out, consistent with epoll's EPOLLERR/EPOLLHUP
       * handling.
       */

      /*
       * EV_ERROR: kqueue reports per-change failures (e.g.
       * EVFILT_WRITE EV_ADD on a pipe whose read end is already
       * closed -> EPIPE on FreeBSD) via an eventlist entry with
       * EV_ERROR set and data = errno.  macOS happens to accept
       * the same registration and surface the condition through
       * EV_EOF on the write filter instead, so this branch is a
       * no-op there for that scenario, but the contract applies
       * to every kqueue platform.
       */
      if (batch[i].flags & EV_ERROR)
        {
          fd = (void *)batch[i].udata;
          /*
           * The EV_ADD that produced this EV_ERROR was rejected
           * by the kernel, so the filter is NOT actually registered
           * in kqueue.  Clear the corresponding bit from
           * registered_bands to prevent a later EV_DELETE (issued
           * on unregister/close) from failing with ENOENT/EBADF
           * and tripping iv_fatal() in kevent_retry().
           */
          if (batch[i].filter == EVFILT_READ)
            {
              fd->registered_bands &= ~MASKIN;
              iv_fd_make_ready(active, fd, MASKIN | MASKERR);
            }
          else if (batch[i].filter == EVFILT_WRITE)
            {
              fd->registered_bands &= ~MASKOUT;
              iv_fd_make_ready(active, fd, MASKOUT | MASKERR);
            }
          else
            {
              fd->registered_bands &= ~(MASKIN | MASKOUT);
              iv_fd_make_ready(active, fd, MASKIN | MASKOUT | MASKERR);
            }
          continue;
        }

      fd = (void *)batch[i].udata;
      if (batch[i].filter == EVFILT_READ)
        {
          int bands = MASKIN;
          /* EV_EOF on read: write end closed; FreeBSD and macOS. */
          if (batch[i].flags & EV_EOF)
            bands |= MASKERR;
          iv_fd_make_ready(active, fd, bands);
        }
      else if (batch[i].filter == EVFILT_WRITE)
        {
          int bands = MASKOUT;
          /* EV_EOF on write: read end closed (macOS); FreeBSD reports EPIPE via EV_ERROR. */
          if (batch[i].flags & EV_EOF)
            bands |= MASKERR;
          iv_fd_make_ready(active, fd, bands);
        }
      else
        {
          iv_fatal("iv_fd_kqueue_poll: got message from filter %d",
                   batch[i].filter);
        }
    }

  if (run_events)
    iv_event_run_pending_events();

  return run_timers;
}

static void iv_fd_kqueue_upload_all(struct iv_state *st)
{
  struct kevent kev[UPLOAD_BATCH];
  int num;

  num = 0;
  iv_fd_kqueue_upload(st, kev, UPLOAD_BATCH, &num);

  if (num)
    kevent_retry("iv_fd_kqueue_upload_all", st, kev, num);
}

static void iv_fd_kqueue_unregister_fd(struct iv_state *st, struct iv_fd_ *fd)
{
  if (!iv_list_empty(&fd->list_notify))
    iv_fd_kqueue_upload_all(st);
}

static void iv_fd_kqueue_notify_fd(struct iv_state *st, struct iv_fd_ *fd)
{
  iv_list_del_init(&fd->list_notify);
  if (fd->registered_bands != fd->wanted_bands)
    iv_list_add_tail(&fd->list_notify, &st->u.kqueue.notify);
}

static int iv_fd_kqueue_notify_fd_sync(struct iv_state *st, struct iv_fd_ *fd)
{
  struct kevent kev;
  int ret;

  if (fd->wanted_bands & MASKIN)
    {
      EV_SET(&kev, fd->fd, EVFILT_READ, EV_ADD | EV_ENABLE,
             0, 0, (void *)(intptr_t)fd);

      ret = __kevent_retry(st->u.kqueue.kqueue_fd, &kev, 1);
      if (ret == 0)
        {
          fd->registered_bands |= MASKIN;
          return 0;
        }
    }

  if (fd->wanted_bands & MASKOUT)
    {
      EV_SET(&kev, fd->fd, EVFILT_WRITE, EV_ADD | EV_ENABLE,
             0, 0, (void *)(intptr_t)fd);

      ret = __kevent_retry(st->u.kqueue.kqueue_fd, &kev, 1);
      if (ret == 0)
        {
          fd->registered_bands |= MASKOUT;
          return 0;
        }
    }

  return -1;
}

static void iv_fd_kqueue_deinit(struct iv_state *st)
{
  close(st->u.kqueue.kqueue_fd);
}

static int iv_fd_kqueue_event_rx_on(struct iv_state *st)
{
  struct kevent add;
  int ret;

  EV_SET(&add, (intptr_t)st, EVFILT_USER,
         EV_ADD | EV_CLEAR | EV_ENABLE, 0, 0, NULL);

  ret = __kevent_retry(st->u.kqueue.kqueue_fd, &add, 1);
  if (ret == 0)
    st->numobjs++;

  return ret;
}

static void iv_fd_kqueue_event_rx_off(struct iv_state *st)
{
  struct kevent delete;

  EV_SET(&delete, (intptr_t)st, EVFILT_USER, EV_DELETE, 0, 0, NULL);
  kevent_retry("iv_fd_kqueue_event_rx_off", st, &delete, 1);

  st->numobjs--;
}

static void iv_fd_kqueue_event_send(struct iv_state *dest)
{
  struct kevent send;

  EV_SET(&send, (intptr_t)dest, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  kevent_retry("iv_fd_kqueue_event_send", dest, &send, 1);
}

const struct iv_fd_poll_method iv_fd_poll_method_kqueue =
{
  .name     = "kqueue",
  .init     = iv_fd_kqueue_init,
  .set_poll_timeout = iv_fd_kqueue_set_poll_timeout,
  .clear_poll_timeout = iv_fd_kqueue_clear_poll_timeout,
  .poll     = iv_fd_kqueue_poll,
  .unregister_fd    = iv_fd_kqueue_unregister_fd,
  .notify_fd    = iv_fd_kqueue_notify_fd,
  .notify_fd_sync   = iv_fd_kqueue_notify_fd_sync,
  .deinit     = iv_fd_kqueue_deinit,
  .event_rx_on    = iv_fd_kqueue_event_rx_on,
  .event_rx_off   = iv_fd_kqueue_event_rx_off,
  .event_send   = iv_fd_kqueue_event_send,
};
