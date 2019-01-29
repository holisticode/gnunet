/*
     This file is part of GNUnet
     Copyright (C) 2010-2014, 2018, 2019 GNUnet e.V.

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
 * @file transport/gnunet-communicator-udp.c
 * @brief Transport plugin using UDP.
 * @author Christian Grothoff
 *
 * TODO:
 * - support DNS names in BINDTO option (#5528)
 * - support NAT connection reversal method (#5529)
 * - support other UDP-specific NAT traversal methods 
 */
#include "platform.h"
#include "gnunet_util_lib.h"
#include "gnunet_protocols.h"
#include "gnunet_signatures.h"
#include "gnunet_constants.h"
#include "gnunet_nt_lib.h"
#include "gnunet_nat_service.h"
#include "gnunet_statistics_service.h"
#include "gnunet_transport_communication_service.h"

/**
 * How many messages do we keep at most in the queue to the
 * transport service before we start to drop (default,
 * can be changed via the configuration file).
 * Should be _below_ the level of the communicator API, as
 * otherwise we may read messages just to have them dropped
 * by the communicator API.
 */
#define DEFAULT_MAX_QUEUE_LENGTH 8

/**
 * How often do we rekey based on time (at least)
 */ 
#define REKEY_TIME_INTERVAL GNUNET_TIME_UNIT_DAYS

/**
 * How long do we wait until we must have received the initial KX?
 */ 
#define PROTO_QUEUE_TIMEOUT GNUNET_TIME_UNIT_MINUTES

/**
 * How often do we rekey based on number of bytes transmitted?
 * (additionally randomized).
 */ 
#define REKEY_MAX_BYTES (1024LLU * 1024 * 1024 * 4LLU)

/**
 * Address prefix used by the communicator.
 */
#define COMMUNICATOR_ADDRESS_PREFIX "udp"

/**
 * Configuration section used by the communicator.
 */
#define COMMUNICATOR_CONFIG_SECTION "communicator-udp"

GNUNET_NETWORK_STRUCT_BEGIN


/**
 * Signature we use to verify that the ephemeral key was really chosen by
 * the specified sender.  If possible, the receiver should respond with
 * a `struct UDPAck` (possibly via backchannel).
 */
struct UdpHandshakeSignature
{
  /**
   * Purpose must be #GNUNET_SIGNATURE_COMMUNICATOR_UDP_HANDSHAKE
   */
  struct GNUNET_CRYPTO_EccSignaturePurpose purpose;

  /**
   * Identity of the inititor of the UDP connection (UDP client).
   */ 
  struct GNUNET_PeerIdentity sender;

  /**
   * Presumed identity of the target of the UDP connection (UDP server)
   */ 
  struct GNUNET_PeerIdentity receiver;

  /**
   * Ephemeral key used by the @e sender.
   */ 
  struct GNUNET_CRYPTO_EcdhePublicKey ephemeral;

  /**
   * Monotonic time of @e sender, to possibly help detect replay attacks
   * (if receiver persists times by sender).
   */ 
  struct GNUNET_TIME_AbsoluteNBO monotonic_time;
};


/**
 * "Plaintext" header at beginning of KX message. Followed
 * by encrypted `struct UDPConfirmation`.
 */
struct InitialKX
{

  /**
   * Ephemeral key for KX.
   */ 
  struct GNUNET_CRYPT_EddsaPublicKey ephemeral;

  /**
   * HMAC for the following encrypted message, using GCM.  HMAC uses
   * key derived from the handshake with sequence number zero.
   */ 
  uint8_t gcm_tag[128/8];

};


/**
 * Encrypted continuation of UDP initial handshake, followed
 * by message header with payload.
 */
struct UDPConfirmation
{
  /**
   * Sender's identity
   */
  struct GNUNET_PeerIdentity sender;

  /**
   * Sender's signature of type #GNUNET_SIGNATURE_COMMUNICATOR_UDP_HANDSHAKE
   */
  struct GNUNET_CRYPTO_EddsaSignature sender_sig;

  /**
   * Monotonic time of @e sender, to possibly help detect replay attacks
   * (if receiver persists times by sender).
   */ 
  struct GNUNET_TIME_AbsoluteNBO monotonic_time;

  /* followed by messages */

  /* padding may follow actual messages */
};


/**
 * UDP key acknowledgement.  May be sent via backchannel. Allows the
 * sender to use `struct UDPBox` with the acknowledge key henceforth.
 */ 
struct UDPAck
{

  /**
   * Type is #GNUNET_MESSAGE_TYPE_COMMUNICATOR_UDP_ACK.
   */ 
  struct GNUNET_MessageHeader header;

  /**
   * Sequence acknowledgement limit. Specifies current maximum sequence
   * number supported by receiver.
   */ 
  uint32_t sequence_max GNUNET_PACKED;
  
  /**
   * CMAC of the base key being acknowledged.
   */ 
  struct GNUNET_HashCode cmac;

};


/**
 * UDP message box.  Always sent encrypted, only allowed after
 * the receiver sent a `struct UDPAck` for the base key!
 */ 
struct UDPBox
{

  /**
   * Key and IV identification code. KDF applied to an acknowledged
   * base key and a sequence number.  Sequence numbers must be used
   * monotonically increasing up to the maximum specified in 
   * `struct UDPAck`. Without further `struct UDPAck`s, the sender
   * must fall back to sending handshakes!
   */
  struct GNUNET_ShortHashCode kid;

  /**
   * 128-bit authentication tag for the following encrypted message,
   * from GCM.  MAC starts at the @e body_start that follows and
   * extends until the end of the UDP payload.  If the @e hmac is
   * wrong, the receiver should check if the message might be a
   * `struct UdpHandshakeSignature`.
   */ 
  uint8_t gcm_tag[128/8];

  
};


GNUNET_NETWORK_STRUCT_END

/**
 * Shared secret we generated for a particular sender or receiver.
 */
struct SharedSecret;


/**
 * Pre-generated "kid" code (key and IV identification code) to
 * quickly derive master key for a `struct UDPBox`.
 */
struct KeyCacheEntry
{

  /**
   * Kept in a DLL.
   */
  struct KeyCacheEntry *next;
  
  /**
   * Kept in a DLL.
   */
  struct KeyCacheEntry *prev;

  /**
   * Key and IV identification code. KDF applied to an acknowledged
   * base key and a sequence number.  Sequence numbers must be used
   * monotonically increasing up to the maximum specified in 
   * `struct UDPAck`. Without further `struct UDPAck`s, the sender
   * must fall back to sending handshakes!
   */
  struct GNUNET_ShortHashCode kid;

  /**
   * Corresponding shared secret.
   */
  struct SharedSecret *ss;

