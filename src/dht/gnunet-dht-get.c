/*
     This file is part of GNUnet.
     Copyright (C) 2001, 2002, 2004, 2005, 2006, 2007, 2009 GNUnet e.V.

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
     Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
     Boston, MA 02110-1301, USA.
*/
/**
 * @file dht/gnunet-dht-get.c
 * @brief search for data in DHT
 * @author Christian Grothoff
 * @author Nathan Evans
 */
#include "platform.h"
#include "gnunet_dht_service.h"

#define LOG(kind,...) GNUNET_log_from (kind, "dht-clients",__VA_ARGS__)
/**
 * The type of the query
 */
static unsigned int query_type;

/**
 * Desired replication level
 */
static unsigned int replication = 5;

/**
 * The key for the query
 */
static char *query_key;

/**
 * User supplied timeout value
 */
static struct GNUNET_TIME_Relative timeout_request = { 60000 };

/**
 * Be verbose
 */
static unsigned int verbose;

/**
 * Use DHT demultixplex_everywhere
 */
static int demultixplex_everywhere;

/**
 * Handle to the DHT
 */
static struct GNUNET_DHT_Handle *dht_handle;

/**
 * Global handle of the configuration
 */
static const struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Handle for the get request
 */
static struct GNUNET_DHT_GetHandle *get_handle;

/**
 * Count of results found
 */
static unsigned int result_count;

/**
 * Global status value
 */
static int ret;

/**
 * Task scheduled to handle timeout.
 */
static struct GNUNET_SCHEDULER_Task *tt;


/**
 * Task run to clean up on shutdown.
 *
 * @param cls unused
 */
static void
cleanup_task (void *cls)
{
  if (NULL != get_handle)
  {
    GNUNET_DHT_get_stop (get_handle);
    get_handle = NULL;
  }
  if (NULL != dht_handle)
  {
    GNUNET_DHT_disconnect (dht_handle);
    dht_handle = NULL;
  }
  if (NULL != tt)
  {
    GNUNET_SCHEDULER_cancel (tt);
    tt = NULL;
  }
}


/**
 * Task run on timeout. Triggers shutdown.
 *
 * @param cls unused
 */
static void
timeout_task (void *cls)
{
  tt = NULL;
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Iterator called on each result obtained for a DHT
 * operation that expects a reply
 *
 * @param cls closure
 * @param exp when will this value expire
 * @param key key of the result
 * @param get_path peers on reply path (or NULL if not recorded)
 * @param get_path_length number of entries in get_path
 * @param put_path peers on the PUT path (or NULL if not recorded)
 * @param put_path_length number of entries in get_path
 * @param type type of the result
 * @param size number of bytes in data
 * @param data pointer to the result data
 */
static void
get_result_iterator (void *cls, struct GNUNET_TIME_Absolute exp,
                     const struct GNUNET_HashCode * key,
                     const struct GNUNET_PeerIdentity *get_path,
                     unsigned int get_path_length,
                     const struct GNUNET_PeerIdentity *put_path,
                     unsigned int put_path_length,
                     enum GNUNET_BLOCK_Type type,
                     size_t size,
                     const void *data)
{
  FPRINTF (stdout,
	   _("Result %d, type %d:\n%.*s\n"),
	   result_count,
           type,
           (unsigned int) size,
           (char *) data);
  if (verbose)
  {
    FPRINTF (stdout,
             "  GET path: ");
    for (unsigned int i=0;i<get_path_length;i++)
      FPRINTF (stdout,
               "%s%s",
               (0 == i) ? "" : "-",
               GNUNET_i2s (&get_path[i]));
    FPRINTF (stdout,
             "\n  PUT path: ");
    for (unsigned int i=0;i<put_path_length;i++)
      FPRINTF (stdout,
               "%s%s",
               (0 == i) ? "" : "-",
               GNUNET_i2s (&put_path[i]));
    FPRINTF (stdout,
             "\n");
  }
  result_count++;
}


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param c configuration
 */
static void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *c)
{
  struct GNUNET_HashCode key;



  cfg = c;
  if (NULL == query_key)
  {
    FPRINTF (stderr, "%s",  _("Must provide key for DHT GET!\n"));
    ret = 1;
    return;
  }
  if (NULL == (dht_handle = GNUNET_DHT_connect (cfg, 1)))
  {
    FPRINTF (stderr, "%s",  _("Failed to connect to DHT service!\n"));
    ret = 1;
    return;
  }
  if (query_type == GNUNET_BLOCK_TYPE_ANY)      /* Type of data not set */
    query_type = GNUNET_BLOCK_TYPE_TEST;
  GNUNET_CRYPTO_hash (query_key, strlen (query_key), &key);
  if (verbose)
    FPRINTF (stderr, "%s `%s' \n",  _("Issueing DHT GET with key"), GNUNET_h2s_full (&key));
  GNUNET_SCHEDULER_add_shutdown (&cleanup_task, NULL);
  tt = GNUNET_SCHEDULER_add_delayed (timeout_request,
				     &timeout_task, NULL);
  get_handle =
      GNUNET_DHT_get_start (dht_handle, query_type, &key, replication,
                            (demultixplex_everywhere) ? GNUNET_DHT_RO_DEMULTIPLEX_EVERYWHERE : GNUNET_DHT_RO_NONE,
                            NULL, 0, &get_result_iterator, NULL);

}

/**
 * Entry point for gnunet-dht-get
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{

  struct GNUNET_GETOPT_CommandLineOption options[] = {
  
    GNUNET_GETOPT_option_string ('k',
                                 "key",
                                 "KEY",
                                 gettext_noop ("the query key"),
                                 &query_key),
  
    GNUNET_GETOPT_option_uint ('r',
                                   "replication",
                                   "LEVEL",
                                   gettext_noop ("how many parallel requests (replicas) to create"),
                                   &replication),
  
  
    GNUNET_GETOPT_option_uint ('t',
                                   "type",
                                   "TYPE",
                                   gettext_noop ("the type of data to look for"),
                                   &query_type),
  
    GNUNET_GETOPT_option_relative_time ('T',
                                            "timeout",
                                            "TIMEOUT",
                                            gettext_noop ("how long to execute this query before giving up?"),
                                            &timeout_request),
  
    GNUNET_GETOPT_option_flag ('x',
                                  "demultiplex",
                                  gettext_noop ("use DHT's demultiplex everywhere option"),
                                  &demultixplex_everywhere),
  
    GNUNET_GETOPT_option_verbose (&verbose),
    GNUNET_GETOPT_OPTION_END
  };



  if (GNUNET_OK != GNUNET_STRINGS_get_utf8_args (argc, argv, &argc, &argv))
    return 2;
  return (GNUNET_OK ==
          GNUNET_PROGRAM_run (argc, argv, "gnunet-dht-get",
                              gettext_noop
                              ("Issue a GET request to the GNUnet DHT, prints results."),
                              options, &run, NULL)) ? ret : 1;
}

/* end of gnunet-dht-get.c */
