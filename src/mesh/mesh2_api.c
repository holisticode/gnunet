/*
     This file is part of GNUnet.
     (C) 2011 Christian Grothoff (and other contributing authors)
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
 * @file mesh/mesh2_api.c
 * @brief mesh2 api: client implementation of new mesh service
 * @author Bartlomiej Polot
 *
 * STRUCTURE:
 * - DATA STRUCTURES
 * - DECLARATIONS
 * - AUXILIARY FUNCTIONS
 * - RECEIVE HANDLERS
 * - SEND FUNCTIONS
 * - API CALL DEFINITIONS
 *
 * TODO: add regex to reconnect
 */
#include "platform.h"
#include "gnunet_common.h"
#include "gnunet_client_lib.h"
#include "gnunet_util_lib.h"
#include "gnunet_peer_lib.h"
#include "gnunet_mesh2_service.h"
#include "mesh2.h"
#include "mesh2_protocol.h"

#define LOG(kind,...) GNUNET_log_from (kind, "mesh2-api",__VA_ARGS__)

/* FIXME: use dynamic values, expose in API */
#define INITIAL_WINDOW_SIZE 4
#define ACK_THRESHOLD 2
/* FIXME END */

#define DEBUG_ACK GNUNET_YES

/******************************************************************************/
/************************      DATA STRUCTURES     ****************************/
/******************************************************************************/

/**
 * Transmission queue to the service
 */
struct GNUNET_MESH_TransmitHandle
{

    /**
     * Double Linked list
     */
  struct GNUNET_MESH_TransmitHandle *next;

    /**
     * Double Linked list
     */
  struct GNUNET_MESH_TransmitHandle *prev;

    /**
     * Tunnel this message is sent on / for (may be NULL for control messages).
     */
  struct GNUNET_MESH_Tunnel *tunnel;

    /**
     * Callback to obtain the message to transmit, or NULL if we
     * got the message in 'data'.  Notice that messages built
     * by 'notify' need to be encapsulated with information about
     * the 'target'.
     */
  GNUNET_CONNECTION_TransmitReadyNotify notify;

    /**
     * Closure for 'notify'
     */
  void *notify_cls;

    /**
     * How long is this message valid.  Once the timeout has been
     * reached, the message must no longer be sent.  If this
     * is a message with a 'notify' callback set, the 'notify'
     * function should be called with 'buf' NULL and size 0.
     */
  struct GNUNET_TIME_Absolute timeout;

    /**
     * Task triggering a timeout, can be NO_TASK if the timeout is FOREVER.
     */
  GNUNET_SCHEDULER_TaskIdentifier timeout_task;

    /**
     * Size of 'data' -- or the desired size of 'notify' if 'data' is NULL.
     */
  size_t size;
};


/**
 * Opaque handle to the service.
 */
struct GNUNET_MESH_Handle
{

    /**
     * Handle to the server connection, to send messages later
     */
  struct GNUNET_CLIENT_Connection *client;

    /**
     * Set of handlers used for processing incoming messages in the tunnels
     */
  const struct GNUNET_MESH_MessageHandler *message_handlers;

  /**
   * Number of handlers in the handlers array.
   */
  unsigned int n_handlers;

  /**
   * Ports open.
   */
  const uint32_t *ports;

  /**
   * Number of ports.
   */
  unsigned int n_ports;

    /**
     * Double linked list of the tunnels this client is connected to, head.
     */
  struct GNUNET_MESH_Tunnel *tunnels_head;

    /**
     * Double linked list of the tunnels this client is connected to, tail.
     */
  struct GNUNET_MESH_Tunnel *tunnels_tail;

    /**
     * Callback for inbound tunnel creation
     */
  GNUNET_MESH_InboundTunnelNotificationHandler *new_tunnel;

    /**
     * Callback for inbound tunnel disconnection
     */
  GNUNET_MESH_TunnelEndHandler *cleaner;

    /**
     * Handle to cancel pending transmissions in case of disconnection
     */
  struct GNUNET_CLIENT_TransmitHandle *th;

    /**
     * Closure for all the handlers given by the client
     */
  void *cls;

    /**
     * Messages to send to the service, head.
     */
  struct GNUNET_MESH_TransmitHandle *th_head;

    /**
     * Messages to send to the service, tail.
     */
  struct GNUNET_MESH_TransmitHandle *th_tail;

    /**
     * tid of the next tunnel to create (to avoid reusing IDs often)
     */
  MESH_TunnelNumber next_tid;

    /**
     * Have we started the task to receive messages from the service
     * yet? We do this after we send the 'MESH_LOCAL_CONNECT' message.
     */
  int in_receive;

  /**
   * Configuration given by the client, in case of reconnection
   */
  const struct GNUNET_CONFIGURATION_Handle *cfg;

  /**
   * Time to the next reconnect in case one reconnect fails
   */
  struct GNUNET_TIME_Relative reconnect_time;
  
  /**
   * Task for trying to reconnect.
   */
  GNUNET_SCHEDULER_TaskIdentifier reconnect_task;

  /**
   * Monitor callback
   */
  GNUNET_MESH_TunnelsCB tunnels_cb;

  /**
   * Monitor callback closure.
   */
  void *tunnels_cls;

  /**
   * Tunnel callback.
   */
  GNUNET_MESH_TunnelCB tunnel_cb;

  /**
   * Tunnel callback closure.
   */
  void *tunnel_cls;

#if DEBUG_ACK
  unsigned int acks_sent;
  unsigned int acks_recv;
#endif
};


/**
 * Description of a peer
 */
struct GNUNET_MESH_Peer
{
    /**
     * ID of the peer in short form
     */
  GNUNET_PEER_Id id;

  /**
   * Tunnel this peer belongs to
   */
  struct GNUNET_MESH_Tunnel *t;

  /**
   * Flag indicating whether service has informed about its connection
   */
  int connected;

};


/**
 * Opaque handle to a tunnel.
 */
struct GNUNET_MESH_Tunnel
{

    /**
     * DLL next
     */
  struct GNUNET_MESH_Tunnel *next;

    /**
     * DLL prev
     */
  struct GNUNET_MESH_Tunnel *prev;

    /**
     * Handle to the mesh this tunnel belongs to
     */
  struct GNUNET_MESH_Handle *mesh;

    /**
     * Local ID of the tunnel
     */
  MESH_TunnelNumber tid;

    /**
     * Port number.
     */
  uint32_t port;

    /**
     * Other end of the tunnel.
     */
  GNUNET_PEER_Id peer;

  /**
   * Any data the caller wants to put in here
   */
  void *ctx;

    /**
     * Size of packet queued in this tunnel
     */
  unsigned int packet_size;

    /**
     * Is the tunnel allowed to buffer?
     */
  int buffering;

    /**
     * Maximum allowed PID to send (last ACK recevied).
     */
  uint32_t last_ack_recv;

    /**
     * Last PID received from the service.
     */
  uint32_t last_pid_recv;

  /**
   * Last packet ID sent to the service.
   */
  uint32_t last_pid_sent;

  /**
   * Last ACK value sent to the service: how much are we willing to accept?
   */
  uint32_t last_ack_sent;
};


/******************************************************************************/
/***********************         DECLARATIONS         *************************/
/******************************************************************************/