  /**
   * Sequence number used to derive this entry from master key.
   */ 
  uint32_t sequence_number;
};


/**
 * Information we track per sender address we have recently been
 * in contact with (decryption from sender).
 */
struct SenderAddress;

/**
 * Information we track per receiving address we have recently been
 * in contact with (encryption to receiver).
 */
struct ReceiverAddress;

/**
 * Shared secret we generated for a particular sender or receiver.
 */
struct SharedSecret
{
  /**
   * Kept in a DLL.
   */
  struct SharedSecret *next;

  /**
   * Kept in a DLL.
   */
  struct SharedSecret *prev;

  /**
   * Kept in a DLL, sorted by sequence number. Only if we are decrypting.
   */
  struct KeyCacheEntry *kce_head;
  
  /**
   * Kept in a DLL, sorted by sequence number. Only if we are decrypting.
   */
  struct KeyCacheEntry *kce_tail;

  /**
   * Sender we use this shared secret with, or NULL.
   */
  struct SenderAddress *sender;

  /**
   * Receiver we use this shared secret with, or NULL.
   */
  struct ReceiverAddress *receiver;
  
  /**
   * Master shared secret.
   */ 
  struct GNUNET_HashCode master;

  /**
   * CMAC is used to identify @e master in ACKs.
   */ 
  struct GNUNET_HashCode cmac;

  /**
   * Up to which sequence number did we use this @e master already?
   * (for sending or receiving)
   */ 
  uint32_t sequence_used;

  /**
   * Up to which sequence number did the other peer allow us to use
   * this key, or up to which number did we allow the other peer to
   * use this key?
   */
  uint32_t sequence_allowed;
};


/**
 * Information we track per sender address we have recently been
 * in contact with (we decrypt messages from the sender).
 */
struct SenderAddress
{

  /**
   * To whom are we talking to.
   */
  struct GNUNET_PeerIdentity target;  

  /**
   * Entry in sender expiration heap.
   */ 
  struct GNUNET_CONTAINER_HeapNode *hn;

  /**
   * Shared secrets we used with @e target, first used is head.
   */
  struct SharedSecret *ss_head;

  /**
   * Shared secrets we used with @e target, last used is tail.
   */
  struct SharedSecret *ss_tail;
  
  /**
   * Address of the other peer.
   */
  struct sockaddr *address;
  
  /**
   * Length of the address.
   */
  socklen_t address_len;

  /**
   * Timeout for this sender.
   */
  struct GNUNET_TIME_Absolute timeout;

  /**
   * Length of the DLL at @a ss_head.
   */ 
  unsigned int num_secrets;
  
  /**
   * Which network type does this queue use?
   */
  enum GNUNET_NetworkType nt;
  
};


/**
 * Information we track per receiving address we have recently been
 * in contact with (encryption to receiver).
 */
struct ReceiverAddress
{

  /**
   * To whom are we talking to.
   */
  struct GNUNET_PeerIdentity target;  
  
  /**
   * Shared secrets we received from @e target, first used is head.
   */
  struct SharedSecret *ss_head;

  /**
   * Shared secrets we received with @e target, last used is tail.
   */
  struct SharedSecret *ss_tail;

  /**
   * Address of the other peer.
   */
  struct sockaddr *address;
  
  /**
   * Length of the address.
   */
  socklen_t address_len;

  /**
   * Message queue we are providing for the #ch.
   */
  struct GNUNET_MQ_Handle *mq;

  /**
   * handle for this queue with the #ch.
   */
  struct GNUNET_TRANSPORT_QueueHandle *qh;

  /**
   * Timeout for this receiver address.
   */
  struct GNUNET_TIME_Absolute timeout;

  /**
   * Length of the DLL at @a ss_head.
   */ 
  unsigned int num_secrets;
  
  /**
   * Which network type does this queue use?
   */
  enum GNUNET_NetworkType nt;
  
};


/**
 * Cache of pre-generated key IDs.
 */
static struct GNUNET_CONTINER_MultiShortMap *key_cache;

/**
 * ID of read task
 */
static struct GNUNET_SCHEDULER_Task *read_task;

/**
 * ID of timeout task
 */
static struct GNUNET_SCHEDULER_Task *timeout_task;

/**
 * For logging statistics.
 */
static struct GNUNET_STATISTICS_Handle *stats;

/**
 * Our environment.
 */
static struct GNUNET_TRANSPORT_CommunicatorHandle *ch;

/**
 * Receivers (map from peer identity to `struct ReceiverAddress`)
 */
static struct GNUNET_CONTAINER_MultiPeerMap *receivers;

/**
 * Senders (map from peer identity to `struct SenderAddress`)
 */
static struct GNUNET_CONTAINER_MultiPeerMap *senders;

/**
 * Expiration heap for senders (contains `struct SenderAddress`)
 */
static struct GNUNET_CONTAINER_Heap *senders_heap;

/**
 * Expiration heap for receivers (contains `struct ReceiverAddress`)
 */
static struct GNUNET_CONTAINER_Heap *receivers_heap;

/**
 * Our socket.
 */
static struct GNUNET_NETWORK_Handle *udp_sock;

/**
 * Our public key.
 */
static struct GNUNET_PeerIdentity my_identity;

/**
 * Our private key.
 */
static struct GNUNET_CRYPTO_EddsaPrivateKey *my_private_key;

/**
 * Our configuration.
 */
static const struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Network scanner to determine network types.
 */
static struct GNUNET_NT_InterfaceScanner *is;

/**
 * Connection to NAT service.
 */
static struct GNUNET_NAT_Handle *nat;


/**
 * Functions with this signature are called whenever we need
 * to close a receiving state due to timeout.
 *
 * @param receiver entity to close down
 */
static void
receiver_destroy (struct ReceiverAddress *receiver)
{
  struct GNUNET_MQ_Handle *mq;
  
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
	      "Disconnecting receiver for peer `%s'\n",
	      GNUNET_i2s (&receiver->target));
  if (NULL != (mq = receiver->mq))
  {
    receiver->mq = NULL;
    GNUNET_MQ_destroy (mq);
  }
  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CONTAINER_multipeermap_remove (receivers,
						       &receiver->target,
						       receiver));
  // FIXME: remove from receiver_heap
  GNUNET_STATISTICS_set (stats,
			 "# receivers active",
			 GNUNET_CONTAINER_multipeermap_size (receivers),
			 GNUNET_NO);
  GNUNET_free (receiver->address);
  GNUNET_free (receiver);
}


/**
 * Free memory used by key cache entry.
 *
 * @param kce the key cache entry
 */ 
