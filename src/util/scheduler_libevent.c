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
#include "gnunet_scheduler_lib.h"
#include "disk.h"
//#define DEBUG
#include <inttypes.h>
#include <event2/event.h>

#define LOG(kind, ...) GNUNET_log_from (kind, "util-scheduler", __VA_ARGS__)

#define LOG_STRERROR(kind, syscall) GNUNET_log_from_strerror (kind, \
                                                              "util-scheduler", \
                                                              syscall)
static struct event_base *base;
static int activate_fd;
static struct Scheduled *parent_task;

static bool activated = false;

struct callback_context
{
  struct GNUNET_SCHEDULER_Handle *handle;
  struct Scheduled *task;
  struct event *evt;
  struct DriverContext *driver_context;
};

static int
libevent_add (void *cls,
            struct GNUNET_SCHEDULER_Task *task,
            struct GNUNET_SCHEDULER_FdInfo *fdi)
{
  struct DriverContext *context = cls;

  LOG (GNUNET_ERROR_TYPE_DEBUG,
      "libevent add\n");
      
  GNUNET_assert (NULL != context);
  GNUNET_assert (NULL != task);
  GNUNET_assert (NULL != fdi);
  GNUNET_assert (0 != (GNUNET_SCHEDULER_ET_IN & fdi->et) ||
                 0 != (GNUNET_SCHEDULER_ET_OUT & fdi->et));

  if (! ((NULL != fdi->fd) ^ (NULL != fdi->fh)) || (fdi->sock < 0))
  {
    // exactly one out of {fd, hf} must be != NULL and the OS handle must be valid 
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

  LOG (GNUNET_ERROR_TYPE_DEBUG,
      "libevent del\n");
      
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

  LOG (GNUNET_ERROR_TYPE_DEBUG,
      "libevent set wakeup\n");

  GNUNET_assert (NULL != context);
  context->timeout = dt;
      
}

static void event_callback(evutil_socket_t fd, short events, void *param) 
{

  LOG (GNUNET_ERROR_TYPE_DEBUG,
      "libevent event callback running\n");

  struct callback_context *ctx = (struct callback_context*) param;

  if (events == EV_TIMEOUT) {
    LOG (GNUNET_ERROR_TYPE_DEBUG, "libevent timeout only event, event not triggered -> don't do anything\n");
    return;
  }

  GNUNET_assert (NULL != ctx);
  GNUNET_assert (NULL != ctx->task);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "libevent callback set task ready: %p\n", ctx->task);

  GNUNET_SCHEDULER_task_ready (ctx->task->task, ctx->task->fdi);
}


/**
* For debugging purposes, logging internal libevent callback functions 
*/
static void log_callback(int sev, const char* msg) 
{
  switch (sev) {
    case _EVENT_LOG_DEBUG:  LOG(GNUNET_ERROR_TYPE_DEBUG, 
                               "libevent - log_callback: %s\n", msg); 
                            break;
    case _EVENT_LOG_WARN:   LOG(GNUNET_ERROR_TYPE_WARNING, 
                              "libevent - log_callback: %s\n", msg); 
                            break;
    case _EVENT_LOG_ERR:    LOG(GNUNET_ERROR_TYPE_ERROR, 
                             "libevent - log_callback: %s\n", msg); 
                            break;
    case _EVENT_LOG_MSG:

    default: LOG(GNUNET_ERROR_TYPE_INFO, 
                 "libevent - log_callback: %s\n", msg); break;
  }
}

/**
* For debugging purposes, logging fatal callback 
*/
static void fatal_callback(int err) 
{
  LOG(GNUNET_ERROR_TYPE_ERROR, "libevent fatal_callback with error: %v\n", err);
}

static void libevent_post_do_work(struct GNUNET_SCHEDULER_Handle *sh, 
    struct GNUNET_SCHEDULER_Task *active_task) 
{

  GNUNET_NETWORK_fdset_zero (sh->rs);
  GNUNET_NETWORK_fdset_zero (sh->ws);
  for (unsigned int i = 0; i != active_task->fds_len; ++i)
  {
    struct GNUNET_SCHEDULER_FdInfo *fdi = &active_task->fds[i];
    if (0 != (GNUNET_SCHEDULER_ET_IN & fdi->et))
    {
      GNUNET_NETWORK_fdset_set_native (sh->rs,
                                       fdi->sock);
    }
    if (0 != (GNUNET_SCHEDULER_ET_OUT & fdi->et))
    {
      GNUNET_NETWORK_fdset_set_native (sh->ws,
                                       fdi->sock);
    }
  }
}

/**
 * Called at the end of GNUNET_SCHEDULER_driver_init().
 * It actually identifies which is the shutdown pipe, which needs to run when 
 * all events have been handled and the loop will shut down.
 *
*/
static void libevent_activate_loop (struct GNUNET_SCHEDULER_Handle *sh, 
    const struct GNUNET_DISK_FileHandle *fh) 
{

  activate_fd = fh->fd;
  //
  //
  sh->rs = GNUNET_NETWORK_fdset_create ();
  sh->ws = GNUNET_NETWORK_fdset_create ();
  GNUNET_NETWORK_fdset_handle_set (sh->rs, fh);
}