/**
 * Function called to send a message to the service.
 * "buf" will be NULL and "size" zero if the socket was closed for writing in
 * the meantime.
 *
 * @param cls closure, the mesh handle
 * @param size number of bytes available in buf
 * @param buf where the callee should write the connect message
 * @return number of bytes written to buf
 */
static size_t
send_callback (void *cls, size_t size, void *buf);


/******************************************************************************/
/***********************     AUXILIARY FUNCTIONS      *************************/
/******************************************************************************/

/**
 * Check if transmission is a payload packet.
 *
 * @param th Transmission handle.
 *
 * @return GNUNET_YES if it is a payload packet,
 *         GNUNET_NO if it is a mesh management packet.
 */
static int
th_is_payload (struct GNUNET_MESH_TransmitHandle *th)
{
  return (th->notify != NULL) ? GNUNET_YES : GNUNET_NO;
}


/**
 * Check whether there is any message ready in the queue and find the size.
 * 
 * @param h Mesh handle.
 * 
 * @return The size of the first ready message in the queue,
 *         0 if there is none.
 */
static size_t
message_ready_size (struct GNUNET_MESH_Handle *h)
{
  struct GNUNET_MESH_TransmitHandle *th;
  struct GNUNET_MESH_Tunnel *t;

  for (th = h->th_head; NULL != th; th = th->next)
  {
    t = th->tunnel;
    if (GNUNET_NO == th_is_payload (th))
    {
      LOG (GNUNET_ERROR_TYPE_DEBUG, "#  message internal\n");
      return th->size;
    }
    if (GNUNET_YES == GMC_is_pid_bigger(t->last_ack_recv, t->last_pid_sent))
    {
      LOG (GNUNET_ERROR_TYPE_DEBUG, "#  message payload ok (%u <= %u)\n",
           t->last_pid_sent, t->last_ack_recv);
      return th->size;
    }
  }
  return 0;
}


/**
 * Get the tunnel handler for the tunnel specified by id from the given handle
 * @param h Mesh handle
 * @param tid ID of the wanted tunnel
 * @return handle to the required tunnel or NULL if not found
 */
static struct GNUNET_MESH_Tunnel *
retrieve_tunnel (struct GNUNET_MESH_Handle *h, MESH_TunnelNumber tid)
{
  struct GNUNET_MESH_Tunnel *t;

  t = h->tunnels_head;
  while (t != NULL)
  {
    if (t->tid == tid)
      return t;
    t = t->next;
  }
  return NULL;
}


/**
 * Create a new tunnel and insert it in the tunnel list of the mesh handle
 * @param h Mesh handle
 * @param tid desired tid of the tunnel, 0 to assign one automatically
 * @return handle to the created tunnel
 */
static struct GNUNET_MESH_Tunnel *
create_tunnel (struct GNUNET_MESH_Handle *h, MESH_TunnelNumber tid)
{
  struct GNUNET_MESH_Tunnel *t;

  t = GNUNET_malloc (sizeof (struct GNUNET_MESH_Tunnel));
  GNUNET_CONTAINER_DLL_insert (h->tunnels_head, h->tunnels_tail, t);
  t->mesh = h;
  if (0 == tid)
  {
    t->tid = h->next_tid;
    while (NULL != retrieve_tunnel (h, h->next_tid))
    {
      h->next_tid++;
      h->next_tid &= ~GNUNET_MESH_LOCAL_TUNNEL_ID_SERV;
      h->next_tid |= GNUNET_MESH_LOCAL_TUNNEL_ID_CLI;
    }
  }
  else
  {
    t->tid = tid;
  }
  t->last_ack_recv = (uint32_t) -1;
  t->last_pid_recv = (uint32_t) -1;
  t->last_ack_sent = (uint32_t) -1;
  t->last_pid_sent = (uint32_t) -1;
  t->buffering = GNUNET_YES;
  return t;
}


/**
 * Destroy the specified tunnel.
 * - Destroys all peers, calling the disconnect callback on each if needed
 * - Cancels all outgoing traffic for that tunnel, calling respective notifys
 * - Calls cleaner if tunnel was inbound
 * - Frees all memory used
 *
 * @param t Pointer to the tunnel.
 * @param call_cleaner Whether to call the cleaner handler.
 *
 * @return Handle to the required tunnel or NULL if not found.
 */
static void
destroy_tunnel (struct GNUNET_MESH_Tunnel *t, int call_cleaner)
{
  struct GNUNET_MESH_Handle *h;
  struct GNUNET_MESH_TransmitHandle *th;
  struct GNUNET_MESH_TransmitHandle *next;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "destroy_tunnel %X\n", t->tid);

  if (NULL == t)
  {
    GNUNET_break (0);
    return;
  }
  h = t->mesh;

  /* free all peer's ID */
  GNUNET_CONTAINER_DLL_remove (h->tunnels_head, h->tunnels_tail, t);
  GNUNET_PEER_change_rc (t->peer, -1);

  /* signal tunnel destruction */
  if ( (NULL != h->cleaner) && (0 != t->peer) && (GNUNET_YES == call_cleaner) )
    h->cleaner (h->cls, t, t->ctx);

  /* check that clients did not leave messages behind in the queue */
  for (th = h->th_head; NULL != th; th = next)
  {
    next = th->next;
    if (th->tunnel != t)
      continue;
    /* Clients should have aborted their requests already.
     * Management traffic should be ok, as clients can't cancel that */
    GNUNET_break (GNUNET_NO == th_is_payload(th));
    GNUNET_CONTAINER_DLL_remove (h->th_head, h->th_tail, th);

    /* clean up request */
    if (GNUNET_SCHEDULER_NO_TASK != th->timeout_task)
      GNUNET_SCHEDULER_cancel (th->timeout_task);
    GNUNET_free (th);    
  }

  /* if there are no more pending requests with mesh service, cancel active request */
  /* Note: this should be unnecessary... */
  if ((0 == message_ready_size (h)) && (NULL != h->th))
  {
    GNUNET_CLIENT_notify_transmit_ready_cancel (h->th);
    h->th = NULL;
  }

  if (0 != t->peer)
    GNUNET_PEER_change_rc (t->peer, -1);
  GNUNET_free (t);
  return;
}


/**
 * Notify client that the transmission has timed out
 * 
 * @param cls closure
 * @param tc task context
 */
static void
timeout_transmission (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_MESH_TransmitHandle *th = cls;
  struct GNUNET_MESH_Handle *mesh;

  mesh = th->tunnel->mesh;
  GNUNET_CONTAINER_DLL_remove (mesh->th_head, mesh->th_tail, th);
  th->tunnel->packet_size = 0;
  if (GNUNET_YES == th_is_payload (th))
    th->notify (th->notify_cls, 0, NULL);
  GNUNET_free (th);
  if ((0 == message_ready_size (mesh)) && (NULL != mesh->th))
  {
    /* nothing ready to transmit, no point in asking for transmission */
    GNUNET_CLIENT_notify_transmit_ready_cancel (mesh->th);
    mesh->th = NULL;
  }
}


/**
 * Add a transmit handle to the transmission queue and set the
 * timeout if needed.
 *
 * @param h mesh handle with the queue head and tail
 * @param th handle to the packet to be transmitted
 */