static void
kce_destroy (struct KeyCacheEntry *kce)
{
  struct SharedSecret *ss = kce->ss;

  GNUNET_CONTAINER_DLL_remove (ss->kce_head,
			       ss->kce_tail,
			       kce);
  GNUNET_assert (GNUNET_YES ==
		 GNUNET_CONTAINER_multishortmap_remove (key_cache,
							&kce->kid,
							kce));
  GNUNET_free (kce);
}


/**
 * Compute @a kid.
 *
 * @param msec master secret for HMAC calculation
 * @param serial number for the @a smac calculation
 * @param kid[out] where to write the key ID
 */
static void
get_kid (const struct GNUNET_HashCode *msec,
	 uint32_t serial,
	 struct GNUNET_ShortHashCode *kid)
{
  uint32_t sid = htonl (serial);

  GNUNET_CRYPTO_hkdf (kid,
		      sizeof (*kid),
		      GCRY_MD_SHA512,
		      GCRY_MD_SHA256,
		      &sid,
		      sizeof (sid),
		      msec,
		      sizeof (*msec),
		      "UDP-KID",
		      strlen ("UDP-KID"),
		      NULL, 0);
}


/**
 * Setup key cache entry for sequence number @a seq and shared secret @a ss.
 *
 * @param ss shared secret
 * @param seq sequence number for the key cache entry
 */
static void
kce_generate (struct SharedSecret *ss,
	      uint32_t seq)
{
  struct KeyCacheEntry *kce;

  GNUNET_assert (0 < seq);
  kce = GNUNET_new (struct KeyCacheEntry);
  kce->ss = ss;
  kce->sequence_number = seq;
  get_kid (&ss->master,
	   seq,
	   &kce->kid);
  GNUNET_CONTAINER_DLL_insert (ss->kce_head,
			       ss->kce_tail,
			       kce);
  (void) GNUNET_CONTAINER_multishortmap_put (key_cache,
					     &kce->kid,
					     kce,
					     GNUNET_CONTAINER_MULTIHASHMAPOPTION_MULTIPLE);
  GNUNET_STATISTICS_set (stats,
			 "# KIDs active",
			 GNUNET_CONTAINER_multipeermap_size (key_cache),
			 GNUNET_NO);
}


/**
 * Destroy @a ss and associated key cache entries.
 *
 * @param ss shared secret to destroy
 */
static void
secret_destroy (struct SharedSecret *ss)
{
  struct SenderAddress *sender;
  struct ReceiverAddress *receiver;
  struct KeyCacheEntry *kce;

  if (NULL != (sender = ss->sender))
  {
    GNUNET_CONTAINER_DLL_remove (sender->ss_head,
				 sender->ss_tail,
				 ss);
    sender->num_secrets--;
  }
  if (NULL != (receiver = ss->receiver))
  {
    GNUNET_CONTAINER_DLL_remove (receiver->ss_head,
				 receiver->ss_tail,
				 ss);
    receiver->num_secrets--;
  }
  while (NULL != (kce = ss->kce_head))
    kce_destroy (kce);
  GNUNET_STATISTICS_update (stats,
			    "# Secrets active",
			    -1,
			    GNUNET_NO);
  GNUNET_STATISTICS_set (stats,
			 "# KIDs active",
			 GNUNET_CONTAINER_multipeermap_size (key_cache),
			 GNUNET_NO);
  GNUNET_free (ss);
}


/**
 * Functions with this signature are called whenever we need
 * to close a sender's state due to timeout.
 *
 * @param sender entity to close down
 */
static void
sender_destroy (struct SenderAddress *sender)
{
  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CONTAINER_multipeermap_remove (senders,
						       &sender->target,
						       sender));
  // FIXME: remove from sender_heap
  GNUNET_STATISTICS_set (stats,
			 "# senders active",
			 GNUNET_CONTAINER_multipeermap_size (senders),
			 GNUNET_NO);
  GNUNET_free (sender->address);
  GNUNET_free (sender);
}


/**
 * Compute @a smac over @a buf.
 *
 * @param msec master secret for HMAC calculation
 * @param serial number for the @a smac calculation
 * @param buf buffer to MAC
 * @param buf_size number of bytes in @a buf
 * @param smac[out] where to write the HMAC
 */
static void
get_hmac (const struct GNUNET_HashCode *msec,
	  uint32_t serial,
	  const void *buf,
	  size_t buf_size,
	  struct GNUNET_ShortHashCode *smac)
{
  uint32_t sid = htonl (serial);

  GNUNET_CRYPTO_hkdf (smac,
		      sizeof (*smac),
		      GCRY_MD_SHA512,
		      GCRY_MD_SHA256,
		      &sid,
		      sizeof (sid),
		      msec,
		      sizeof (*msec),
		      "UDP-HMAC",
		      strlen ("UDP-HMAC"),
		      NULL, 0);
}


/**
 * Compute @a key and @a iv.
 *
 * @param msec master secret for calculation
 * @param serial number for the @a smac calculation
 * @param key[out] where to write the decrption key
 * @param iv[out] where to write the IV
 */
static void
get_iv_key (const struct GNUNET_HashCode *msec,
	    uint32_t serial,
	    char key[256/8],
	    char iv[96/8])
{
  uint32_t sid = htonl (serial);
  char res[sizeof(key) + sizeof (iv)];

  GNUNET_CRYPTO_hkdf (res,
		      sizeof (res),
		      GCRY_MD_SHA512,
		      GCRY_MD_SHA256,
		      &sid,
		      sizeof (sid),
		      msec,
		      sizeof (*msec),
		      "UDP-IV-KEY",
		      strlen ("UDP-IV-KEY"),
		      NULL, 0);
  memcpy (key,
	  sid,
	  sizeof (key));
  memcpy (iv,
	  &sid[sizeof(key)],
	  sizeof (iv));
}


/**
 * Increment sender timeout due to activity.
 *
 * @param sender address for which the timeout should be rescheduled
 */
static void
reschedule_sender_timeout (struct SenderAddress *sender)
{
  sender->timeout
    = GNUNET_TIME_relative_to_absolute (GNUNET_CONSTANTS_IDLE_CONNECTION_TIMEOUT);
  // FIXME: update heap!
}


/**
 * Increment receiver timeout due to activity.
 *
 * @param receiver address for which the timeout should be rescheduled
 */
static void
reschedule_receiver_timeout (struct ReceiverAddress *receiver)
{
  receiver->timeout
    = GNUNET_TIME_relative_to_absolute (GNUNET_CONSTANTS_IDLE_CONNECTION_TIMEOUT);
  // FIXME: update heap!
}


/**
 * Calcualte cmac from master in @a ss.
 *
 * @param ss[in,out] data structure to complete
 */
