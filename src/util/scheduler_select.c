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
 * @file util/scheduler_select.c
 * @brief implementation of scheduler driver selection 
 * @author Fabio Barone 
 */

#include "platform.h"
#include "gnunet_util_lib.h"
#include "disk.h"
// DEBUG
#include <inttypes.h>

#define LOG(kind, ...) GNUNET_log_from (kind, "util-scheduler", __VA_ARGS__)

#define LOG_STRERROR(kind, syscall) GNUNET_log_from_strerror (kind, \
                                                              "util-scheduler", \
                                                              syscall)
static int
select_add (void *cls,
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
select_del (void *cls,
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
select_set_wakeup (void *cls,
                   struct GNUNET_TIME_Absolute dt)
{
  struct DriverContext *context = cls;

  GNUNET_assert (NULL != context);
  context->timeout = dt;
}


static int
select_event_loop (struct GNUNET_SCHEDULER_Handle *sh,
             struct DriverContext *context)
{
  LOG (GNUNET_ERROR_TYPE_DEBUG,
      "using select for event loop\n");
      
  struct GNUNET_NETWORK_FDSet *rs;
  struct GNUNET_NETWORK_FDSet *ws;
  int select_result;

  GNUNET_assert (NULL != context);
  rs = GNUNET_NETWORK_fdset_create ();
  ws = GNUNET_NETWORK_fdset_create ();
  while ((NULL != context->scheduled_head) ||
         (GNUNET_TIME_UNIT_FOREVER_ABS.abs_value_us !=
          context->timeout.abs_value_us))
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG,
         "select timeout = %s\n",
         GNUNET_STRINGS_absolute_time_to_string (context->timeout));

    GNUNET_NETWORK_fdset_zero (rs);
    GNUNET_NETWORK_fdset_zero (ws);

    for (struct Scheduled *pos = context->scheduled_head;
         NULL != pos;
         pos = pos->next)
    {
      if (0 != (GNUNET_SCHEDULER_ET_IN & pos->et))
      {
        GNUNET_NETWORK_fdset_set_native (rs, pos->fdi->sock);
      }
      if (0 != (GNUNET_SCHEDULER_ET_OUT & pos->et))
      {
        GNUNET_NETWORK_fdset_set_native (ws, pos->fdi->sock);
      }
    }
    struct GNUNET_TIME_Relative time_remaining =
      GNUNET_TIME_absolute_get_remaining (context->timeout);
    /*
    if (NULL == scheduler_select)
    {
    */
      select_result = GNUNET_NETWORK_socket_select (rs,
                                                    ws,
                                                    NULL,
                                                    time_remaining);
      /*
    }
    else
    {
      select_result = scheduler_select (scheduler_select_cls,
                                        rs,
                                        ws,
                                        NULL,
                                        time_remaining);
    }
    */
    if (select_result == GNUNET_SYSERR)
    {
      if (errno == EINTR)
        continue;

      LOG_STRERROR (GNUNET_ERROR_TYPE_ERROR,
                    "select");
#if USE_LSOF
      char lsof[512];

      snprintf (lsof,
                sizeof(lsof),
                "lsof -p %d",
                getpid ());
      (void) close (1);
      (void) dup2 (2, 1);
      if (0 != system (lsof))
        LOG_STRERROR (GNUNET_ERROR_TYPE_WARNING,
                      "system");
#endif
#if DEBUG_FDS
      for (struct Scheduled *s = context->scheduled_head;
           NULL != s;
           s = s->next)
      {
        int flags = fcntl (s->fdi->sock,
                           F_GETFD);

        if ((flags == -1) &&
            (EBADF == errno))
        {
          LOG (GNUNET_ERROR_TYPE_ERROR,
               "Got invalid file descriptor %d!\n",
               s->fdi->sock);
#if EXECINFO
          dump_backtrace (s->task);
#endif
        }
      }
#endif
      GNUNET_assert (0);
      GNUNET_NETWORK_fdset_destroy (rs);
      GNUNET_NETWORK_fdset_destroy (ws);
      return GNUNET_SYSERR;
    }
    if (select_result > 0)
    {
      for (struct Scheduled *pos = context->scheduled_head;
           NULL != pos;
           pos = pos->next)
      {
        int is_ready = GNUNET_NO;

        if ((0 != (GNUNET_SCHEDULER_ET_IN & pos->et)) &&
            (GNUNET_YES ==
             GNUNET_NETWORK_fdset_test_native (rs,
                                               pos->fdi->sock)) )
        {
          pos->fdi->et |= GNUNET_SCHEDULER_ET_IN;
          is_ready = GNUNET_YES;
        }
        if ((0 != (GNUNET_SCHEDULER_ET_OUT & pos->et)) &&
            (GNUNET_YES ==
             GNUNET_NETWORK_fdset_test_native (ws,
                                               pos->fdi->sock)) )
        {
          pos->fdi->et |= GNUNET_SCHEDULER_ET_OUT;
          is_ready = GNUNET_YES;
        }
        if (GNUNET_YES == is_ready)
        {
          GNUNET_SCHEDULER_task_ready (pos->task,
                                       pos->fdi);
        }
      }
    }
    if (GNUNET_YES == GNUNET_SCHEDULER_do_work (sh))
    {
      LOG (GNUNET_ERROR_TYPE_DEBUG,
           "scheduler has more tasks ready!\n");
    }
  }
  GNUNET_NETWORK_fdset_destroy (rs);
  GNUNET_NETWORK_fdset_destroy (ws);
  return GNUNET_OK;
}
/**
 * Obtain the driver for using select() as the event loop.
 *
 * @return NULL on error
 */
struct GNUNET_SCHEDULER_Driver *
GNUNET_SCHEDULER_driver_select ()
{
  struct GNUNET_SCHEDULER_Driver *select_driver;

  select_driver = GNUNET_new (struct GNUNET_SCHEDULER_Driver);

  select_driver->add = &select_add;
  select_driver->del = &select_del;
  select_driver->set_wakeup = &select_set_wakeup;
  select_driver->event_loop = &select_event_loop;

  return select_driver;
}