static void
add_to_queue (struct GNUNET_MESH_Handle *h,
              struct GNUNET_MESH_TransmitHandle *th)
{
  GNUNET_CONTAINER_DLL_insert_tail (h->th_head, h->th_tail, th);
  if (GNUNET_TIME_UNIT_FOREVER_ABS.abs_value == th->timeout.abs_value)
    return;
  th->timeout_task =
      GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_absolute_get_remaining
                                    (th->timeout), &timeout_transmission, th);
}


/**
 * Auxiliary function to send an already constructed packet to the service.
 * Takes care of creating a new queue element, copying the message and
 * calling the tmt_rdy function if necessary.
 *
 * @param h mesh handle
 * @param msg message to transmit
 * @param tunnel tunnel this send is related to (NULL if N/A)
 */
static void
send_packet (struct GNUNET_MESH_Handle *h,
             const struct GNUNET_MessageHeader *msg,
             struct GNUNET_MESH_Tunnel *tunnel);


/**
 * Send an ack on the tunnel to confirm the processing of a message.
 * 
 * @param h Mesh handle.
 * @param t Tunnel on which to send the ACK.
 */
static void
send_ack (struct GNUNET_MESH_Handle *h, struct GNUNET_MESH_Tunnel *t)
{
  struct GNUNET_MESH_LocalAck msg;
  uint32_t delta;

  delta = t->last_ack_sent - t->last_pid_recv;
  if (delta > ACK_THRESHOLD)
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG,
         "Not sending ACK on tunnel %X: ACK: %u, PID: %u, buffer %u\n",
         t->tid, t->last_ack_sent, t->last_pid_recv, delta);
    return;
  }
  if (GNUNET_YES == t->buffering)
    t->last_ack_sent = t->last_pid_recv + INITIAL_WINDOW_SIZE;
  else
    t->last_ack_sent = t->last_pid_recv + 1;
  LOG (GNUNET_ERROR_TYPE_DEBUG,
       "Sending ACK on tunnel %X: %u\n",
       t->tid, t->last_ack_sent);
  msg.header.type = htons (GNUNET_MESSAGE_TYPE_MESH_LOCAL_ACK);
  msg.header.size = htons (sizeof (msg));
  msg.tunnel_id = htonl (t->tid);
  msg.max_pid = htonl (t->last_ack_sent);

#if DEBUG_ACK
  t->mesh->acks_sent++;
#endif

  send_packet (h, &msg.header, t);
  return;
}



/**
 * Reconnect callback: tries to reconnect again after a failer previous
 * reconnecttion
 * @param cls closure (mesh handle)
 * @param tc task context
 */
static void
reconnect_cbk (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc);


/**
 * Send a connect packet to the service with the applications and types
 * requested by the user.
 *
 * @param h The mesh handle.
 *
 */
static void
send_connect (struct GNUNET_MESH_Handle *h)
{
  size_t size;

  size = sizeof (struct GNUNET_MESH_ClientConnect);
  size += h->n_ports * sizeof (uint32_t);
  {
    char buf[size] GNUNET_ALIGN;
    struct GNUNET_MESH_ClientConnect *msg;
    uint32_t *ports;
    uint16_t i;

    /* build connection packet */
    msg = (struct GNUNET_MESH_ClientConnect *) buf;
    msg->header.type = htons (GNUNET_MESSAGE_TYPE_MESH_LOCAL_CONNECT);
    msg->header.size = htons (size);
    ports = (uint32_t *) &msg[1];
    for (i = 0; i < h->n_ports; i++)
    {
      ports[i] = htonl (h->ports[i]);
      LOG (GNUNET_ERROR_TYPE_DEBUG, " port %u\n",
           h->ports[i]);
    }
    LOG (GNUNET_ERROR_TYPE_DEBUG,
         "Sending %lu bytes long message with %u ports\n",
         ntohs (msg->header.size), h->n_ports);
    send_packet (h, &msg->header, NULL);
  }
}


/**
 * Reconnect to the service, retransmit all infomation to try to restore the
 * original state.
 *
 * @param h handle to the mesh
 *
 * @return GNUNET_YES in case of sucess, GNUNET_NO otherwise (service down...)
 */
static int
do_reconnect (struct GNUNET_MESH_Handle *h)
{
  struct GNUNET_MESH_Tunnel *t;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "*****************************\n");
  LOG (GNUNET_ERROR_TYPE_DEBUG, "*******   RECONNECT   *******\n");
  LOG (GNUNET_ERROR_TYPE_DEBUG, "*****************************\n");
  LOG (GNUNET_ERROR_TYPE_DEBUG, "******** on %p *******\n", h);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "*****************************\n");

  /* disconnect */
  if (NULL != h->th)
  {
    GNUNET_CLIENT_notify_transmit_ready_cancel (h->th);
    h->th = NULL;
  }
  if (NULL != h->client)
  {
    GNUNET_CLIENT_disconnect (h->client);
  }

  /* connect again */
  h->client = GNUNET_CLIENT_connect ("mesh", h->cfg);
  if (h->client == NULL)
  {
    h->reconnect_task = GNUNET_SCHEDULER_add_delayed (h->reconnect_time,
                                                      &reconnect_cbk, h);
    h->reconnect_time =
        GNUNET_TIME_relative_min (GNUNET_TIME_UNIT_SECONDS,
                                  GNUNET_TIME_relative_multiply
                                  (h->reconnect_time, 2));
    LOG (GNUNET_ERROR_TYPE_DEBUG, 
	 "Next retry in %s\n",
         GNUNET_STRINGS_relative_time_to_string (h->reconnect_time,
						 GNUNET_NO));
    GNUNET_break (0);
    return GNUNET_NO;
  }
  else
  {
    h->reconnect_time = GNUNET_TIME_UNIT_MILLISECONDS;
  }
  send_connect (h);
  /* Rebuild all tunnels */
  for (t = h->tunnels_head; NULL != t; t = t->next)
  {
    struct GNUNET_MESH_TunnelMessage tmsg;

    if (t->tid >= GNUNET_MESH_LOCAL_TUNNEL_ID_SERV)
    {
      /* Tunnel was created by service (incoming tunnel) */
      /* TODO: Notify service of missing tunnel, to request
       * creator to recreate path (find a path to him via DHT?)
       */
      continue;
    }
    t->last_ack_sent = (uint32_t) -1;
    t->last_pid_sent = (uint32_t) -1;
    t->last_ack_recv = (uint32_t) -1;
    t->last_pid_recv = (uint32_t) -1;
    tmsg.header.type = htons (GNUNET_MESSAGE_TYPE_MESH_LOCAL_TUNNEL_CREATE);
    tmsg.header.size = htons (sizeof (struct GNUNET_MESH_TunnelMessage));
    tmsg.tunnel_id = htonl (t->tid);
    GNUNET_PEER_resolve (t->peer, &tmsg.peer);
    send_packet (h, &tmsg.header, t);

    if (GNUNET_NO == t->buffering)
      GNUNET_MESH_tunnel_buffer (t, GNUNET_NO);
  }
  return GNUNET_YES;
}

/**
 * Reconnect callback: tries to reconnect again after a failer previous
 * reconnecttion
 * @param cls closure (mesh handle)
 * @param tc task context
 */