static void
calculate_cmac (struct SharedSecret *ss)
{
  GNUNET_CRYPTO_hkdf (&ss->cmac,
		      sizeof (ss->cmac),
		      GCRY_MD_SHA512,
		      GCRY_MD_SHA256,
		      "CMAC",
		      strlen ("CMAC"),
		      &ss->master,
		      sizeof (ss->master),
		      "UDP-CMAC",
		      strlen ("UDP-CMAC"),
		      NULL, 0);
}


/**
 * We received @a plaintext_len bytes of @a plaintext from a @a sender.
 * Pass it on to CORE.  
 *
 * @param queue the queue that received the plaintext
 * @param plaintext the plaintext that was received
 * @param plaintext_len number of bytes of plaintext received
 */ 
static void
pass_plaintext_to_core (struct SenderAddress *sender,
			const void *plaintext,
			size_t plaintext_len)
{
  const struct GNUNET_MessageHeader *hdr = plaintext;

  while (ntohs (hdr->size) < plaintext_len)
  {
    GNUNET_STATISTICS_update (stats,
			      "# bytes given to core",
			      ntohs (hdr->size),
			      GNUNET_NO);
    (void) GNUNET_TRANSPORT_communicator_receive (ch,
						  &queue->target,
						  hdr,
						  NULL /* no flow control possible */,
						  NULL);
    /* move on to next message, if any */
    plaintext_len -= ntohs (hdr->size);
    if (plaintext_len < sizeof (*hdr))
      break;
    hdr = plaintext + ntohs (hdr->size);
  }
  GNUNET_STATISTICS_update (stats,
			    "# bytes padding discarded",
			    plaintext_len,
			    GNUNET_NO);
}


/**
 * Setup @a cipher based on shared secret @a msec and
 * serial number @a serial.
 *
 * @param msec master shared secret
 * @param serial serial number of cipher to set up
 * @param cipher[out] cipher to initialize
 */
static void
setup_cipher (const struct GNUNET_HashCode *msec,
	      uint32_t serial,
	      gcry_cipher_hd_t *cipher)
{
  char key[256/8];
  char iv[96/8];

  gcry_cipher_open (cipher,
		    GCRY_CIPHER_AES256 /* low level: go for speed */,
		    GCRY_CIPHER_MODE_GCM,
		    0 /* flags */);
  get_iv_key (msec,
	      serial,
	      key,
	      iv);
  gcry_cipher_setkey (*cipher,
		      key,
		      sizeof (key));
  gcry_cipher_setiv (*cipher,
		     iv,
		     sizeof (iv));
}


/**
 * Try to decrypt @a buf using shared secret @a ss and key/iv 
 * derived using @a serial.
 *
 * @param ss shared secret
 * @param tag GCM authentication tag
 * @param serial serial number to use
 * @param in_buf input buffer to decrypt
 * @param in_buf_size number of bytes in @a in_buf and available in @a out_buf
 * @param out_buf where to write the result
 * @return #GNUNET_OK on success
 */
static int
try_decrypt (const struct SharedSecret *ss,
	     char tag[128/8],
	     uint32_t serial,
	     const char *in_buf,
	     size_t in_buf_size,
	     char *out_buf)
{
  gcry_cipher_hd_t cipher;

  setup_cipher (&ss->master,
		serial,
		&cipher);
  GNUNET_assert (0 ==
		 gcry_cipher_decrypt (cipher,
				      in_buf,
				      in_buf_size,
				      out_buf,
				      in_buf_size));
  if (0 !=
      gcry_cipher_checktag (cipher,
			    tag,
			    sizeof (tag)))
  {
    gcry_cipher_close (cipher);
    GNUNET_STATISTICS_update (stats,
			      "# AEAD authentication failures",
			      1,
			      GNUNET_NO);
    return GNUNET_SYSERR;
  }
  gcry_cipher_close (cipher);
  return GNUNET_OK;
}


/**
 * Setup shared secret for decryption.
 *
 * @param ephemeral ephemeral key we received from the other peer
 * @return new shared secret
 */
static struct SharedSecret *
setup_shared_secret_dec (const struct GNUNET_CRYPTO_EcdhePublicKey *ephemeral)
{
  struct SharedSecret *ss;

  ss = GNUNET_new (struct SharedSecret);
  GNUNET_CRYPTO_eddsa_ecdh (my_private_key,
			    ephemeral,
			    &ss->master);
  return ss;
}


/**
 * Setup shared secret for encryption.
 *
 * @param ephemeral ephemeral key we are sending to the other peer
 * @param receiver[in,out] queue to initialize encryption key for
 * @return new shared secret
 */
static struct SharedSecret *
setup_shared_secret_enc (const struct GNUNET_CRYPTO_EcdhePrivateKey *ephemeral,
			 struct ReceiverAddress *receiver)
{
  struct SharedSecret *ss;

  ss = GNUNET_new (struct SharedSecret);
  GNUNET_CRYPTO_ecdh_eddsa (ephemeral,
			    &receiver->target.public_key,
			    &ss->master);
  calculcate_cmac (ss);
  ss->receiver = receiver;
  GNUNET_CONTAINER_DLL_insert (receiver->ss_head,
			       receiver->ss_tail,
			       ss);
  receiver->num_secrets++;
  GNUNET_STATISTICS_update (stats,
			    "# Secrets active",
			    1,
			    GNUNET_NO);
  return ss;
}
		

/**
 * Test if we have received a valid message in plaintext.
 * If so, handle it.
 *
 * @param sender peer to process inbound plaintext for
 * @param buf buffer we received
 * @param buf_size number of bytes in @a buf
 */ 
static void
try_handle_plaintext (struct SenderAddress *sender,
		      const void *buf,
		      size_t buf_size)
{
  const struct GNUNET_MessageHeader *hdr
    = (const struct GNUNET_MessageHeader *) queue->pread_buf;
  const struct UDPAck *ack
    = (const struct UDPAck *) queue->pread_buf;
  uint16_t type;

  if (sizeof (*hdr) > buf_size)
    return; /* not even a header */
  if (ntohs (hdr->size) > buf_size)
    return 0; /* not even a header */
  type = ntohs (hdr->type);
  switch (type)
  {
  case GNUNET_MESSAGE_TYPE_COMMUNICATOR_UDP_ACK:
    /* lookup master secret by 'cmac', then update sequence_max */
    for (struct SharedSecret *ss = sender->ss_head;
	 NULL != ss;
	 ss = ss->next)
    {
      if (0 == memcmp (&ack->cmac,
		       &ss->cmac,
		       sizeof (ss->cmac)))
      {
	ss->sequence_allowed = GNUNET_MAX (ss->sequence_allowed,
					   ntohl (ack->sequence_max));
	/* move ss to head to avoid discarding it anytime soon! */
	GNUNET_CONTAINER_DLL_remove (sender->ss_head,
				     sender->ss_tail,
				     ss);
	GNUNET_CONTAINER_DLL_insert (sender->ss_head,
				     sender->ss_tail,
				     ss);
	break;
      }
    }
    /* There could be more messages after the ACK, handle those as well */
    buf += ntohs (hdr->size);
    buf_size -= ntohs (hdr->size);
    pass_plaintext_to_core (sender,
			    buf,
			    buf_size);
    break;
  default:
    pass_plaintext_to_core (sender,
			    buf,
			    buf_size);
  }
}


