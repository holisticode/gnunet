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
#include "gnunet_helper_lib.h"


/**
 * Wrapper around
 */
struct GNUNET_TESTBED_HelperHandle
{
  /**
   * The process handle
   */
  struct GNUNET_OS_Process *process;

  /**
   * Pipe connecting to stdin of the process.
   */
  struct GNUNET_DISK_PipeHandle *cpipe_in;

  /**
   * Pipe from the stdout of the process.
   */
  struct GNUNET_DISK_PipeHandle *cpipe_out;

  /**
   * The port number for ssh; used for helpers starting ssh
   */
  char *port;

  /**
   * The ssh destination string; used for helpers starting ssh
   */
  char *dst; 
};


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
 * Obtain the host's hostname.
 * 
 * @param host handle to the host, NULL means 'localhost'
 * @return hostname of the host
 */
const char *
GNUNET_TESTBED_host_get_hostname_ (const struct GNUNET_TESTBED_Host *host);


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
 * Opaque wrapper around GNUNET_HELPER_Handle
 */
struct GNUNET_TESTBED_HelperHandle;


/**
 * Run a given helper process at the given host.  Communication
 * with the helper will be via GNUnet messages on stdin/stdout.
 * Runs the process via 'ssh' at the specified host, or locally.
 * Essentially an SSH-wrapper around the 'gnunet_helper_lib.h' API.
 * 
 * @param host host to use, use "NULL" for localhost
 * @param binary_argv binary name and command-line arguments to give to the binary
 * @return handle to terminate the command, NULL on error
 */
struct GNUNET_TESTBED_HelperHandle *
GNUNET_TESTBED_host_run_ (const struct GNUNET_TESTBED_Host *host,
			  char *const binary_argv[]);


/**
 * Stops a helper in the HelperHandle using GNUNET_HELPER_stop
 *
 * @param handle the handle returned from GNUNET_TESTBED_host_start_
 */
void
GNUNET_TESTBED_host_stop_ (struct GNUNET_TESTBED_HelperHandle *handle);


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

#endif
/* end of testbed_api_hosts.h */