static void
reconnect_cbk (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_MESH_Handle *h = cls;

  h->reconnect_task = GNUNET_SCHEDULER_NO_TASK;
  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
    return;
  do_reconnect (h);
}


/**
 * Reconnect to the service, retransmit all infomation to try to restore the
 * original state.
 *
 * @param h handle to the mesh
 *
 * @return GNUNET_YES in case of sucess, GNUNET_NO otherwise (service down...)
 */
static void
reconnect (struct GNUNET_MESH_Handle *h)
{
  LOG (GNUNET_ERROR_TYPE_DEBUG, "Requested RECONNECT\n");
  h->in_receive = GNUNET_NO;
  if (GNUNET_SCHEDULER_NO_TASK == h->reconnect_task)
    h->reconnect_task = GNUNET_SCHEDULER_add_delayed (h->reconnect_time,
                                                      &reconnect_cbk, h);
}


/******************************************************************************/
/***********************      RECEIVE HANDLERS     ****************************/
/******************************************************************************/

/**
 * Process the new tunnel notification and add it to the tunnels in the handle
 *
 * @param h     The mesh handle
 * @param msg   A message with the details of the new incoming tunnel
 */
static void
process_tunnel_created (struct GNUNET_MESH_Handle *h,
                        const struct GNUNET_MESH_TunnelNotification *msg)
{
  struct GNUNET_MESH_Tunnel *t;
  MESH_TunnelNumber tid;

  tid = ntohl (msg->tunnel_id);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "Creating incoming tunnel %X\n", tid);
  if (tid < GNUNET_MESH_LOCAL_TUNNEL_ID_SERV)
  {
    GNUNET_break (0);
    return;
  }
  if (NULL != h->new_tunnel)
  {
    t = create_tunnel (h, tid);
    t->peer = GNUNET_PEER_intern (&msg->peer);
    GNUNET_PEER_change_rc (t->peer, 1);
    t->mesh = h;
    t->tid = tid;
    t->port = ntohl (msg->port);
    if (0 != (msg->opt & MESH_TUNNEL_OPT_NOBUFFER))
      t->buffering = GNUNET_NO;
    else
      t->buffering = GNUNET_YES;
    LOG (GNUNET_ERROR_TYPE_DEBUG, "  created tunnel %p\n", t);
    t->ctx = h->new_tunnel (h->cls, t, &msg->peer, t->port);
    LOG (GNUNET_ERROR_TYPE_DEBUG, "User notified\n");
  }
  else
  {
    struct GNUNET_MESH_TunnelMessage d_msg;

    LOG (GNUNET_ERROR_TYPE_DEBUG, "No handler for incoming tunnels\n");

    d_msg.header.type = htons (GNUNET_MESSAGE_TYPE_MESH_LOCAL_TUNNEL_DESTROY);
    d_msg.header.size = htons (sizeof (struct GNUNET_MESH_TunnelMessage));
    d_msg.tunnel_id = msg->tunnel_id;

    send_packet (h, &d_msg.header, NULL);
  }
  return;
}


/**
 * Process the tunnel destroy notification and free associated resources
 *
 * @param h     The mesh handle
 * @param msg   A message with the details of the tunnel being destroyed
 */
static void
process_tunnel_destroy (struct GNUNET_MESH_Handle *h,
                        const struct GNUNET_MESH_TunnelMessage *msg)
{
  struct GNUNET_MESH_Tunnel *t;
  MESH_TunnelNumber tid;

  tid = ntohl (msg->tunnel_id);
  t = retrieve_tunnel (h, tid);

  if (NULL == t)
  {
    return;
  }
  LOG (GNUNET_ERROR_TYPE_DEBUG, "tunnel %X destroyed\n", t->tid);
  destroy_tunnel (t, GNUNET_YES);
  return;
}


/**
 * Process the incoming data packets
 *
 * @param h         The mesh handle
 * @param message   A message encapsulating the data
 * 
 * @return GNUNET_YES if everything went fine
 *         GNUNET_NO if client closed connection (h no longer valid)
 */
static int
process_incoming_data (struct GNUNET_MESH_Handle *h,
                       const struct GNUNET_MessageHeader *message)
{
  const struct GNUNET_MessageHeader *payload;
  const struct GNUNET_MESH_MessageHandler *handler;
  const struct GNUNET_PeerIdentity *peer;
  struct GNUNET_PeerIdentity id;
  struct GNUNET_MESH_Unicast *ucast;
  struct GNUNET_MESH_ToOrigin *to_orig;
  struct GNUNET_MESH_Tunnel *t;
  unsigned int i;
  uint32_t pid;
  uint16_t type;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "Got a data message!\n");
  type = ntohs (message->type);
  switch (type)
  {
  case GNUNET_MESSAGE_TYPE_MESH_UNICAST:
    ucast = (struct GNUNET_MESH_Unicast *) message;

    t = retrieve_tunnel (h, ntohl (ucast->tid));
    payload = (struct GNUNET_MessageHeader *) &ucast[1];
    peer = &ucast->oid;
    pid = ntohl (ucast->pid);
    LOG (GNUNET_ERROR_TYPE_DEBUG, "  ucast on tunnel %s [%X]\n",
         GNUNET_i2s (peer), ntohl (ucast->tid));
    break;
  case GNUNET_MESSAGE_TYPE_MESH_TO_ORIGIN:
    to_orig = (struct GNUNET_MESH_ToOrigin *) message;
    t = retrieve_tunnel (h, ntohl (to_orig->tid));
    payload = (struct GNUNET_MessageHeader *) &to_orig[1];
    GNUNET_PEER_resolve (t->peer, &id);
    peer = &id;
    pid = ntohl (to_orig->pid);
    LOG (GNUNET_ERROR_TYPE_DEBUG, "  torig on tunnel %s [%X]\n",
         GNUNET_i2s (peer), ntohl (to_orig->tid));
    break;
  default:
    GNUNET_break (0);
    return GNUNET_YES;
  }
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  pid %u\n", pid);
  if (NULL == t)
  {
    /* Tunnel was ignored/destroyed, probably service didn't get it yet */
    LOG (GNUNET_ERROR_TYPE_DEBUG, "  ignored!\n");
    return GNUNET_YES;
  }
  if (GNUNET_YES ==
      GMC_is_pid_bigger(pid, t->last_ack_sent))
  {
    GNUNET_break (0);
    LOG (GNUNET_ERROR_TYPE_WARNING,
         "  unauthorized message! (%u, ACK %u)\n",
         pid, t->last_ack_sent);
    // FIXME fc what now? accept? reject?
    return GNUNET_YES;
  }
  t->last_pid_recv = pid;
  type = ntohs (payload->type);
  send_ack (h, t);
  for (i = 0; i < h->n_handlers; i++)
  {
    handler = &h->message_handlers[i];
    if (handler->type == type)
    {
      if (GNUNET_OK !=
          handler->callback (h->cls, t, &t->ctx, peer, payload))
      {
        LOG (GNUNET_ERROR_TYPE_DEBUG, "callback caused disconnection\n");
        GNUNET_MESH_disconnect (h);
        return GNUNET_NO;
      }
      else
      {
        LOG (GNUNET_ERROR_TYPE_DEBUG,
             "callback completed successfully\n");
      }
    }
  }
  return GNUNET_YES;
}