/**
 * We established a shared secret with a sender. We should try to send
 * the sender an `struct UDPAck` at the next opportunity to allow the
 * sender to use @a ss longer (assuming we did not yet already
 * recently).
 */
static void
consider_ss_ack (struct SharedSecret *ss)
{
  GNUNET_assert (NULL != ss->sender);
  for (uint32_t i=1;i<0 /* FIXME: ack-based! */;i++)
    kce_generate (ss,
		  i);
  // FIXME: consider generating ACK and more KCEs for ss!
}


/**
 * We received a @a box with matching @a kce.  Decrypt and process it.
 *
 * @param box the data we received
 * @param box_len number of bytes in @a box
 * @param kce key index to decrypt @a box
 */ 
static void
decrypt_box (const struct UDPBox *box,
	     size_t box_len,
	     struct KeyCacheEntry *kce)
{
  struct SharedSecret *ss = kce->ss;
  gcry_cipher_hd_t cipher;
  char out_buf[box_len - sizeof (*box)];

  GNUNET_assert (NULL != ss->sender);
  if (GNUNET_OK !=
      try_decrypt (ss,
		   box->gcm_tag,
		   kce->sequence_number,
		   box_len - sizeof (*box),
		   out_buf,
		   sizeof (out_buf)))
  {
    GNUNET_STATISTICS_update (stats,
			      "# Decryption failures with valid KCE",
			      1,
			      GNUNET_NO);
    kce_destroy (kce);
    return;
  }
  kce_destroy (kce);
  GNUNET_STATISTICS_update (stats,
			    "# bytes decrypted with BOX",
			    sizeof (out_buf),
			    GNUNET_NO);
  try_handle_plaintext (ss->sender,
			out_buf,
			sizeof (out_buf));
  consider_ss_ack (ss);
}


/**
 * Socket read task. 
 *
 * @param cls NULL
 */
static void
sock_read (void *cls)
{
  struct sockaddr_storage sa;
  socklen_t salen = sizeof (sa);
  char buf[UINT16_MAX];
  ssize_t rcvd;

  (void) cls;
  read_task
      = GNUNET_SCHEDULER_add_read_net (GNUNET_TIME_UNIT_FOREVER_REL,
				       udp_sock,
				       &sock_read,
				       NULL);
  rcvd = GNUNET_NETWORK_socket_recvfrom (udp_sock,
					 buf,
					 sizeof (buf),
					 (struct sockaddr *) &sa,
					 &salen);
  if (-1 == rcvd)
  {
    GNUNET_log_strerror (GNUNET_ERROR_TYPE_DEBUG,
			 "recv");
    return;
  }
  /* first, see if it is a UDPBox */
  if (rcvd > sizeof (struct UDPBox))
  {
    const struct UDPBox *box;
    struct KeyCacheEntry *kce;

    box = (const struct UDPBox *) buf;
    kce = GNUNET_CONTAINER_multihashmap_get (key_cache,
					     &box->kid);
    if (NULL != kce)
    {
      decrypt_box (box,
		   (size_t) rcvd,
		   kce);
      return;
    }
  }
  /* next, test if it is a KX */
  if (rcvd < sizeof (struct UDPConfirmation) + sizeof (struct InitialKX))
  {
    GNUNET_STATISTICS_update (stats,
			      "# messages dropped (no kid, too small for KX)",
			      1,
			      GNUNET_NO);
    return;
  }

  {
    const struct InitialKX *kx;
    struct SharedSecret *ss;
    char pbuf[rcvd - sizeof (struct InitialKX)];
    const struct UDPConfirmation *uc;
    struct SenderAddress *sender;

    kx = (const struct InitialKX *) buf;
    ss = setup_shared_secret_dec (&kx->ephemral);
    if (GNUNET_OK !=
	try_decrypt (ss,
		     0,
		     kx->gcm_tag,
		     &buf[sizeof (*kx)],
		     (const struct GNUNET_CRYPTO_EcdhePublicKey *) buf,
		     pbuf))
    {
      GNUNET_free (ss);
      GNUNET_STATISTICS_update (stats,
				"# messages dropped (no kid, AEAD decryption failed)",
				1,
				GNUNET_NO);
      return;
    }
    uc = (const struct UDPConfirmation *) pbuf;
    if (GNUNET_OK !=
	verify_confirmation (&kx->ephemeral,
			     uc))
    {
      GNUNET_break_op (0);
      GNUNET_free (ss);
      GNUNET_STATISTICS_update (stats,
				"# messages dropped (sender signature invalid)",
				1,
				GNUNET_NO);
      return;
    }
    calculcate_cmac (ss);
    sender = setup_sender (&uc->sender,
			   (const struct sockaddr *) &sa,
			   salen);
    ss->sender = sender;
    GNUNET_CONTAINER_DLL_insert (sender->ss_head,
				 sender->ss_tail,
				 ss);
    sender->num_secrets++;
    GNUNET_STATISTICS_update (stats,
			      "# Secrets active",
			      1,
			      GNUNET_NO);
    GNUNET_STATISTICS_update (stats,
			      "# bytes decrypted without BOX",
			      sizeof (pbuf) - sizeof (*uc),
			      GNUNET_NO);
    try_handle_plaintext (sender,
			  &uc[1],
			  sizeof (pbuf) - sizeof (*uc));
    consider_ss_ack (ss);
    if (sender->num_secrets > MAX_SECRETS)
      secret_destroy (sender->ss_tail);
  }
}


/**
 * Convert UDP bind specification to a `struct sockaddr *`
 *
 * @param bindto bind specification to convert
 * @param[out] sock_len set to the length of the address
 * @return converted bindto specification
 */
