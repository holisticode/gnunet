/*
      This file is part of GNUnet
      (C) 2008--2012 Christian Grothoff (and other contributing authors)

      GNUnet is free software; you can redistribute it and/or modify
      it under the terms of the GNU General Public License as published
      by the Free Software Foundation; either version 3, or (at your
      option) any later version.

      GNUnet is distributed in the hope that it will be useful, but
      WITHOUT ANY WARRANTY; without even the implied warranty of
      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
      General Public License for more details.

      You should have received a copy of the GNU General Public License
      along with GNUnet; see the file COPYING.  If not, write to the
      Free Software Foundation, Inc., 59 Temple Place - Suite 330,
      Boston, MA 02111-1307, USA.
 */

/**
 * @file testbed/testbed_api_hosts.h
 * @brief internal API to access the 'hosts' subsystem
 * @author Christian Grothoff
 */

#ifndef NEW_TESTING_API_HOSTS_H
#define NEW_TESTING_API_HOSTS_H

#include "gnunet_testbed_service.h"
#include "testbed_helper.h"


/**
 * Lookup a host by ID.
 *
 * @param id global host ID assigned to the host; 0 is
 *        reserved to always mean 'localhost'
 * @return handle to the host, NULL on error
 */
struct GNUNET_TESTBED_Host *
GNUNET_TESTBED_host_lookup_by_id_ (uint32_t id);


/**
 * Create a host by ID; given this host handle, we could not
 * run peers at the host, but we can talk about the host
 * internally.
 *
 * @param id global host ID assigned to the host; 0 is
 *        reserved to always mean 'localhost'
 * @return handle to the host, NULL on error
 */
struct GNUNET_TESTBED_Host *
GNUNET_TESTBED_host_create_by_id_ (uint32_t id);


/**
 * Obtain a host's unique global ID.
 *
 * @param host handle to the host, NULL means 'localhost'
 * @return id global host ID assigned to the host (0 is
 *         'localhost', but then obviously not globally unique)
 */
uint32_t
GNUNET_TESTBED_host_get_id_ (const struct GNUNET_TESTBED_Host *host);


/**
 * Obtain the host's username
 *
 * @param host handle to the host, NULL means 'localhost'
 * @return username to login to the host
 */
const char *
GNUNET_TESTBED_host_get_username_ (const struct GNUNET_TESTBED_Host *host);


/**
 * Obtain the host's ssh port
 *
 * @param host handle to the host, NULL means 'localhost'
 * @return username to login to the host
 */
uint16_t
GNUNET_TESTBED_host_get_ssh_port_ (const struct GNUNET_TESTBED_Host *host);


/**
 * Marks a host as registered with a controller
 *
 * @param host the host to mark
 * @param controller the controller at which this host is registered
 */
void
GNUNET_TESTBED_mark_host_registered_at_ (struct GNUNET_TESTBED_Host *host,
                                         const struct GNUNET_TESTBED_Controller
                                         *controller);


/**
 * Checks whether a host has been registered with the given controller
 *
 * @param host the host to check
 * @param controller the controller at which host's registration is checked
 * @return GNUNET_YES if registered; GNUNET_NO if not
 */
int
GNUNET_TESTBED_is_host_registered_ (const struct GNUNET_TESTBED_Host *host,
                                    const struct GNUNET_TESTBED_Controller
                                    *controller);


/**
 * (re)sets the operation queue for parallel overlay connects
 *
 * @param h the host handle
 * @param npoc the number of parallel overlay connects - the queue size
 */
void
GNUNET_TESTBED_set_num_parallel_overlay_connects_ (struct
                                                   GNUNET_TESTBED_Host *h,
                                                   unsigned int npoc);


/**
 * Releases a time slot thus making it available for be used again
 *
 * @param h the host handle
 * @param index the index of the the time slot
 * @param key the key to prove ownership of the timeslot
 * @return GNUNET_YES if the time slot is successfully removed; GNUNET_NO if the
 *           time slot cannot be removed - this could be because of the index
 *           greater than existing number of time slots or `key' being different
 */
int
GNUNET_TESTBED_release_time_slot_ (struct GNUNET_TESTBED_Host *h,
                                   unsigned int index, void *key);


/**
 * Function to update a time slot
 *
 * @param h the host handle
 * @param index the index of the time slot to update
 * @param key the key to identify ownership of the slot
 * @param time the new time
 * @param failed should this reading be treated as coming from a fail event
 */
void
GNUNET_TESTBED_update_time_slot_ (struct GNUNET_TESTBED_Host *h,
                                  unsigned int index, void *key,
                                  struct GNUNET_TIME_Relative time, int failed);


/**
 * Returns a timing slot which will be exclusively locked
 *
 * @param h the host handle
 * @param key a pointer which is associated to the returned slot; should not be
 *          NULL. It serves as a key to determine the correct owner of the slot
 * @return the time slot index in the array of time slots in the controller
 *           handle
 */
unsigned int
GNUNET_TESTBED_get_tslot_ (struct GNUNET_TESTBED_Host *h, void *key);


/**
 * Queues the given operation in the queue for parallel overlay connects of the
 * given host
 *
 * @param h the host handle
 * @param op the operation to queue in the given host's parally overlay connect
 *          queue 
 */
void
GNUNET_TESTBED_host_queue_oc (struct GNUNET_TESTBED_Host *h, 
                              struct GNUNET_TESTBED_Operation *op);

#endif
/* end of testbed_api_hosts.h */