/**
 * Process a local ACK message, enabling the client to send
 * more data to the service.
 * 
 * @param h Mesh handle.
 * @param message Message itself.
 */
static void
process_ack (struct GNUNET_MESH_Handle *h,
             const struct GNUNET_MessageHeader *message)
{
  struct GNUNET_MESH_LocalAck *msg;
  struct GNUNET_MESH_Tunnel *t;
  uint32_t ack;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "Got an ACK!\n");
  h->acks_recv++;
  msg = (struct GNUNET_MESH_LocalAck *) message;

  t = retrieve_tunnel (h, ntohl (msg->tunnel_id));

  if (NULL == t)
  {
    LOG (GNUNET_ERROR_TYPE_WARNING,
         "ACK on unknown tunnel %X\n",
         ntohl (msg->tunnel_id));
    return;
  }
  ack = ntohl (msg->max_pid);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  on tunnel %X, ack %u!\n", t->tid, ack);
  if (GNUNET_YES == GMC_is_pid_bigger(ack, t->last_ack_recv))
    t->last_ack_recv = ack;
  else
    return;
  if (NULL == h->th && 0 < t->packet_size)
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG, "  tmt rdy was NULL, requesting!\n", t->tid, ack);
    h->th =
        GNUNET_CLIENT_notify_transmit_ready (h->client, t->packet_size,
                                             GNUNET_TIME_UNIT_FOREVER_REL,
                                             GNUNET_YES, &send_callback, h);
  }
}


/**
 * Process a local reply about info on all tunnels, pass info to the user.
 *
 * @param h Mesh handle.
 * @param message Message itself.
 */
static void
process_get_tunnels (struct GNUNET_MESH_Handle *h,
                     const struct GNUNET_MessageHeader *message)
{
  struct GNUNET_MESH_LocalMonitor *msg;

  GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "Get Tunnels messasge received\n");

  if (NULL == h->tunnels_cb)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "  ignored\n");
    return;
  }

  msg = (struct GNUNET_MESH_LocalMonitor *) message;
  if (ntohs (message->size) !=
      (sizeof (struct GNUNET_MESH_LocalMonitor) +
       sizeof (struct GNUNET_PeerIdentity)))
  {
    GNUNET_break_op (0);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Get tunnels message: size %hu - expected %u\n",
                ntohs (message->size),
                sizeof (struct GNUNET_MESH_LocalMonitor));
    return;
  }
  h->tunnels_cb (h->tunnels_cls,
                 ntohl (msg->tunnel_id),
                 &msg->owner,
                 &msg->destination);
}



/**
 * Process a local monitor_tunnel reply, pass info to the user.
 *
 * @param h Mesh handle.
 * @param message Message itself.
 */
static void
process_show_tunnel (struct GNUNET_MESH_Handle *h,
                     const struct GNUNET_MessageHeader *message)
{
  struct GNUNET_MESH_LocalMonitor *msg;
  size_t esize;

  GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "Show Tunnel messasge received\n");

  if (NULL == h->tunnel_cb)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "  ignored\n");
    return;
  }

  /* Verify message sanity */
  msg = (struct GNUNET_MESH_LocalMonitor *) message;
  esize = sizeof (struct GNUNET_MESH_LocalMonitor);
  if (ntohs (message->size) != esize)
  {
    GNUNET_break_op (0);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Show tunnel message: size %hu - expected %u\n",
                ntohs (message->size),
                esize);

    h->tunnel_cb (h->tunnel_cls, NULL, NULL);
    h->tunnel_cb = NULL;
    h->tunnel_cls = NULL;

    return;
  }

  h->tunnel_cb (h->tunnel_cls,
                &msg->destination,
                &msg->owner);
}


/**
 * Function to process all messages received from the service
 *
 * @param cls closure
 * @param msg message received, NULL on timeout or fatal error
 */
static void
msg_received (void *cls, const struct GNUNET_MessageHeader *msg)
{
  struct GNUNET_MESH_Handle *h = cls;

  if (msg == NULL)
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG, 
	 "Mesh service disconnected, reconnecting\n", h);
    reconnect (h);
    return;
  }
  LOG (GNUNET_ERROR_TYPE_DEBUG, "\n",
       GNUNET_MESH_DEBUG_M2S (ntohs (msg->type)));
  LOG (GNUNET_ERROR_TYPE_DEBUG, "Received a message: %s\n",
       GNUNET_MESH_DEBUG_M2S (ntohs (msg->type)));
  switch (ntohs (msg->type))
  {
    /* Notify of a new incoming tunnel */
  case GNUNET_MESSAGE_TYPE_MESH_LOCAL_TUNNEL_CREATE:
    process_tunnel_created (h, (struct GNUNET_MESH_TunnelNotification *) msg);
    break;
    /* Notify of a tunnel disconnection */
  case GNUNET_MESSAGE_TYPE_MESH_LOCAL_TUNNEL_DESTROY:
    process_tunnel_destroy (h, (struct GNUNET_MESH_TunnelMessage *) msg);
    break;
    /* Notify of a new data packet in the tunnel */
  case GNUNET_MESSAGE_TYPE_MESH_UNICAST:
  case GNUNET_MESSAGE_TYPE_MESH_MULTICAST:
  case GNUNET_MESSAGE_TYPE_MESH_TO_ORIGIN:
    if (GNUNET_NO == process_incoming_data (h, msg))
      return;
    break;
  case GNUNET_MESSAGE_TYPE_MESH_LOCAL_ACK:
    process_ack (h, msg);
    break;
  case GNUNET_MESSAGE_TYPE_MESH_LOCAL_INFO_TUNNELS:
        process_get_tunnels (h, msg);
    break;
  case GNUNET_MESSAGE_TYPE_MESH_LOCAL_INFO_TUNNEL:
        process_show_tunnel (h, msg);
    break;
  default:
    /* We shouldn't get any other packages, log and ignore */
    LOG (GNUNET_ERROR_TYPE_WARNING,
         "unsolicited message form service (type %s)\n",
         GNUNET_MESH_DEBUG_M2S (ntohs (msg->type)));
  }
  LOG (GNUNET_ERROR_TYPE_DEBUG, "message processed\n");
  if (GNUNET_YES == h->in_receive)
  {
    GNUNET_CLIENT_receive (h->client, &msg_received, h,
                           GNUNET_TIME_UNIT_FOREVER_REL);
  }
  else
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG,
         "in receive off, not calling CLIENT_receive\n");
  }
}


/******************************************************************************/
/************************       SEND FUNCTIONS     ****************************/
/******************************************************************************/

/**
 * Function called to send a message to the service.
 * "buf" will be NULL and "size" zero if the socket was closed for writing in
 * the meantime.
 *
 * @param cls closure, the mesh handle
 * @param size number of bytes available in buf
 * @param buf where the callee should write the connect message
 * @return number of bytes written to buf
 */
