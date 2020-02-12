/*
      This file is part of GNUnet
      Copyright (C) 2009-2020 GNUnet e.V.

      GNUnet is free software: you can redistribute it and/or modify it
      under the terms of the GNU Affero General Public License as published
      by the Free Software Foundation, either version 3 of the License,
      or (at your option) any later version.

      GNUnet is distributed in the hope that it will be useful, but
      WITHOUT ANY WARRANTY; without even the implied warranty of
      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
      Affero General Public License for more details.

      You should have received a copy of the GNU Affero General Public License
      along with this program.  If not, see <http://www.gnu.org/licenses/>.

     SPDX-License-Identifier: AGPL3.0-or-later
 */
/**
 * @file util/scheduler_libevent.c
 * @brief libevent implementation of scheduler selection 
 * @author Fabio Barone 
 */
#include "platform.h"
#include "gnunet_util_lib.h"
#include "disk.h"
#define DEBUG
#include <inttypes.h>
#include <event2/event.h>

#define LOG(kind, ...) GNUNET_log_from (kind, "util-scheduler", __VA_ARGS__)

#define LOG_STRERROR(kind, syscall) GNUNET_log_from_strerror (kind, \
                                                              "util-scheduler", \
                                                              syscall)
static struct event_base *event_base;

static int
libevent_add (void *cls,
            struct GNUNET_SCHEDULER_Task *task,
            struct GNUNET_SCHEDULER_FdInfo *fdi)
{
  struct DriverContext *context = cls;

  GNUNET_assert (NULL != context);
  GNUNET_assert (NULL != task);
  GNUNET_assert (NULL != fdi);
  GNUNET_assert (0 != (GNUNET_SCHEDULER_ET_IN & fdi->et) ||
                 0 != (GNUNET_SCHEDULER_ET_OUT & fdi->et));

  if (! ((NULL != fdi->fd) ^ (NULL != fdi->fh)) || (fdi->sock < 0))
  {
    /* exactly one out of {fd, hf} must be != NULL and the OS handle must be valid */
    return GNUNET_SYSERR;
  }

  struct Scheduled *scheduled = GNUNET_new (struct Scheduled);
  scheduled->task = task;
  scheduled->fdi = fdi;
  scheduled->et = fdi->et;

  GNUNET_CONTAINER_DLL_insert (context->scheduled_head,
                               context->scheduled_tail,
                               scheduled);
  return GNUNET_OK;
}


static int
libevent_del (void *cls,
            struct GNUNET_SCHEDULER_Task *task)
{
  struct DriverContext *context;
  struct Scheduled *pos;
  int ret;

  GNUNET_assert (NULL != cls);

  context = cls;
  ret = GNUNET_SYSERR;
  pos = context->scheduled_head;
  while (NULL != pos)
  {
    struct Scheduled *next = pos->next;
    if (pos->task == task)
    {
      GNUNET_CONTAINER_DLL_remove (context->scheduled_head,
                                   context->scheduled_tail,
                                   pos);
      GNUNET_free (pos);
      ret = GNUNET_OK;
    }
    pos = next;
  }
  return ret;
}

static void
libevent_set_wakeup (void *cls,
                   struct GNUNET_TIME_Absolute dt)
{
  struct DriverContext *context = cls;

  GNUNET_assert (NULL != context);
  context->timeout = dt;
}

void event_callback(evutil_socket_t fd, short events, void *task) {
  LOG (GNUNET_ERROR_TYPE_DEBUG,
      "event callback called\n");
}

static int
libevent_event_loop (struct GNUNET_SCHEDULER_Handle *sh,
             struct DriverContext *context)
{
  LOG (GNUNET_ERROR_TYPE_DEBUG,
      "using libevent for event loop\n");
      
  LOG(GNUNET_ERROR_TYPE_DEBUG,"libevent");
  event_base = event_base_new(); 
  if (!event_base)
  {
    LOG (GNUNET_ERROR_TYPE_ERROR,
        "could not create libevent event base!\n");
    return GNUNET_SYSERR; 
  }

  LOG(GNUNET_ERROR_TYPE_DEBUG,"event base ready");
  int dispatch_result;
  struct event *evt;
  struct timeval *timeout;

  timeout = GNUNET_new (struct timeval);

  timeout->tv_sec = context->timeout.abs_value_us / 1000;
  timeout->tv_usec =(context->timeout.abs_value_us % 1000) * 1000;

  GNUNET_assert (NULL != context);

  LOG(GNUNET_ERROR_TYPE_DEBUG,"entering loop");
    for (struct Scheduled *pos = context->scheduled_head;
         NULL != pos;
         pos = pos->next)
    {
      LOG(GNUNET_ERROR_TYPE_DEBUG,"adding task");
      evt = event_new(event_base, pos->fdi->sock, EV_READ|EV_WRITE, event_callback, pos->task);
      event_add(evt, timeout);
    }

    LOG(GNUNET_ERROR_TYPE_DEBUG,"loop done. dispatching");
  dispatch_result = event_base_dispatch(event_base);

  LOG(GNUNET_ERROR_TYPE_DEBUG,"do work?");
  if (GNUNET_YES == GNUNET_SCHEDULER_do_work (sh))
  {
   LOG(GNUNET_ERROR_TYPE_DEBUG,"do work yes");
    LOG (GNUNET_ERROR_TYPE_DEBUG,
         "scheduler has more tasks ready!\n");
  }

  if (dispatch_result < 0) {
    LOG (GNUNET_ERROR_TYPE_ERROR,
        "error dispatching events");
    return GNUNET_SYSERR; 
  }

  if (dispatch_result == 1) {
    LOG (GNUNET_ERROR_TYPE_DEBUG,
         "event dispatch no events pending or active");
  }

  LOG(GNUNET_ERROR_TYPE_DEBUG,"done");
  return GNUNET_OK;
}

/**
 * Obtain the driver for using libevent() as the event loop.
 *
 * @return NULL on error
 */
struct GNUNET_SCHEDULER_Driver *
GNUNET_SCHEDULER_driver_libevent ()
{
  struct GNUNET_SCHEDULER_Driver *libevent_driver;

  libevent_driver = GNUNET_new (struct GNUNET_SCHEDULER_Driver);

  libevent_driver->add = &libevent_add;
  libevent_driver->del = &libevent_del;
  libevent_driver->set_wakeup = &libevent_set_wakeup;
  libevent_driver->event_loop = &libevent_event_loop;

  return libevent_driver;
}