static struct sockaddr *
udp_address_to_sockaddr (const char *bindto,
			 socklen_t *sock_len)
{
  struct sockaddr *in;
  unsigned int port;
  char dummy[2];
  char *colon;
  char *cp;
  
  if (1 == SSCANF (bindto,
		   "%u%1s",
		   &port,
		   dummy))
  {
    /* interpreting value as just a PORT number */
    if (port > UINT16_MAX)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		  "BINDTO specification `%s' invalid: value too large for port\n",
		  bindto);
      return NULL;
    }
    if (GNUNET_YES ==
	GNUNET_CONFIGURATION_get_value_yesno (cfg,
					      COMMUNICATOR_CONFIG_SECTION,
					      "DISABLE_V6"))
    {
      struct sockaddr_in *i4;
      
      i4 = GNUNET_malloc (sizeof (struct sockaddr_in));
      i4->sin_family = AF_INET;
      i4->sin_port = htons ((uint16_t) port);
      *sock_len = sizeof (struct sockaddr_in);
      in = (struct sockaddr *) i4;
    }
    else
    {
      struct sockaddr_in6 *i6;
      
      i6 = GNUNET_malloc (sizeof (struct sockaddr_in6));
      i6->sin6_family = AF_INET6;
      i6->sin6_port = htons ((uint16_t) port);
      *sock_len = sizeof (struct sockaddr_in6);
      in = (struct sockaddr *) i6;
    }
    return in;
  }
  cp = GNUNET_strdup (bindto);
  colon = strrchr (cp, ':');
  if (NULL != colon)
  {
    /* interpet value after colon as port */
    *colon = '\0';
    colon++;
    if (1 == SSCANF (colon,
		     "%u%1s",
		     &port,
		     dummy))
    {
      /* interpreting value as just a PORT number */
      if (port > UINT16_MAX)
      {
	GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		    "BINDTO specification `%s' invalid: value too large for port\n",
		    bindto);
	GNUNET_free (cp);
	return NULL;
      }
    }
    else
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		  "BINDTO specification `%s' invalid: last ':' not followed by number\n",
		  bindto);
      GNUNET_free (cp);
      return NULL;
    }
  }
  else
  {
    /* interpret missing port as 0, aka pick any free one */
    port = 0;
  }
  {
    /* try IPv4 */
    struct sockaddr_in v4;

    if (1 == inet_pton (AF_INET,
			cp,
			&v4))
    {
      v4.sin_port = htons ((uint16_t) port);
      in = GNUNET_memdup (&v4,
			  sizeof (v4));
      *sock_len = sizeof (v4);
      GNUNET_free (cp);
      return in;
    }
  }
  {
    /* try IPv6 */
    struct sockaddr_in6 v6;
    const char *start;

    start = cp;
    if ( ('[' == *cp) &&
	 (']' == cp[strlen (cp)-1]) )
    {
      start++; /* skip over '[' */
      cp[strlen (cp) -1] = '\0'; /* eat ']' */
    }
    if (1 == inet_pton (AF_INET6,
			start,
			&v6))
    {
      v6.sin6_port = htons ((uint16_t) port);
      in = GNUNET_memdup (&v6,
			  sizeof (v6));
      *sock_len = sizeof (v6);
      GNUNET_free (cp);
      return in;
    }
  }
  /* #5528 FIXME (feature!): maybe also try getnameinfo()? */
  GNUNET_free (cp);
  return NULL;
}


#if 0
/**
 *
 * 
 */
static void
XXX_write (void *cls)
{
  ssize_t sent;

  sent = GNUNET_NETWORK_socket_sendto (udp_sock,
				       );
  if (-1 == sent)
  {
    GNUNET_log_strerror (GNUNET_ERROR_TYPE_WARNING,
			 "send");
    return;			 
  }
}
#endif


/**
 * Signature of functions implementing the sending functionality of a
 * message queue.
 *
 * @param mq the message queue
 * @param msg the message to send
 * @param impl_state our `struct ReceiverAddress`
 */
static void
mq_send (struct GNUNET_MQ_Handle *mq,
	 const struct GNUNET_MessageHeader *msg,
	 void *impl_state)
{
  struct ReceiverAddress *receiver = impl_state;
  uint16_t msize = ntohs (msg->size);

  GNUNET_assert (mq == receiver->mq);
  // FIXME: pick encryption method, encrypt and transmit and call MQ-send-contiue!!

#if 0
  /* compute 'tc' and append in encrypted format to cwrite_buf */
  tc.sender = my_identity;
  tc.monotonic_time = GNUNET_TIME_absolute_hton (GNUNET_TIME_absolute_get_monotonic (cfg));
  ths.purpose.purpose = htonl (GNUNET_SIGNATURE_COMMUNICATOR_UDP_HANDSHAKE);
  ths.purpose.size = htonl (sizeof (ths));
  ths.sender = my_identity;
  ths.receiver = queue->target;
  ths.ephemeral = *epub;
  ths.monotonic_time = tc.monotonic_time;
  GNUNET_assert (GNUNET_OK ==
		 GNUNET_CRYPTO_eddsa_sign (my_private_key,
					   &ths.purpose,
					   &tc.sender_sig));
  GNUNET_assert (0 ==
		 gcry_cipher_encrypt (queue->out_cipher,
				      &queue->cwrite_buf[queue->cwrite_off],
				      sizeof (tc),
				      &tc,
				      sizeof (tc)));
#endif


}


/**
 * Signature of functions implementing the destruction of a message
 * queue.  Implementations must not free @a mq, but should take care
 * of @a impl_state.
 *
 * @param mq the message queue to destroy
 * @param impl_state our `struct ReceiverAddress`
 */
static void
mq_destroy (struct GNUNET_MQ_Handle *mq,
	    void *impl_state)
{
  struct ReceiverAddress *receiver = impl_state;

  if (mq == receiver->mq)
  {
    receiver->mq = NULL;
    receiver_destroy (receiver);
  }
}


/**
 * Implementation function that cancels the currently sent message.
 *
 * @param mq message queue
 * @param impl_state our `struct RecvierAddress`
 */
static void
mq_cancel (struct GNUNET_MQ_Handle *mq,
	   void *impl_state)
{
  /* Cancellation is impossible with UDP; bail */
  GNUNET_assert (0);
}


/**
 * Generic error handler, called with the appropriate
 * error code and the same closure specified at the creation of
 * the message queue.
 * Not every message queue implementation supports an error handler.
 *
 * @param cls our `struct ReceiverAddress`
 * @param error error code
 */
static void
mq_error (void *cls,
	  enum GNUNET_MQ_Error error)
{
  struct ReceiverAddress *receiver = cls;

  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
	      "MQ error in queue to %s: %d\n",
	      GNUNET_i2s (&receiver->target),
	      (int) error);
  receiver_destroy (receiver);
}


/**
 * Setup a receiver for transmission.  Setup the MQ processing and
 * inform transport that the queue is ready. 
 *
 * @param 
 */ 