static size_t
send_callback (void *cls, size_t size, void *buf)
{
  struct GNUNET_MESH_Handle *h = cls;
  struct GNUNET_MESH_TransmitHandle *th;
  struct GNUNET_MESH_TransmitHandle *next;
  struct GNUNET_MESH_Tunnel *t;
  char *cbuf = buf;
  size_t tsize;
  size_t psize;
  size_t nsize;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "\n");
  LOG (GNUNET_ERROR_TYPE_DEBUG, "# Send packet() Buffer %u\n", size);
  if ((0 == size) || (NULL == buf))
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG, "# Received NULL send callback on %p\n", h);
    reconnect (h);
    h->th = NULL;
    return 0;
  }
  tsize = 0;
  next = h->th_head;
  nsize = message_ready_size (h);
  while ((NULL != (th = next)) && (0 < nsize) && (size >= nsize))
  {
    t = th->tunnel;
    if (GNUNET_YES == th_is_payload (th))
    {
      LOG (GNUNET_ERROR_TYPE_DEBUG, "#  payload\n");
      if (GNUNET_NO == GMC_is_pid_bigger(t->last_ack_recv, t->last_pid_sent))
      {
        /* This tunnel is not ready to transmit yet, try next message */
        next = th->next;
        continue;
      }
      t->packet_size = 0;
      if (t->tid >= GNUNET_MESH_LOCAL_TUNNEL_ID_SERV)
      {
        /* traffic to origin */
        struct GNUNET_MESH_ToOrigin to;
        struct GNUNET_MessageHeader *mh;

        GNUNET_assert (size >= th->size);
        mh = (struct GNUNET_MessageHeader *) &cbuf[sizeof (to)];
        psize = th->notify (th->notify_cls, size - sizeof (to), mh);
        LOG (GNUNET_ERROR_TYPE_DEBUG, "#  to origin, type %s\n",
             GNUNET_MESH_DEBUG_M2S (ntohs (mh->type)));
        if (psize > 0)
        {
          psize += sizeof (to);
          GNUNET_assert (size >= psize);
          to.header.size = htons (psize);
          to.header.type = htons (GNUNET_MESSAGE_TYPE_MESH_TO_ORIGIN);
          to.tid = htonl (t->tid);
          to.pid = htonl (t->last_pid_sent + 1);
          to.ttl = 0;
          memset (&to.oid, 0, sizeof (struct GNUNET_PeerIdentity));
          memcpy (cbuf, &to, sizeof (to));
        }
      }
      else
      {
        /* unicast */
        struct GNUNET_MESH_Unicast uc;
        struct GNUNET_MessageHeader *mh;

        GNUNET_assert (size >= th->size);
        mh = (struct GNUNET_MessageHeader *) &cbuf[sizeof (uc)];
        psize = th->notify (th->notify_cls, size - sizeof (uc), mh);
        LOG (GNUNET_ERROR_TYPE_DEBUG, "#  unicast, type %s\n",
             GNUNET_MESH_DEBUG_M2S (ntohs (mh->type)));
        if (psize > 0)
        {
          psize += sizeof (uc);
          GNUNET_assert (size >= psize);
          uc.header.size = htons (psize);
          uc.header.type = htons (GNUNET_MESSAGE_TYPE_MESH_UNICAST);
          uc.tid = htonl (t->tid);
          uc.pid = htonl (t->last_pid_sent + 1);
          uc.ttl = 0;
          memset (&uc.oid, 0, sizeof (struct GNUNET_PeerIdentity));
          memcpy (cbuf, &uc, sizeof (uc));
        }
      }
      t->last_pid_sent++;
    }
    else
    {
      struct GNUNET_MessageHeader *mh = (struct GNUNET_MessageHeader *) &th[1];

      LOG (GNUNET_ERROR_TYPE_DEBUG, "#  mesh traffic, type %s\n",
           GNUNET_MESH_DEBUG_M2S (ntohs (mh->type)));
      memcpy (cbuf, &th[1], th->size);
      psize = th->size;
    }
    if (th->timeout_task != GNUNET_SCHEDULER_NO_TASK)
      GNUNET_SCHEDULER_cancel (th->timeout_task);
    GNUNET_CONTAINER_DLL_remove (h->th_head, h->th_tail, th);
    GNUNET_free (th);
    next = h->th_head;
    nsize = message_ready_size (h);
    cbuf += psize;
    size -= psize;
    tsize += psize;
  }
  LOG (GNUNET_ERROR_TYPE_DEBUG, "#  total size: %u\n", tsize);
  h->th = NULL;
  size = message_ready_size (h);
  if (0 != size)
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG, "#  next size: %u\n", size);
    h->th =
        GNUNET_CLIENT_notify_transmit_ready (h->client, size,
                                             GNUNET_TIME_UNIT_FOREVER_REL,
                                             GNUNET_YES, &send_callback, h);
  }
  else
  {
    if (NULL != h->th_head)
      LOG (GNUNET_ERROR_TYPE_DEBUG, "#  can't transmit any more\n");
    else
      LOG (GNUNET_ERROR_TYPE_DEBUG, "#  nothing left to transmit\n");
  }
  if (GNUNET_NO == h->in_receive)
  {
    LOG (GNUNET_ERROR_TYPE_DEBUG, "# start receiving from service\n");
    h->in_receive = GNUNET_YES;
    GNUNET_CLIENT_receive (h->client, &msg_received, h,
                           GNUNET_TIME_UNIT_FOREVER_REL);
  }
  LOG (GNUNET_ERROR_TYPE_DEBUG, "# Send packet() END\n");
  return tsize;
}


/**
 * Auxiliary function to send an already constructed packet to the service.
 * Takes care of creating a new queue element, copying the message and
 * calling the tmt_rdy function if necessary.
 * 
 * @param h mesh handle
 * @param msg message to transmit
 * @param tunnel tunnel this send is related to (NULL if N/A)
 */
static void
send_packet (struct GNUNET_MESH_Handle *h,
             const struct GNUNET_MessageHeader *msg,
             struct GNUNET_MESH_Tunnel *tunnel)
{
  struct GNUNET_MESH_TransmitHandle *th;
  size_t msize;

  LOG (GNUNET_ERROR_TYPE_DEBUG, " Sending message to service: %s\n",
       GNUNET_MESH_DEBUG_M2S(ntohs(msg->type)));
  msize = ntohs (msg->size);
  th = GNUNET_malloc (sizeof (struct GNUNET_MESH_TransmitHandle) + msize);
  th->timeout = GNUNET_TIME_UNIT_FOREVER_ABS;
  th->size = msize;
  th->tunnel = tunnel;
  memcpy (&th[1], msg, msize);
  add_to_queue (h, th);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  queued\n");
  if (NULL != h->th)
    return;
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  calling ntfy tmt rdy for %u bytes\n", msize);
  h->th =
      GNUNET_CLIENT_notify_transmit_ready (h->client, msize,
                                           GNUNET_TIME_UNIT_FOREVER_REL,
                                           GNUNET_YES, &send_callback, h);
}


/******************************************************************************/
/**********************      API CALL DEFINITIONS     *************************/
/******************************************************************************/