/**
 * Called during GNUNET_SCHEDULER_run() after GNUNET_SCHEDULER_driver_init (driver).
 *
 * Runs the event loop with libevent. 
 *
 * A loop iterates the scheduled events, which are added via event_add() and then dispatched.
 * Due to the architecture of the GNUNET_SCHEDULER, the loop is run **once** (EVLOOP_ONCE):
 * <quote>Block until we have an active event, then exit once all active events have had their callbacks run.</quote>
 *
 * After all active events have fired, #GNUNET_SCHEDULER_do_work() runs in order to execute the callbacks.
 *
 * Then the event loop is restarted for subsequent scheduled events.
 * Events are activated via the #event_callback() function.
 *
*/
static int
libevent_event_loop (struct GNUNET_SCHEDULER_Handle *sh,
             struct DriverContext *context)
{
  int dispatch_result;
  struct event *evt;
  struct timeval *timeout;
  timeout = GNUNET_new (struct timeval);

  GNUNET_assert (NULL != context);

  LOG(GNUNET_ERROR_TYPE_DEBUG,"running libevent_event_loop\n");

  while ((NULL != context->scheduled_head) ||
         (GNUNET_TIME_UNIT_FOREVER_ABS.abs_value_us !=
          context->timeout.abs_value_us))
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG,
         "libevent timeout = %s\n",
         GNUNET_STRINGS_absolute_time_to_string (context->timeout));

    for (struct Scheduled *pos = context->scheduled_head;
         NULL != pos;
         pos = pos->next)
    {
      struct GNUNET_TIME_Relative time_remaining =
        GNUNET_TIME_absolute_get_remaining (context->timeout);

    if (time_remaining.rel_value_us / GNUNET_TIME_UNIT_SECONDS.rel_value_us > (unsigned long long) LONG_MAX)
    {
      timeout->tv_sec = LONG_MAX;
      timeout->tv_usec = 999999L;
    } else 
    {
    timeout->tv_sec = (long) (time_remaining.rel_value_us
                        / GNUNET_TIME_UNIT_SECONDS.rel_value_us);
    timeout->tv_usec =
      (time_remaining.rel_value_us
       - (timeout->tv_sec * GNUNET_TIME_UNIT_SECONDS.rel_value_us));
    }

        struct callback_context *ctx = GNUNET_new(struct callback_context);
        ctx->handle = sh;
        ctx->task = pos;
        ctx->driver_context = context;
      if ( (pos->et & GNUNET_SCHEDULER_ET_IN)  != 0) {
        evt = event_new(base, pos->fdi->sock, EV_READ, event_callback, ctx);
      } else if ( (pos->et & GNUNET_SCHEDULER_ET_OUT) != 0) {
        evt = event_new(base, pos->fdi->sock, EV_WRITE, event_callback, ctx);
        /* TODO: is this actually needed and how? */
      } else {
        evt = event_new(base, pos->fdi->sock, EV_SIGNAL|EV_PERSIST, event_callback, ctx);
      }
      ctx->evt = evt;
      int addResult = event_add(evt, (time_remaining.rel_value_us == GNUNET_TIME_UNIT_FOREVER_REL.rel_value_us) ? NULL : timeout);
      if (0 != addResult) {
        LOG (GNUNET_ERROR_TYPE_ERROR,
          "error adding event");
        return GNUNET_SYSERR; 
      }

      if (pos->fdi->sock == activate_fd) {
        event_active(evt, EV_READ,0);
      }
    }

    LOG(GNUNET_ERROR_TYPE_DEBUG,"libevent dispatching events (starting loop).\n");

    dispatch_result = event_base_loop(base, EVLOOP_ONCE);
    if (dispatch_result < 0) {
      LOG (GNUNET_ERROR_TYPE_ERROR,
          "error dispatching events");
      return GNUNET_SYSERR; 
    }
    if (dispatch_result == 1) {
      LOG (GNUNET_ERROR_TYPE_DEBUG,
           "event dispatch no events pending or active");
    }

    if (GNUNET_YES == GNUNET_SCHEDULER_do_work (sh))
    {
      LOG (GNUNET_ERROR_TYPE_DEBUG,
           "scheduler has more tasks ready!\n");
    }
  }

  LOG(GNUNET_ERROR_TYPE_DEBUG,"libevent_event_loop done.");
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

  LOG (GNUNET_ERROR_TYPE_DEBUG,
      "setting up libevent for event loop\n");
#if DEBUG      
  event_set_log_callback(log_callback);
  event_enable_debug_logging(EVENT_DBG_ALL);
  event_set_fatal_callback(fatal_callback);
#endif


  base = event_base_new(); 
  if (!base)
  {
    LOG (GNUNET_ERROR_TYPE_ERROR,
        "could not create libevent event base!\n");
    return NULL; 
  }

  libevent_driver->add = &libevent_add;
  libevent_driver->del = &libevent_del;
  libevent_driver->set_wakeup = &libevent_set_wakeup;
  libevent_driver->event_loop = &libevent_event_loop;
  libevent_driver->activate_loop = &libevent_activate_loop;
  libevent_driver->post_do_work = &libevent_post_do_work;

  return libevent_driver;
}