static struct ReceiverAddress *
receiver_setup (const struct GNUNET_PeerIdentity *target,
		const struct sockddr *address,
		socklen_t address_len)
{
  struct ReceiverAddress *receiver;

  receiver = GNUNET_new (struct ReceiverAddress);
  receiver->address = GNUNET_memdup (address,
				     address_len);
  receiver->address_len = address_len;
  receiver->target = *target;
  receiver->nt = GNUNET_NT_scanner_get_type (is,
					     address,
					     address_len);
  (void) GNUNET_CONTAINER_multipeermap_put (receivers,
					    &receiver->target,
					    receiver,
					    GNUNET_CONTAINER_MULTIHASHMAPOPTION_MULTIPLE);
  // FIXME: add to receiver heap!
  GNUNET_STATISTICS_set (stats,
			 "# receivers active",
			 GNUNET_CONTAINER_multipeermap_size (receivers),
			 GNUNET_NO);
  receiver->timeout
    = GNUNET_TIME_relative_to_absolute (GNUNET_CONSTANTS_IDLE_CONNECTION_TIMEOUT);
  receiver->mq
    = GNUNET_MQ_queue_for_callbacks (&mq_send,
				     &mq_destroy,
				     &mq_cancel,
				     receiver,
				     NULL,
				     &mq_error,
				     receiver);
  {
    char *foreign_addr;

    switch (address->sa_family)
    {
    case AF_INET:
      GNUNET_asprintf (&foreign_addr,
		       "%s-%s",
		       COMMUNICATOR_ADDRESS_PREFIX,
		       GNUNET_a2s(queue->address,
				  queue->address_len));
      break;
    case AF_INET6:
      GNUNET_asprintf (&foreign_addr,
		       "%s-%s",
		       COMMUNICATOR_ADDRESS_PREFIX,
		       GNUNET_a2s(queue->address,
				  queue->address_len));
      break;
    default:
      GNUNET_assert (0);
    }
    queue->qh
      = GNUNET_TRANSPORT_communicator_mq_add (ch,
					      &receiver->target,
					      foreign_addr,
					      1200 /* FIXME: MTU OK? */,
					      queue->nt,
					      GNUNET_TRANSPORT_CS_OUTBOUND,
					      queue->mq);
    GNUNET_free (foreign_addr);
  }
}


/**
 * Function called by the transport service to initialize a
 * message queue given address information about another peer.
 * If and when the communication channel is established, the
 * communicator must call #GNUNET_TRANSPORT_communicator_mq_add()
 * to notify the service that the channel is now up.  It is
 * the responsibility of the communicator to manage sane
 * retries and timeouts for any @a peer/@a address combination
 * provided by the transport service.  Timeouts and retries
 * do not need to be signalled to the transport service.
 *
 * @param cls closure
 * @param peer identity of the other peer
 * @param address where to send the message, human-readable
 *        communicator-specific format, 0-terminated, UTF-8
 * @return #GNUNET_OK on success, #GNUNET_SYSERR if the provided address is invalid
 */
static int
mq_init (void *cls,
	 const struct GNUNET_PeerIdentity *peer,
	 const char *address)
{
  struct ReceiverAddress *receiver;
  const char *path;
  struct sockaddr *in;
  socklen_t in_len;
  
  if (0 != strncmp (address,
		    COMMUNICATOR_ADDRESS_PREFIX "-",
		    strlen (COMMUNICATOR_ADDRESS_PREFIX "-")))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  path = &address[strlen (COMMUNICATOR_ADDRESS_PREFIX "-")];
  in = udp_address_to_sockaddr (path,
				&in_len);  
  receiver = receiver_setup (peer,
			     in,
			     in_len);
  return GNUNET_OK;  
}


/**
 * Iterator over all receivers to clean up.
 *
 * @param cls NULL
 * @param target unused
 * @param value the queue to destroy
 * @return #GNUNET_OK to continue to iterate
 */
static int
get_receiver_delete_it (void *cls,
			const struct GNUNET_PeerIdentity *target,
			void *value)
{
  struct ReceiverAddress *receiver = value;

  (void) cls;
  (void) target;
  receiver_destroy (receiver);
  return GNUNET_OK;
}


/**
 * Iterator over all senders to clean up.
 *
 * @param cls NULL
 * @param target unused
 * @param value the queue to destroy
 * @return #GNUNET_OK to continue to iterate
 */
static int
get_receiver_delete_it (void *cls,
			const struct GNUNET_PeerIdentity *target,
			void *value)
{
  struct SenderAddress *sender = value;

  (void) cls;
  (void) target;
  sender_destroy (sender);
  return GNUNET_OK;
}


/**
 * Shutdown the UNIX communicator.
 *
 * @param cls NULL (always)
 */
static void
do_shutdown (void *cls)
{
  if (NULL != nat)
  {
     GNUNET_NAT_unregister (nat);
     nat = NULL;
  }
  if (NULL != read_task)
  {
    GNUNET_SCHEDULER_cancel (read_task);
    read_task = NULL;
  }
  if (NULL != udp_sock)
  {
    GNUNET_break (GNUNET_OK ==
                  GNUNET_NETWORK_socket_close (udp_sock));
    udp_sock = NULL;
  }
  GNUNET_CONTAINER_multipeermap_iterate (receivers,
					 &get_receiver_delete_it,
                                         NULL);
  GNUNET_CONTAINER_multipeermap_destroy (receivers);
  GNUNET_CONTAINER_multipeermap_iterate (senders,
					 &get_sender_delete_it,
                                         NULL);
  GNUNET_CONTAINER_multipeermap_destroy (senders);
  GNUNET_CONTAINER_multishortmap_destroy (key_cache);
  GNUNET_CONTAINER_heap_destroy (senders_heap);
  GNUNET_CONTAINER_heap_destroy (receivers_heap);
  if (NULL != ch)
  {
    GNUNET_TRANSPORT_communicator_disconnect (ch);
    ch = NULL;
  }
  if (NULL != stats)
  {
    GNUNET_STATISTICS_destroy (stats,
			       GNUNET_NO);
    stats = NULL;
  }
  if (NULL != my_private_key)
  {
    GNUNET_free (my_private_key);
    my_private_key = NULL;
  }
  if (NULL != is)
  {
     GNUNET_NT_scanner_done (is);
     is = NULL;
  }
}


/**
 * Function called when the transport service has received an
 * acknowledgement for this communicator (!) via a different return
 * path.
 *
 * Not applicable for UDP.
 *
 * @param cls closure
 * @param sender which peer sent the notification
 * @param msg payload
 */
static void
enc_notify_cb (void *cls,
               const struct GNUNET_PeerIdentity *sender,
               const struct GNUNET_MessageHeader *msg)
{
  (void) cls;
  (void) sender;
  (void) msg;
  GNUNET_break_op (0);
}