struct GNUNET_MESH_Handle *
GNUNET_MESH_connect (const struct GNUNET_CONFIGURATION_Handle *cfg, void *cls,
                     GNUNET_MESH_InboundTunnelNotificationHandler new_tunnel,
                     GNUNET_MESH_TunnelEndHandler cleaner,
                     const struct GNUNET_MESH_MessageHandler *handlers,
                     const uint32_t *ports)
{
  struct GNUNET_MESH_Handle *h;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "GNUNET_MESH_connect()\n");
  h = GNUNET_malloc (sizeof (struct GNUNET_MESH_Handle));
  LOG (GNUNET_ERROR_TYPE_DEBUG, " addr %p\n", h);
  h->cfg = cfg;
  h->new_tunnel = new_tunnel;
  h->cleaner = cleaner;
  h->client = GNUNET_CLIENT_connect ("mesh", cfg);
  if (h->client == NULL)
  {
    GNUNET_break (0);
    GNUNET_free (h);
    return NULL;
  }
  h->cls = cls;
  h->message_handlers = handlers;
  h->ports = ports;
  h->next_tid = GNUNET_MESH_LOCAL_TUNNEL_ID_CLI;
  h->reconnect_time = GNUNET_TIME_UNIT_MILLISECONDS;
  h->reconnect_task = GNUNET_SCHEDULER_NO_TASK;

  /* count handlers */
  for (h->n_handlers = 0;
       handlers && handlers[h->n_handlers].type;
       h->n_handlers++) ;
  for (h->n_ports = 0;
       ports && ports[h->n_ports];
       h->n_ports++) ;
  send_connect (h);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "GNUNET_MESH_connect() END\n");
  return h;
}


/**
 * Disconnect from the mesh service. All tunnels will be destroyed. All tunnel
 * disconnect callbacks will be called on any still connected peers, notifying
 * about their disconnection. The registered inbound tunnel cleaner will be
 * called should any inbound tunnels still exist.
 *
 * @param handle connection to mesh to disconnect
 */
void
GNUNET_MESH_disconnect (struct GNUNET_MESH_Handle *handle)
{
  struct GNUNET_MESH_Tunnel *t;
  struct GNUNET_MESH_Tunnel *aux;
  struct GNUNET_MESH_TransmitHandle *th;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "MESH DISCONNECT\n");

#if DEBUG_ACK
  LOG (GNUNET_ERROR_TYPE_INFO, "Sent %d ACKs\n", handle->acks_sent);
  LOG (GNUNET_ERROR_TYPE_INFO, "Recv %d ACKs\n\n", handle->acks_recv);
#endif

  t = handle->tunnels_head;
  while (NULL != t)
  {
    aux = t->next;
    if (t->tid < GNUNET_MESH_LOCAL_TUNNEL_ID_SERV)
    {
      GNUNET_break (0);
      LOG (GNUNET_ERROR_TYPE_DEBUG, "tunnel %X not destroyed\n", t->tid);
    }
    destroy_tunnel (t, GNUNET_YES);
    t = aux;
  }
  while ( (th = handle->th_head) != NULL)
  {
    struct GNUNET_MessageHeader *msg;

    /* Make sure it is an allowed packet (everything else should have been
     * already canceled).
     */
    GNUNET_break (GNUNET_NO == th_is_payload (th));
    msg = (struct GNUNET_MessageHeader *) &th[1];
    switch (ntohs(msg->type))
    {
      case GNUNET_MESSAGE_TYPE_MESH_LOCAL_CONNECT:
      case GNUNET_MESSAGE_TYPE_MESH_LOCAL_TUNNEL_DESTROY:
      case GNUNET_MESSAGE_TYPE_MESH_LOCAL_INFO_TUNNELS:
      case GNUNET_MESSAGE_TYPE_MESH_LOCAL_INFO_TUNNEL:
        break;
      default:
        GNUNET_break (0);
        LOG (GNUNET_ERROR_TYPE_ERROR, "unexpected msg %u\n",
             ntohs(msg->type));
    }

    GNUNET_CONTAINER_DLL_remove (handle->th_head, handle->th_tail, th);
    GNUNET_free (th);
  }

  if (NULL != handle->th)
  {
    GNUNET_CLIENT_notify_transmit_ready_cancel (handle->th);
    handle->th = NULL;
  }
  if (NULL != handle->client)
  {
    GNUNET_CLIENT_disconnect (handle->client);
    handle->client = NULL;
  }
  if (GNUNET_SCHEDULER_NO_TASK != handle->reconnect_task)
  {
    GNUNET_SCHEDULER_cancel(handle->reconnect_task);
    handle->reconnect_task = GNUNET_SCHEDULER_NO_TASK;
  }
  GNUNET_free (handle);
}


struct GNUNET_MESH_Tunnel *
GNUNET_MESH_tunnel_create (struct GNUNET_MESH_Handle *h, 
                           void *tunnel_ctx,
                           const struct GNUNET_PeerIdentity *peer,
                           uint32_t port)
{
  struct GNUNET_MESH_Tunnel *t;
  struct GNUNET_MESH_TunnelMessage msg;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "Creating new tunnel\n");
  t = create_tunnel (h, 0);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  at %p\n", t);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "  number %X\n", t->tid);
  t->ctx = tunnel_ctx;
  msg.header.type = htons (GNUNET_MESSAGE_TYPE_MESH_LOCAL_TUNNEL_CREATE);
  msg.header.size = htons (sizeof (struct GNUNET_MESH_TunnelMessage));
  msg.tunnel_id = htonl (t->tid);
  msg.port = htonl (port);
  msg.peer = *peer;
  send_packet (h, &msg.header, t);
  return t;
}


/**
 * Destroy an existing tunnel. The existing callback for the tunnel will NOT
 * be called.
 *
 * @param tunnel tunnel handle
 */
void
GNUNET_MESH_tunnel_destroy (struct GNUNET_MESH_Tunnel *tunnel)
{
  struct GNUNET_MESH_Handle *h;
  struct GNUNET_MESH_TunnelMessage msg;
  struct GNUNET_MESH_TransmitHandle *th;

  LOG (GNUNET_ERROR_TYPE_DEBUG, "Destroying tunnel\n");
  h = tunnel->mesh;

  msg.header.type = htons (GNUNET_MESSAGE_TYPE_MESH_LOCAL_TUNNEL_DESTROY);
  msg.header.size = htons (sizeof (struct GNUNET_MESH_TunnelMessage));
  msg.tunnel_id = htonl (tunnel->tid);
  th = h->th_head;
  while (th != NULL)
  {
    struct GNUNET_MESH_TransmitHandle *aux;
    if (th->tunnel == tunnel)
    {
      aux = th->next;
      /* FIXME call the handler? */
      if (GNUNET_YES == th_is_payload (th))
        th->notify (th->notify_cls, 0, NULL);
      GNUNET_CONTAINER_DLL_remove (h->th_head, h->th_tail, th);
      GNUNET_free (th);
      th = aux;
    }
    else
      th = th->next;
  }

  destroy_tunnel (tunnel, GNUNET_NO);
  send_packet (h, &msg.header, NULL);
}


/**
 * Turn on/off the buffering status of the tunnel.
 * 
 * @param tunnel Tunnel affected.
 * @param buffer GNUNET_YES to turn buffering on (default),
 *               GNUNET_NO otherwise.
 */
void
GNUNET_MESH_tunnel_buffer (struct GNUNET_MESH_Tunnel *tunnel, int buffer)
{
  struct GNUNET_MESH_TunnelMessage msg;
  struct GNUNET_MESH_Handle *h;

  h = tunnel->mesh;
  tunnel->buffering = buffer;

  if (GNUNET_YES == buffer)
    msg.header.type = htons (GNUNET_MESSAGE_TYPE_MESH_LOCAL_TUNNEL_BUFFER);
  else
    msg.header.type = htons (GNUNET_MESSAGE_TYPE_MESH_LOCAL_TUNNEL_NOBUFFER);
  msg.header.size = htons (sizeof (struct GNUNET_MESH_TunnelMessage));
  msg.tunnel_id = htonl (tunnel->tid);

  send_packet (h, &msg.header, NULL);
}


struct GNUNET_MESH_TransmitHandle *
GNUNET_MESH_notify_transmit_ready (struct GNUNET_MESH_Tunnel *tunnel, int cork,
                                   struct GNUNET_TIME_Relative maxdelay,
                                   size_t notify_size,
                                   GNUNET_CONNECTION_TransmitReadyNotify notify,
                                   void *notify_cls)
{
  struct GNUNET_MESH_TransmitHandle *th;
  size_t overhead;

  GNUNET_assert (NULL != tunnel);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "MESH NOTIFY TRANSMIT READY\n");
  LOG (GNUNET_ERROR_TYPE_DEBUG, "    on tunnel %X\n", tunnel->tid);
  if (tunnel->tid >= GNUNET_MESH_LOCAL_TUNNEL_ID_SERV)
    LOG (GNUNET_ERROR_TYPE_DEBUG, "    to origin\n");
  else
    LOG (GNUNET_ERROR_TYPE_DEBUG, "    to destination\n");
  LOG (GNUNET_ERROR_TYPE_DEBUG, "    payload size %u\n", notify_size);
  GNUNET_assert (NULL != notify);
  GNUNET_assert (0 == tunnel->packet_size); // Only one data packet allowed
  th = GNUNET_malloc (sizeof (struct GNUNET_MESH_TransmitHandle));
  th->tunnel = tunnel;
  th->timeout = GNUNET_TIME_relative_to_absolute (maxdelay);
  if (tunnel->tid >= GNUNET_MESH_LOCAL_TUNNEL_ID_SERV)
    overhead = sizeof (struct GNUNET_MESH_ToOrigin);
  else
    overhead = sizeof (struct GNUNET_MESH_Unicast);
  tunnel->packet_size = th->size = notify_size + overhead;
  LOG (GNUNET_ERROR_TYPE_DEBUG, "    total size %u\n", th->size);
  th->notify = notify;
  th->notify_cls = notify_cls;
  add_to_queue (tunnel->mesh, th);
  if (NULL != tunnel->mesh->th)
    return th;
  if (!GMC_is_pid_bigger (tunnel->last_ack_recv, tunnel->last_pid_sent))
    return th;
  LOG (GNUNET_ERROR_TYPE_DEBUG, "    call client notify tmt rdy\n");
  tunnel->mesh->th =
      GNUNET_CLIENT_notify_transmit_ready (tunnel->mesh->client, th->size,
                                           GNUNET_TIME_UNIT_FOREVER_REL,
                                           GNUNET_YES, &send_callback,
                                           tunnel->mesh);
  LOG (GNUNET_ERROR_TYPE_DEBUG, "MESH NOTIFY TRANSMIT READY END\n");
  return th;
}


/**
 * Cancel the specified transmission-ready notification.
 *
 * @param th handle that was returned by "notify_transmit_ready".
 */
void
GNUNET_MESH_notify_transmit_ready_cancel (struct GNUNET_MESH_TransmitHandle *th)
{
  struct GNUNET_MESH_Handle *mesh;

  th->tunnel->packet_size = 0;
  mesh = th->tunnel->mesh;
  if (th->timeout_task != GNUNET_SCHEDULER_NO_TASK)
    GNUNET_SCHEDULER_cancel (th->timeout_task);
  GNUNET_CONTAINER_DLL_remove (mesh->th_head, mesh->th_tail, th);
  GNUNET_free (th);
  if ((0 == message_ready_size (mesh)) && (NULL != mesh->th))
  {
    /* queue empty, no point in asking for transmission */
    GNUNET_CLIENT_notify_transmit_ready_cancel (mesh->th);
    mesh->th = NULL;
  }
}


/**
 * Request information about the running mesh peer.
 * The callback will be called for every tunnel known to the service,
 * listing all active peers that blong to the tunnel.
 *
 * If called again on the same handle, it will overwrite the previous
 * callback and cls. To retrieve the cls, monitor_cancel must be
 * called first.
 *
 * WARNING: unstable API, likely to change in the future!
 *
 * @param h Handle to the mesh peer.
 * @param callback Function to call with the requested data.
 * @param callback_cls Closure for @c callback.
 */
void
GNUNET_MESH_get_tunnels (struct GNUNET_MESH_Handle *h,
                         GNUNET_MESH_TunnelsCB callback,
                         void *callback_cls)
{
  struct GNUNET_MessageHeader msg;

  msg.size = htons (sizeof (msg));
  msg.type = htons (GNUNET_MESSAGE_TYPE_MESH_LOCAL_INFO_TUNNELS);
  send_packet (h, &msg, NULL);
  h->tunnels_cb = callback;
  h->tunnels_cls = callback_cls;

  return;
}


/**
 * Cancel a monitor request. The monitor callback will not be called.
 *
 * @param h Mesh handle.
 *
 * @return Closure given to GNUNET_MESH_monitor, if any.
 */
void *
GNUNET_MESH_get_tunnels_cancel (struct GNUNET_MESH_Handle *h)
{
  void *cls;

  cls = h->tunnels_cls;
  h->tunnels_cb = NULL;
  h->tunnels_cls = NULL;
  return cls;
}


/**
 * Request information about a specific tunnel of the running mesh peer.
 *
 * WARNING: unstable API, likely to change in the future!
 * FIXME Add destination option.
 *
 * @param h Handle to the mesh peer.
 * @param initiator ID of the owner of the tunnel.
 * @param tunnel_number Tunnel number.
 * @param callback Function to call with the requested data.
 * @param callback_cls Closure for @c callback.
 */
void
GNUNET_MESH_show_tunnel (struct GNUNET_MESH_Handle *h,
                         struct GNUNET_PeerIdentity *initiator,
                         unsigned int tunnel_number,
                         GNUNET_MESH_TunnelCB callback,
                         void *callback_cls)
{
  struct GNUNET_MESH_LocalMonitor msg;

  msg.header.size = htons (sizeof (msg));
  msg.header.type = htons (GNUNET_MESSAGE_TYPE_MESH_LOCAL_INFO_TUNNEL);
  msg.owner = *initiator;
  msg.tunnel_id = htonl (tunnel_number);
  msg.reserved = 0;
  send_packet (h, &msg.header, NULL);
  h->tunnel_cb = callback;
  h->tunnel_cls = callback_cls;

  return;
}


/**
 * Transition API for tunnel ctx management
 */
void
GNUNET_MESH_tunnel_set_data (struct GNUNET_MESH_Tunnel *tunnel, void *data)
{
  tunnel->ctx = data;
}

/**
 * Transition API for tunnel ctx management
 */
void *
GNUNET_MESH_tunnel_get_data (struct GNUNET_MESH_Tunnel *tunnel)
{
  return tunnel->ctx;
}