/**
 * Signature of the callback passed to #GNUNET_NAT_register() for
 * a function to call whenever our set of 'valid' addresses changes.
 *
 * @param cls closure
 * @param app_ctx[in,out] location where the app can store stuff
 *                  on add and retrieve it on remove
 * @param add_remove #GNUNET_YES to add a new public IP address, 
 *                   #GNUNET_NO to remove a previous (now invalid) one
 * @param ac address class the address belongs to
 * @param addr either the previous or the new public IP address
 * @param addrlen actual length of the @a addr
 */
static void
nat_address_cb (void *cls,
		void **app_ctx,
		int add_remove,
		enum GNUNET_NAT_AddressClass ac,
		const struct sockaddr *addr,
		socklen_t addrlen)
{
  char *my_addr;
  struct GNUNET_TRANSPORT_AddressIdentifier *ai;

  if (GNUNET_YES == add_remove)
  {
    enum GNUNET_NetworkType nt;

    GNUNET_asprintf (&my_addr,
		     "%s-%s",
		     COMMUNICATOR_ADDRESS_PREFIX,
		     GNUNET_a2s (addr,
				 addrlen));
    nt = GNUNET_NT_scanner_get_type (is,
				     addr,
				     addrlen); 
    ai = GNUNET_TRANSPORT_communicator_address_add (ch,
						    my_addr,
						    nt,
						    GNUNET_TIME_UNIT_FOREVER_REL);
    GNUNET_free (my_addr);
    *app_ctx = ai;
  }
  else
  {
    ai = *app_ctx;
    GNUNET_TRANSPORT_communicator_address_remove (ai);
    *app_ctx = NULL;
  }
}


/**
 * Setup communicator and launch network interactions.
 *
 * @param cls NULL (always)
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param c configuration
 */
static void
run (void *cls,
     char *const *args,
     const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *c)
{
  char *bindto;
  struct sockaddr *in;
  socklen_t in_len;
  struct sockaddr_storage in_sto;
  socklen_t sto_len;
  
  (void) cls;
  cfg = c;
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_filename (cfg,
					       COMMUNICATOR_CONFIG_SECTION,
					       "BINDTO",
					       &bindto))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               COMMUNICATOR_CONFIG_SECTION,
                               "BINDTO");
    return;
  }
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_number (cfg,
					     COMMUNICATOR_CONFIG_SECTION,
					     "MAX_QUEUE_LENGTH",
					     &max_queue_length))
    max_queue_length = DEFAULT_MAX_QUEUE_LENGTH;

  in = udp_address_to_sockaddr (bindto,
				&in_len);
  if (NULL == in)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		"Failed to setup UDP socket address with path `%s'\n",
		bindto);
    GNUNET_free (bindto);
    return;
  }
  udp_sock = GNUNET_NETWORK_socket_create (in->sa_family,
					   SOCK_DGRAM,
					   IPPROTO_UDP);
  if (NULL == udp_sock)
  {
    GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
			 "socket");
    GNUNET_free (in);
    GNUNET_free (bindto);
    return;
  }
  if (GNUNET_OK !=
      GNUNET_NETWORK_socket_bind (udp_sock,
                                  in,
				  in_len))
  {
    GNUNET_log_strerror_file (GNUNET_ERROR_TYPE_ERROR,
			      "bind",
			      bindto);
    GNUNET_NETWORK_socket_close (udp_sock);
    listen_sock = NULL;
    GNUNET_free (in);
    GNUNET_free (bindto);
    return;
  }
  /* We might have bound to port 0, allowing the OS to figure it out;
     thus, get the real IN-address from the socket */
  sto_len = sizeof (in_sto);
  if (0 != getsockname (GNUNET_NETWORK_get_fd (udp_sock),
			(struct sockaddr *) &in_sto,
			&sto_len))
  {
    memcpy (&in_sto,
	    in,
	    in_len);
    sto_len = in_len;
  }
  GNUNET_free (in);
  GNUNET_free (bindto);
  in = (struct sockaddr *) &in_sto;
  in_len = sto_len;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
	      "Bound to `%s'\n",
	      GNUNET_a2s ((const struct sockaddr *) &in_sto,
			  sto_len));
  stats = GNUNET_STATISTICS_create ("C-UDP",
				    cfg);
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown,
				 NULL);
  is = GNUNET_NT_scanner_init ();
  my_private_key = GNUNET_CRYPTO_eddsa_key_create_from_configuration (cfg);
  if (NULL == my_private_key)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                _("Transport service is lacking key configuration settings. Exiting.\n"));
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  GNUNET_CRYPTO_eddsa_key_get_public (my_private_key,
                                      &my_identity.public_key);
  /* start listening */
  read_task = GNUNET_SCHEDULER_add_read_net (GNUNET_TIME_UNIT_FOREVER_REL,
					     udp_sock,
					     &sock_read,
					     NULL);
  senders = GNUNET_CONTAINER_multipeermap_create (32,
						  GNUNET_YES);
  receivers = GNUNET_CONTAINER_multipeermap_create (32,
						    GNUNET_YES);
  key_cache = GNUNET_CONTAINER_multishortmap_create (1024,
						     GNUNET_YES);
  ch = GNUNET_TRANSPORT_communicator_connect (cfg,
					      COMMUNICATOR_CONFIG_SECTION,
					      COMMUNICATOR_ADDRESS_PREFIX,
                                              GNUNET_TRANSPORT_CC_UNRELIABLE,
					      &mq_init,
					      NULL,
                                              &enc_notify_cb,
                                              NULL);
  if (NULL == ch)
  {
    GNUNET_break (0);
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  nat = GNUNET_NAT_register (cfg,
			     COMMUNICATOR_CONFIG_SECTION,
			     IPPROTO_UDP,
			     1 /* one address */,
			     (const struct sockaddr **) &in,
			     &in_len,
			     &nat_address_cb,
			     NULL /* FIXME: support reversal: #5529 */,
			     NULL /* closure */);
}


/**
 * The main function for the UNIX communicator.
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc,
      char *const *argv)
{
  static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_OPTION_END
  };
  int ret;

  if (GNUNET_OK !=
      GNUNET_STRINGS_get_utf8_args (argc, argv,
				    &argc, &argv))
    return 2;

  ret =
      (GNUNET_OK ==
       GNUNET_PROGRAM_run (argc, argv,
                           "gnunet-communicator-udp",
                           _("GNUnet UDP communicator"),
                           options,
			   &run,
			   NULL)) ? 0 : 1;
  GNUNET_free ((void*) argv);
  return ret;
}


/* end of gnunet-communicator-udp.c */