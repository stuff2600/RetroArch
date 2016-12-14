/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2016 - Daniel De Matteis
 *  Copyright (C)      2016 - Gregor Richards
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#if defined(_MSC_VER) && !defined(_XBOX)
#pragma comment(lib, "ws2_32")
#endif

#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <retro_assert.h>
#include <net/net_compat.h>
#include <net/net_socket.h>
#include <features/features_cpu.h>
#include <retro_endianness.h>

#include "netplay_private.h"
#include "netplay_discovery.h"

#include "../../autosave.h"
#include "../../configuration.h"
#include "../../command.h"
#include "../../movie.h"
#include "../../runloop.h"

#define MAX_RETRIES                 16
#define RETRY_MS                    500

#if defined(AF_INET6) && !defined(HAVE_SOCKET_LEGACY)
#define HAVE_INET6 1
#endif

static int init_tcp_connection(const struct addrinfo *res,
      bool server,
      struct sockaddr *other_addr, socklen_t addr_size)
{
   bool ret = true;
   int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

   if (fd < 0)
   {
      ret = false;
      goto end;
   }

#if defined(IPPROTO_TCP) && defined(TCP_NODELAY)
   {
      int flag = 1;
      if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
#ifdef _WIN32
         (const char*)
#else
         (const void*)
#endif
         &flag,
         sizeof(int)) < 0)
         RARCH_WARN("Could not set netplay TCP socket to nodelay. Expect jitter.\n");
   }
#endif

#if defined(F_SETFD) && defined(FD_CLOEXEC)
   /* Don't let any inherited processes keep open our port */
   if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
      RARCH_WARN("Cannot set Netplay port to close-on-exec. It may fail to reopen if the client disconnects.\n");
#endif

   if (server)
   {
      if (socket_connect(fd, (void*)res, false) < 0)
      {
         ret = false;
         goto end;
      }
   }
   else
   {
#if defined(HAVE_INET6) && defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
      /* Make sure we accept connections on both IPv6 and IPv4 */
      int on = 0;
      if (res->ai_family == AF_INET6)
      {
         if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&on, sizeof(on)) < 0)
            RARCH_WARN("Failed to listen on both IPv6 and IPv4\n");
      }
#endif
      if (  !socket_bind(fd, (void*)res) || 
            listen(fd, 1024) < 0)
      {
         ret = false;
         goto end;
      }
   }

end:
   if (!ret && fd >= 0)
   {
      socket_close(fd);
      fd = -1;
   }

   return fd;
}

static bool init_tcp_socket(netplay_t *netplay, void *direct_host,
      const char *server, uint16_t port)
{
   char port_buf[16];
   bool ret                        = false;
   const struct addrinfo *tmp_info = NULL;
   struct addrinfo *res            = NULL;
   struct addrinfo hints           = {0};

   port_buf[0] = '\0';

   if (!direct_host)
   {
#ifdef HAVE_INET6
      /* Default to hosting on IPv6 and IPv4 */
      if (!server)
         hints.ai_family = AF_INET6;
#endif
      hints.ai_socktype = SOCK_STREAM;
      if (!server)
         hints.ai_flags = AI_PASSIVE;

      snprintf(port_buf, sizeof(port_buf), "%hu", (unsigned short)port);
      if (getaddrinfo_retro(server, port_buf, &hints, &res) < 0)
      {
#ifdef HAVE_INET6
         if (!server)
         {
            /* Didn't work with IPv6, try wildcard */
            hints.ai_family = 0;
            if (getaddrinfo_retro(server, port_buf, &hints, &res) < 0)
               return false;
         }
         else
#endif
         return false;
      }

      if (!res)
         return false;

   }
   else
   {
      /* I'll build my own addrinfo! With blackjack and hookers! */
      struct netplay_host *host = (struct netplay_host *) direct_host;
      hints.ai_family = host->addr.sa_family;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_protocol = 0;
      hints.ai_addrlen = host->addrlen;
      hints.ai_addr = &host->addr;
      res = &hints;

   }

   /* If we're serving on IPv6, make sure we accept all connections, including
    * IPv4 */
#ifdef HAVE_INET6
   if (!direct_host && !server && res->ai_family == AF_INET6)
   {
      struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) res->ai_addr;
      sin6->sin6_addr = in6addr_any;
   }
#endif

   /* If "localhost" is used, it is important to check every possible 
    * address for IPv4/IPv6. */
   tmp_info = res;

   while (tmp_info)
   {
      struct sockaddr_storage sad;
      int fd = init_tcp_connection(
            tmp_info,
            direct_host || server,
            (struct sockaddr*)&sad,
            sizeof(sad));

      if (fd >= 0)
      {
         ret = true;
         if (direct_host || server)
         {
            netplay->connections[0].active = true;
            netplay->connections[0].fd = fd;
            netplay->connections[0].addr = sad;
         }
         else
         {
            netplay->listen_fd = fd;
         }
         break;
      }

      tmp_info = tmp_info->ai_next;
   }

   if (res && !direct_host)
      freeaddrinfo_retro(res);

   if (!ret)
      RARCH_ERR("Failed to set up netplay sockets.\n");

   return ret;
}

static bool init_socket(netplay_t *netplay, void *direct_host, const char *server, uint16_t port)
{
   if (!network_init())
      return false;

   if (!init_tcp_socket(netplay, direct_host, server, port))
      return false;

   if (netplay->is_server && netplay->nat_traversal)
      netplay_init_nat_traversal(netplay);

   return true;
}

/**
 * netplay_hangup:
 *
 * Disconnects an active Netplay connection due to an error
 **/
void netplay_hangup(netplay_t *netplay, struct netplay_connection *connection)
{
   if (!netplay)
      return;
   if (!connection->active)
      return;

   RARCH_WARN("Netplay has disconnected. Will continue without connection ...\n");
   runloop_msg_queue_push("Netplay has disconnected. Will continue without connection.", 0, 480, false);

   socket_close(connection->fd);
   connection->active = false;
   netplay_deinit_socket_buffer(&connection->send_packet_buffer);
   netplay_deinit_socket_buffer(&connection->recv_packet_buffer);

   if (!netplay->is_server)
   {
      netplay->self_mode = NETPLAY_CONNECTION_NONE;
      netplay->connected_players = 0;

   }
   else
   {
      /* Remove this player */
      if (connection->mode == NETPLAY_CONNECTION_PLAYING)
      {
         netplay->connected_players &= ~(1<<connection->player);

         /* FIXME: Duplication */
         if (netplay->is_server)
         {
            uint32_t payload[2];
            payload[0] = htonl(netplay->read_frame_count[connection->player]);
            payload[1] = htonl(connection->player);
            netplay_send_raw_cmd_all(netplay, connection, NETPLAY_CMD_MODE, payload, sizeof(payload));
         }
      }

   }
}

/**
 * netplay_update_unread_ptr
 *
 * Update the global unread_ptr and unread_frame_count to correspond to the
 * earliest unread frame count of any connected player */
void netplay_update_unread_ptr(netplay_t *netplay)
{
   if (netplay->is_server && !netplay->connected_players)
   {
      /* Nothing at all to read! */
      netplay->unread_ptr = netplay->self_ptr;
      netplay->unread_frame_count = netplay->self_frame_count;

   }
   else
   {
      size_t new_unread_ptr = 0;
      uint32_t new_unread_frame_count = (uint32_t) -1;
      uint32_t player;

      for (player = 0; player < MAX_USERS; player++)
      {
         if (!(netplay->connected_players & (1<<player))) continue;
         if (netplay->read_frame_count[player] < new_unread_frame_count)
         {
            new_unread_ptr = netplay->read_ptr[player];
            new_unread_frame_count = netplay->read_frame_count[player];
         }
      }

      if (!netplay->is_server && netplay->server_frame_count < new_unread_frame_count)
      {
         new_unread_ptr = netplay->server_ptr;
         new_unread_frame_count = netplay->server_frame_count;
      }

      netplay->unread_ptr = new_unread_ptr;
      netplay->unread_frame_count = new_unread_frame_count;
   }
}

/* Send the specified input data */
static bool send_input_frame(netplay_t *netplay,
   struct netplay_connection *only, struct netplay_connection *except,
   uint32_t frame, uint32_t player, uint32_t *state)
{
   uint32_t buffer[2 + WORDS_PER_FRAME];
   size_t i;

   buffer[0] = htonl(NETPLAY_CMD_INPUT);
   buffer[1] = htonl(WORDS_PER_FRAME * sizeof(uint32_t));
   buffer[2] = htonl(frame);
   buffer[3] = htonl(player);
   buffer[4] = htonl(state[0]);
   buffer[5] = htonl(state[1]);
   buffer[6] = htonl(state[2]);

   if (only)
   {
      if (!netplay_send(&only->send_packet_buffer, only->fd, buffer, sizeof(buffer)))
      {
         netplay_hangup(netplay, only);
         return false;
      }
   }
   else
   {
      for (i = 0; i < netplay->connections_size; i++)
      {
         struct netplay_connection *connection = &netplay->connections[i];
         if (connection == except) continue;
         if (connection->active &&
             connection->mode >= NETPLAY_CONNECTION_CONNECTED &&
             (connection->mode != NETPLAY_CONNECTION_PLAYING ||
              connection->player != player))
         {
            if (!netplay_send(&connection->send_packet_buffer, connection->fd,
                  buffer, sizeof(buffer)))
               netplay_hangup(netplay, connection);
         }
      }
   }

   return true;
}

/* Send the current input frame */
bool netplay_send_cur_input(netplay_t *netplay,
   struct netplay_connection *connection)
{
   struct delta_frame *dframe = &netplay->buffer[netplay->self_ptr];
   uint32_t player;

   if (netplay->is_server)
   {
      /* Send the other players' input data */
      for (player = 0; player < MAX_USERS; player++)
      {
         if (connection->mode == NETPLAY_CONNECTION_PLAYING &&
               connection->player == player)
            continue;
         if ((netplay->connected_players & (1<<player)))
         {
            if (dframe->have_real[player])
            {
               if (!send_input_frame(netplay, connection, NULL,
                        netplay->self_frame_count, player,
                        dframe->real_input_state[player]))
                  return false;
            }
         }
      }

      /* If we're not playing, send a NOINPUT */
      if (netplay->self_mode != NETPLAY_CONNECTION_PLAYING)
      {
         uint32_t payload = htonl(netplay->self_frame_count);
         if (!netplay_send_raw_cmd(netplay, connection, NETPLAY_CMD_NOINPUT,
               &payload, sizeof(payload)))
            return false;
      }

   }

   /* Send our own data */
   if (netplay->self_mode == NETPLAY_CONNECTION_PLAYING)
   {
      if (!send_input_frame(netplay, connection, NULL,
            netplay->self_frame_count,
            (netplay->is_server ?  NETPLAY_CMD_INPUT_BIT_SERVER : 0) | netplay->self_player,
            dframe->self_state))
         return false;
   }

   if (!netplay_send_flush(&connection->send_packet_buffer, connection->fd,
         false))
      return false;

   return true;
}

/**
 * netplay_send_raw_cmd
 *
 * Send a raw Netplay command to the given connection
 *
 * Returns true on success, false on failure
 */
bool netplay_send_raw_cmd(netplay_t *netplay,
   struct netplay_connection *connection, uint32_t cmd, const void *data,
   size_t size)
{
   uint32_t cmdbuf[2];

   cmdbuf[0] = htonl(cmd);
   cmdbuf[1] = htonl(size);

   if (!netplay_send(&connection->send_packet_buffer, connection->fd, cmdbuf,
         sizeof(cmdbuf)))
      return false;

   if (size > 0)
      if (!netplay_send(&connection->send_packet_buffer, connection->fd, data, size))
         return false;

   return true;
}

/**
 * netplay_send_raw_cmd_all
 *
 * Send a raw Netplay command to all connections, optionally excluding one
 * (typically the client that the relevant command came from)
 */
void netplay_send_raw_cmd_all(netplay_t *netplay,
   struct netplay_connection *except, uint32_t cmd, const void *data,
   size_t size)
{
   size_t i;
   for (i = 0; i < netplay->connections_size; i++)
   {
      struct netplay_connection *connection = &netplay->connections[i];
      if (connection == except)
         continue;
      if (connection->active && connection->mode >= NETPLAY_CONNECTION_CONNECTED)
      {
         if (!netplay_send_raw_cmd(netplay, connection, cmd, data, size))
            netplay_hangup(netplay, connection);
      }
   }
}

static bool netplay_cmd_nak(netplay_t *netplay,
   struct netplay_connection *connection)
{
   netplay_send_raw_cmd(netplay, connection, NETPLAY_CMD_NAK, NULL, 0);
   return false;
}

bool netplay_cmd_crc(netplay_t *netplay, struct delta_frame *delta)
{
   uint32_t payload[2];
   bool success = true;
   size_t i;
   payload[0] = htonl(delta->frame);
   payload[1] = htonl(delta->crc);
   for (i = 0; i < netplay->connections_size; i++)
   {
      if (netplay->connections[i].active &&
            netplay->connections[i].mode >= NETPLAY_CONNECTION_CONNECTED)
         success = netplay_send_raw_cmd(netplay, &netplay->connections[i],
            NETPLAY_CMD_CRC, payload, sizeof(payload)) && success;
   }
   return success;
}

bool netplay_cmd_request_savestate(netplay_t *netplay)
{
   if (netplay->connections_size == 0 ||
       !netplay->connections[0].active ||
       netplay->connections[0].mode < NETPLAY_CONNECTION_CONNECTED)
      return false;
   if (netplay->savestate_request_outstanding)
      return true;
   netplay->savestate_request_outstanding = true;
   return netplay_send_raw_cmd(netplay, &netplay->connections[0],
      NETPLAY_CMD_REQUEST_SAVESTATE, NULL, 0);
}

bool netplay_cmd_mode(netplay_t *netplay,
   struct netplay_connection *connection,
   enum rarch_netplay_connection_mode mode)
{
   uint32_t cmd;
   switch (mode)
   {
      case NETPLAY_CONNECTION_SPECTATING:
         cmd = NETPLAY_CMD_SPECTATE;
         break;

      case NETPLAY_CONNECTION_PLAYING:
         cmd = NETPLAY_CMD_PLAY;
         break;

      default:
         return false;
   }
   return netplay_send_raw_cmd(netplay, connection, cmd, NULL, 0);
}

static bool netplay_get_cmd(netplay_t *netplay,
   struct netplay_connection *connection, bool *had_input)
{
   uint32_t cmd;
   uint32_t flip_frame;
   uint32_t cmd_size;
   ssize_t recvd;
   char msg[512];

   /* We don't handle the initial handshake here */
   switch (connection->mode)
   {
      case NETPLAY_CONNECTION_NONE:
         /* Huh?! */
         return false;
      case NETPLAY_CONNECTION_INIT:
         return netplay_handshake_init(netplay, connection, had_input);
      case NETPLAY_CONNECTION_PRE_NICK:
      {
         bool ret = netplay_handshake_pre_nick(netplay, connection, had_input);
         if (connection->mode >= NETPLAY_CONNECTION_CONNECTED &&
             !netplay_send_cur_input(netplay, connection))
            return false;
         return ret;
      }
      case NETPLAY_CONNECTION_PRE_PASSWORD:
      {
         bool ret = netplay_handshake_pre_password(netplay, connection, had_input);
         if (connection->mode >= NETPLAY_CONNECTION_CONNECTED &&
             !netplay_send_cur_input(netplay, connection))
            return false;
         return ret;
      }
      case NETPLAY_CONNECTION_PRE_SYNC:
      {
         bool ret = netplay_handshake_pre_sync(netplay, connection, had_input);
         if (connection->mode >= NETPLAY_CONNECTION_CONNECTED &&
             !netplay_send_cur_input(netplay, connection))
            return false;
         return ret;
      }
      default:
         break;
   }

   /* FIXME: This depends on delta_frame_ready */

#define RECV(buf, sz) \
   recvd = netplay_recv(&connection->recv_packet_buffer, connection->fd, (buf), \
      (sz), false); \
   if (recvd >= 0 && recvd < (sz)) goto shrt; \
   else if (recvd < 0)

   RECV(&cmd, sizeof(cmd))
      return false;

   cmd      = ntohl(cmd);

   RECV(&cmd_size, sizeof(cmd_size))
      return false;

   cmd_size = ntohl(cmd_size);

   netplay->timeout_cnt = 0;

   switch (cmd)
   {
      case NETPLAY_CMD_ACK:
         /* Why are we even bothering? */
         break;

      case NETPLAY_CMD_NAK:
         /* Disconnect now! */
         return false;

      case NETPLAY_CMD_INPUT:
         {
            uint32_t buffer[WORDS_PER_FRAME];
            uint32_t player;
            unsigned i;
            struct delta_frame *dframe;

            if (cmd_size != WORDS_PER_FRAME * sizeof(uint32_t))
            {
               RARCH_ERR("NETPLAY_CMD_INPUT received an unexpected payload size.\n");
               return netplay_cmd_nak(netplay, connection);
            }

            RECV(buffer, sizeof(buffer))
            {
               RARCH_ERR("Failed to receive NETPLAY_CMD_INPUT input.\n");
               return netplay_cmd_nak(netplay, connection);
            }

            for (i = 0; i < WORDS_PER_FRAME; i++)
               buffer[i] = ntohl(buffer[i]);

            if (netplay->is_server)
            {
               /* Ignore the claimed player #, must be this client */
               if (connection->mode != NETPLAY_CONNECTION_PLAYING)
                  return netplay_cmd_nak(netplay, connection);
               player = connection->player;
            }
            else
            {
               player = buffer[1] & ~NETPLAY_CMD_INPUT_BIT_SERVER;
            }

            if (player >= MAX_USERS || !(netplay->connected_players & (1<<player)))
               return netplay_cmd_nak(netplay, connection);

            if (buffer[0] < netplay->read_frame_count[player])
            {
               /* We already had this, so ignore the new transmission */
               break;
            }
            else if (buffer[0] > netplay->read_frame_count[player])
            {
               /* Out of order = out of luck */
               return netplay_cmd_nak(netplay, connection);
            }

            /* The data's good! */
            dframe = &netplay->buffer[netplay->read_ptr[player]];
            if (!netplay_delta_frame_ready(netplay, dframe, netplay->read_frame_count[player]))
            {
               /* FIXME: Catastrophe! */
               return netplay_cmd_nak(netplay, connection);
            }
            memcpy(dframe->real_input_state[player], buffer + 2,
               WORDS_PER_INPUT*sizeof(uint32_t));
            dframe->have_real[player] = true;
            netplay->read_ptr[player] = NEXT_PTR(netplay->read_ptr[player]);
            netplay->read_frame_count[player]++;

            if (netplay->is_server)
            {
               /* Forward it on if it's past data*/
               if (dframe->frame <= netplay->self_frame_count)
                  send_input_frame(netplay, NULL, connection, buffer[0],
                     player, dframe->real_input_state[player]);
            }

            /* If this was server data, advance our server pointer too */
            if (!netplay->is_server && (buffer[1] & NETPLAY_CMD_INPUT_BIT_SERVER))
            {
               netplay->server_ptr = netplay->read_ptr[player];
               netplay->server_frame_count = netplay->read_frame_count[player];
            }
            break;
         }

      case NETPLAY_CMD_NOINPUT:
         {
            uint32_t frame;

            if (netplay->is_server)
               return netplay_cmd_nak(netplay, connection);

            RECV(&frame, sizeof(frame))
               return netplay_cmd_nak(netplay, connection);
            frame = ntohl(frame);

            if (frame != netplay->server_frame_count)
               return netplay_cmd_nak(netplay, connection);

            netplay->server_ptr = NEXT_PTR(netplay->server_ptr);
            netplay->server_frame_count++;
            break;
         }

      case NETPLAY_CMD_FLIP_PLAYERS:
         if (cmd_size != sizeof(uint32_t))
         {
            RARCH_ERR("CMD_FLIP_PLAYERS received an unexpected command size.\n");
            return netplay_cmd_nak(netplay, connection);
         }

         RECV(&flip_frame, sizeof(flip_frame))
         {
            RARCH_ERR("Failed to receive CMD_FLIP_PLAYERS argument.\n");
            return netplay_cmd_nak(netplay, connection);
         }

         if (netplay->is_server)
            return netplay_cmd_nak(netplay, connection);

         flip_frame = ntohl(flip_frame);

         if (flip_frame < netplay->server_frame_count)
         {
            RARCH_ERR("Host asked us to flip users in the past. Not possible ...\n");
            return netplay_cmd_nak(netplay, connection);
         }

         netplay->flip ^= true;
         netplay->flip_frame = flip_frame;

         /* Force a rewind to assure the flip happens: This just prevents us
          * from skipping other past the flip because our prediction was
          * correct */
         if (flip_frame < netplay->self_frame_count)
            netplay->force_rewind = true;

         RARCH_LOG("%s.\n", msg_hash_to_str(MSG_NETPLAY_USERS_HAS_FLIPPED));
         runloop_msg_queue_push(
               msg_hash_to_str(MSG_NETPLAY_USERS_HAS_FLIPPED), 1, 180, false);

         break;

      case NETPLAY_CMD_SPECTATE:
      {
         uint32_t payload[2];

         if (!netplay->is_server)
            return netplay_cmd_nak(netplay, connection);

         if (connection->mode == NETPLAY_CONNECTION_PLAYING)
         {
            /* The frame we haven't received is their end frame */
            payload[0] = htonl(netplay->read_frame_count[connection->player]);

            /* Mark them as not playing anymore */
            connection->mode = NETPLAY_CONNECTION_SPECTATING;
            netplay->connected_players &= ~(1<<connection->player);

            /* Tell everyone */
            payload[1] = htonl(connection->player);
            netplay_send_raw_cmd_all(netplay, connection, NETPLAY_CMD_MODE, payload, sizeof(payload));

            /* Announce it */
            msg[sizeof(msg)-1] = '\0';
            snprintf(msg, sizeof(msg)-1, "Player %d has left", connection->player+1);
            RARCH_LOG("%s\n", msg);
            runloop_msg_queue_push(msg, 1, 180, false);
         }
         else
         {
            payload[0] = htonl(0);
         }

         /* Tell the player even if they were confused */
         payload[1] = htonl(NETPLAY_CMD_MODE_BIT_YOU | connection->player);
         netplay_send_raw_cmd(netplay, connection, NETPLAY_CMD_MODE, payload, sizeof(payload));
         break;
      }

      case NETPLAY_CMD_PLAY:
      {
         uint32_t payload[2];
         uint32_t player = 0;
         payload[0] = htonl(netplay->self_frame_count + 1);

         if (!netplay->is_server)
            return netplay_cmd_nak(netplay, connection);

         /* Find an available player slot */
         for (player = 0; player < MAX_USERS; player++)
         {
            if (!(netplay->self_mode == NETPLAY_CONNECTION_PLAYING &&
                  netplay->self_player == player) &&
                !(netplay->connected_players & (1<<player)))
               break;
         }
         if (player == MAX_USERS)
         {
            /* FIXME */
            return netplay_cmd_nak(netplay, connection);
         }

         if (connection->mode != NETPLAY_CONNECTION_PLAYING)
         {
            /* Mark them as playing */
            connection->mode = NETPLAY_CONNECTION_PLAYING;
            connection->player = player;
            netplay->connected_players |= 1<<player;

            /* Tell everyone */
            payload[1] = htonl(NETPLAY_CMD_MODE_BIT_PLAYING | connection->player);
            netplay_send_raw_cmd_all(netplay, connection, NETPLAY_CMD_MODE, payload, sizeof(payload));

            /* Announce it */
            msg[sizeof(msg)-1] = '\0';
            snprintf(msg, sizeof(msg)-1, "Player %d has joined", player+1);
            RARCH_LOG("%s\n", msg);
            runloop_msg_queue_push(msg, 1, 180, false);

         }

         /* Tell the player even if they were confused */
         payload[1] = htonl(NETPLAY_CMD_MODE_BIT_PLAYING |
            NETPLAY_CMD_MODE_BIT_YOU | connection->player);
         netplay_send_raw_cmd(netplay, connection, NETPLAY_CMD_MODE, payload, sizeof(payload));

         /* And expect their data */
         netplay->read_ptr[player] = NEXT_PTR(netplay->self_ptr);
         netplay->read_frame_count[player] = netplay->self_frame_count + 1;
         break;
      }

      case NETPLAY_CMD_MODE:
      {
         uint32_t payload[2];
         uint32_t frame, mode, player;
         size_t ptr;
         struct delta_frame *dframe;

#define START(which) \
         do { \
            ptr = which; \
            dframe = &netplay->buffer[ptr]; \
         } while(0)
#define NEXT() \
         do { \
            ptr = NEXT_PTR(ptr); \
            dframe = &netplay->buffer[ptr]; \
         } while(0)

         if (cmd_size != sizeof(payload) ||
             netplay->is_server)
            return netplay_cmd_nak(netplay, connection);

         RECV(payload, sizeof(payload))
         {
            RARCH_ERR("NETPLAY_CMD_MODE failed to receive payload.\n");
            return netplay_cmd_nak(netplay, connection);
         }

         if (netplay->is_server)
            return netplay_cmd_nak(netplay, connection);

         frame = ntohl(payload[0]);

         /* We're changing past input, so must replay it */
         if (frame < netplay->self_frame_count)
            netplay->force_rewind = true;

         mode = ntohl(payload[1]);
         player = mode & 0xFFFF;
         if (player >= MAX_USERS)
            return netplay_cmd_nak(netplay, connection);

         if (mode & NETPLAY_CMD_MODE_BIT_YOU)
         {
            /* A change to me! */
            if (mode & NETPLAY_CMD_MODE_BIT_PLAYING)
            {
               if (frame != netplay->server_frame_count)
                  return netplay_cmd_nak(netplay, connection);

               /* Hooray, I get to play now! */
               if (netplay->self_mode == NETPLAY_CONNECTION_PLAYING)
                  return netplay_cmd_nak(netplay, connection);

               netplay->self_mode = NETPLAY_CONNECTION_PLAYING;
               netplay->self_player = player;

               /* Fix up current frame info */
               if (frame <= netplay->self_frame_count)
               {
                  /* It wanted past frames, better send 'em! */
                  START(netplay->server_ptr);
                  while (dframe->used && dframe->frame <= netplay->self_frame_count)
                  {
                     memcpy(dframe->real_input_state[player], dframe->self_state, sizeof(dframe->self_state));
                     dframe->have_real[player] = true;
                     send_input_frame(netplay, connection, NULL, dframe->frame, player, dframe->self_state);
                     if (dframe->frame == netplay->self_frame_count) break;
                     NEXT();
                  }

               }
               else
               {
                  /* It wants future frames, make sure we don't capture or send intermediate ones */
                  START(netplay->self_ptr);
                  while (dframe->used && dframe->frame < frame)
                  {
                     memset(dframe->self_state, 0, sizeof(dframe->self_state));
                     memset(dframe->real_input_state[player], 0, sizeof(dframe->self_state));
                     dframe->have_local = true;
                     NEXT();
                  }

               }

               /* Announce it */
               msg[sizeof(msg)-1] = '\0';
               snprintf(msg, sizeof(msg)-1, "You have joined as player %d", player+1);
               RARCH_LOG("%s\n", msg);
               runloop_msg_queue_push(msg, 1, 180, false);

            }
            else /* YOU && !PLAYING */
            {
               /* I'm no longer playing, but I should already know this */
               if (netplay->self_mode != NETPLAY_CONNECTION_SPECTATING)
                  return netplay_cmd_nak(netplay, connection);

               /* Announce it */
               strlcpy(msg, "You have left the game", sizeof(msg));
               RARCH_LOG("%s\n", msg);
               runloop_msg_queue_push(msg, 1, 180, false);

            }

         }
         else /* !YOU */
         {
            /* Somebody else is joining or parting */
            if (mode & NETPLAY_CMD_MODE_BIT_PLAYING)
            {
               if (frame != netplay->server_frame_count)
                  return netplay_cmd_nak(netplay, connection);

               netplay->connected_players |= (1<<player);

               netplay->read_ptr[player] = netplay->server_ptr;
               netplay->read_frame_count[player] = netplay->server_frame_count;

               /* Announce it */
               msg[sizeof(msg)-1] = '\0';
               snprintf(msg, sizeof(msg)-1, "Player %d has joined", player+1);
               RARCH_LOG("%s\n", msg);
               runloop_msg_queue_push(msg, 1, 180, false);

            }
            else
            {
               netplay->connected_players &= ~(1<<player);

               /* Announce it */
               msg[sizeof(msg)-1] = '\0';
               snprintf(msg, sizeof(msg)-1, "Player %d has left", player+1);
               RARCH_LOG("%s\n", msg);
               runloop_msg_queue_push(msg, 1, 180, false);
            }

         }

         break;

#undef START
#undef NEXT
      }

      case NETPLAY_CMD_DISCONNECT:
         netplay_hangup(netplay, connection);
         return true;

      case NETPLAY_CMD_CRC:
         {
            uint32_t buffer[2];
            size_t tmp_ptr = netplay->self_ptr;
            bool found = false;

            if (cmd_size != sizeof(buffer))
            {
               RARCH_ERR("NETPLAY_CMD_CRC received unexpected payload size.\n");
               return netplay_cmd_nak(netplay, connection);
            }

            RECV(buffer, sizeof(buffer))
            {
               RARCH_ERR("NETPLAY_CMD_CRC failed to receive payload.\n");
               return netplay_cmd_nak(netplay, connection);
            }

            buffer[0] = ntohl(buffer[0]);
            buffer[1] = ntohl(buffer[1]);

            /* Received a CRC for some frame. If we still have it, check if it
             * matched. This approach could be improved with some quick modular
             * arithmetic. */
            do
            {
               if (     netplay->buffer[tmp_ptr].used 
                     && netplay->buffer[tmp_ptr].frame == buffer[0])
               {
                  found = true;
                  break;
               }

               tmp_ptr = PREV_PTR(tmp_ptr);
            } while (tmp_ptr != netplay->self_ptr);

            if (!found)
            {
               /* Oh well, we got rid of it! */
               break;
            }

            if (buffer[0] <= netplay->other_frame_count)
            {
               /* We've already replayed up to this frame, so we can check it
                * directly */
               uint32_t local_crc = netplay_delta_frame_crc(
                     netplay, &netplay->buffer[tmp_ptr]);

               if (buffer[1] != local_crc)
               {
                  /* Problem! */
                  netplay_cmd_request_savestate(netplay);
               }
            }
            else
            {
               /* We'll have to check it when we catch up */
               netplay->buffer[tmp_ptr].crc = buffer[1];
            }

            break;
         }

      case NETPLAY_CMD_REQUEST_SAVESTATE:
         /* Delay until next frame so we don't send the savestate after the
          * input */
         netplay->force_send_savestate = true;
         break;

      case NETPLAY_CMD_LOAD_SAVESTATE:
         {
            uint32_t frame;
            uint32_t isize;
            uint32_t rd, wn;
            uint32_t player;

            /* Make sure we're ready for it */
            if (netplay->quirks & NETPLAY_QUIRK_INITIALIZATION)
            {
               if (!netplay->is_replay)
               {
                  netplay->is_replay = true;
                  netplay->replay_ptr = netplay->self_ptr;
                  netplay->replay_frame_count = netplay->self_frame_count;
                  netplay_wait_and_init_serialization(netplay);
                  netplay->is_replay = false;
               }
               else
               {
                  netplay_wait_and_init_serialization(netplay);
               }
            }

            /* Only players may load states */
            if (connection->mode != NETPLAY_CONNECTION_PLAYING)
               return netplay_cmd_nak(netplay, connection);

            /* There is a subtlty in whether the load comes before or after the
             * current frame:
             *
             * If it comes before the current frame, then we need to force a
             * rewind to that point.
             *
             * If it comes after the current frame, we need to jump ahead, then
             * (strangely) force a rewind to the frame we're already on, so it
             * gets loaded. This is just to avoid having reloading implemented in
             * too many places. */
            if (cmd_size > netplay->zbuffer_size + 2*sizeof(uint32_t))
            {
               RARCH_ERR("CMD_LOAD_SAVESTATE received an unexpected payload size.\n");
               return netplay_cmd_nak(netplay, connection);
            }

            RECV(&frame, sizeof(frame))
            {
               RARCH_ERR("CMD_LOAD_SAVESTATE failed to receive savestate frame.\n");
               return netplay_cmd_nak(netplay, connection);
            }
            frame = ntohl(frame);

            if (frame != netplay->read_frame_count[connection->player])
            {
               RARCH_ERR("CMD_LOAD_SAVESTATE loading a state out of order!\n");
               return netplay_cmd_nak(netplay, connection);
            }

            RECV(&isize, sizeof(isize))
            {
               RARCH_ERR("CMD_LOAD_SAVESTATE failed to receive inflated size.\n");
               return netplay_cmd_nak(netplay, connection);
            }
            isize = ntohl(isize);

            if (isize != netplay->state_size)
            {
               RARCH_ERR("CMD_LOAD_SAVESTATE received an unexpected save state size.\n");
               return netplay_cmd_nak(netplay, connection);
            }

            RECV(netplay->zbuffer, cmd_size - 2*sizeof(uint32_t))
            {
               RARCH_ERR("CMD_LOAD_SAVESTATE failed to receive savestate.\n");
               return netplay_cmd_nak(netplay, connection);
            }

            /* And decompress it */
            netplay->decompression_backend->set_in(netplay->decompression_stream,
               netplay->zbuffer, cmd_size - 2*sizeof(uint32_t));
            netplay->decompression_backend->set_out(netplay->decompression_stream,
               (uint8_t*)netplay->buffer[netplay->read_ptr[connection->player]].state,
               netplay->state_size);
            netplay->decompression_backend->trans(netplay->decompression_stream,
               true, &rd, &wn, NULL);

            /* Skip ahead if it's past where we are */
            if (frame > netplay->self_frame_count)
            {
               /* This is squirrely: We need to assure that when we advance the
                * frame in post_frame, THEN we're referring to the frame to
                * load into. If we refer directly to read_ptr, then we'll end
                * up never reading the input for read_frame_count itself, which
                * will make the other side unhappy. */
               netplay->self_ptr         = PREV_PTR(netplay->read_ptr[connection->player]);
               netplay->self_frame_count = frame - 1;
            }

            /* Don't expect earlier data from other clients */
            for (player = 0; player < MAX_USERS; player++)
            {
               if (!(netplay->connected_players & (1<<player))) continue;
               if (frame > netplay->read_frame_count[player])
               {
                  netplay->read_ptr[player] = netplay->read_ptr[connection->player];
                  netplay->read_frame_count[player] = frame;
               }
            }

            /* And force rewind to it */
            netplay->force_rewind                  = true;
            netplay->savestate_request_outstanding = false;
            netplay->other_ptr                     = netplay->read_ptr[connection->player];
            netplay->other_frame_count             = frame;
            break;
         }

      case NETPLAY_CMD_PAUSE:
         connection->paused = true;
         netplay->remote_paused = true;
         netplay_send_raw_cmd_all(netplay, connection, NETPLAY_CMD_PAUSE, NULL, 0);
         break;

      case NETPLAY_CMD_RESUME:
      {
         size_t i;
         connection->paused = false;
         netplay->remote_paused = false;
         for (i = 0; i < netplay->connections_size; i++)
         {
            struct netplay_connection *sc = &netplay->connections[i];
            if (sc->active && sc->paused)
            {
               netplay->remote_paused = true;
               break;
            }
         }
         if (!netplay->remote_paused && !netplay->local_paused)
            netplay_send_raw_cmd_all(netplay, connection, NETPLAY_CMD_RESUME, NULL, 0);
         break;
      }

      default:
         RARCH_ERR("%s.\n", msg_hash_to_str(MSG_UNKNOWN_NETPLAY_COMMAND_RECEIVED));
         return netplay_cmd_nak(netplay, connection);
   }

   netplay_recv_flush(&connection->recv_packet_buffer);
   netplay->timeout_cnt = 0;
   if (had_input)
      *had_input = true;
   return true;

shrt:
   /* No more data, reset and try again */
   netplay_recv_reset(&connection->recv_packet_buffer);
   return true;

#undef RECV
}

/**
 * netplay_poll_net_input
 *
 * Poll input from the network
 */
int netplay_poll_net_input(netplay_t *netplay, bool block)
{
   bool had_input = false;
   int max_fd = 0;
   size_t i;

   for (i = 0; i < netplay->connections_size; i++)
   {
      struct netplay_connection *connection = &netplay->connections[i];
      if (connection->active && connection->fd >= max_fd)
         max_fd = connection->fd + 1;
   }

   if (max_fd == 0)
      return 0;

   do
   { 
      had_input = false;

      netplay->timeout_cnt++;

      /* Make sure we're actually ready for data */
      netplay_update_unread_ptr(netplay);
      if (!netplay_delta_frame_ready(netplay,
         &netplay->buffer[netplay->unread_ptr], netplay->unread_frame_count))
         break;
      if (!netplay->is_server &&
          !netplay_delta_frame_ready(netplay,
            &netplay->buffer[netplay->server_ptr],
            netplay->server_frame_count))
         break;

      /* Read input from each connection */
      for (i = 0; i < netplay->connections_size; i++)
      {
         struct netplay_connection *connection = &netplay->connections[i];
         if (connection->active && !netplay_get_cmd(netplay, connection, &had_input))
            netplay_hangup(netplay, connection);
      }

      if (block)
      {
         netplay_update_unread_ptr(netplay);

         /* If we were blocked for input, pass if we have this frame's input */
         if (netplay->unread_frame_count > netplay->self_frame_count)
            break;

         /* If we're supposed to block but we didn't have enough input, wait for it */
         if (!had_input)
         {
            fd_set fds;
            struct timeval tv = {0};
            tv.tv_usec = RETRY_MS * 1000;

            FD_ZERO(&fds);
            for (i = 0; i < netplay->connections_size; i++)
            {
               struct netplay_connection *connection = &netplay->connections[i];
               if (connection->active)
                  FD_SET(connection->fd, &fds);
            }

            if (socket_select(max_fd, &fds, NULL, NULL, &tv) < 0)
               return -1;

            RARCH_LOG("Network is stalling at frame %u, count %u of %d ...\n",
                  netplay->self_frame_count, netplay->timeout_cnt, MAX_RETRIES);

            if (netplay->timeout_cnt >= MAX_RETRIES && !netplay->remote_paused)
               return -1;
         }
      }
   } while (had_input || block);

   return 0;
}

/**
 * netplay_simulate_input:
 * @netplay             : pointer to netplay object
 * @sim_ptr             : frame index for which to simulate input
 * @resim               : are we resimulating, or simulating this frame for the
 *                        first time?
 *
 * "Simulate" input by assuming it hasn't changed since the last read input.
 */
void netplay_simulate_input(netplay_t *netplay, size_t sim_ptr, bool resim)
{
   uint32_t player;
   size_t prev;
   struct delta_frame *simframe, *pframe;

   simframe = &netplay->buffer[sim_ptr];

   for (player = 0; player < MAX_USERS; player++)
   {
      if (!(netplay->connected_players & (1<<player))) continue;
      if (simframe->have_real[player]) continue;

      prev = PREV_PTR(netplay->read_ptr[player]);
      pframe = &netplay->buffer[prev];

      if (resim)
      {
         /* In resimulation mode, we only copy the buttons. The reason for this
          * is nonobvious:
          *
          * If we resimulated nothing, then the /duration/ with which any input
          * was pressed would be approximately correct, since the original
          * simulation came in as the input came in, but the /number of times/
          * the input was pressed would be wrong, as there would be an
          * advancing wavefront of real data overtaking the simulated data
          * (which is really just real data offset by some frames).
          *
          * That's acceptable for arrows in most situations, since the amount
          * you move is tied to the duration, but unacceptable for buttons,
          * which will seem to jerkily be pressed numerous times with those
          * wavefronts.
          */
         const uint32_t keep = (1U<<RETRO_DEVICE_ID_JOYPAD_UP) |
                               (1U<<RETRO_DEVICE_ID_JOYPAD_DOWN) |
                               (1U<<RETRO_DEVICE_ID_JOYPAD_LEFT) |
                               (1U<<RETRO_DEVICE_ID_JOYPAD_RIGHT);
         uint32_t sim_state = simframe->simulated_input_state[player][0] & keep;
         sim_state |= pframe->real_input_state[player][0] & ~keep;
         simframe->simulated_input_state[player][0] = sim_state;
      }
      else
      {
         memcpy(simframe->simulated_input_state[player],
                pframe->real_input_state[player],
                WORDS_PER_INPUT * sizeof(uint32_t));
      }
   }
}

/**
 * netplay_flip_port
 *
 * Should we flip ports 0 and 1?
 */
bool netplay_flip_port(netplay_t *netplay)
{
   size_t frame = netplay->self_frame_count;

   if (netplay->flip_frame == 0)
      return false;

   if (netplay->is_replay)
      frame = netplay->replay_frame_count;

   return netplay->flip ^ (frame < netplay->flip_frame);
}

#ifndef HAVE_SOCKET_LEGACY
/* Custom inet_ntop. Win32 doesn't seem to support this ... */
void netplay_log_connection(const struct sockaddr_storage *their_addr,
      unsigned slot, const char *nick)
{
   union
   {
      const struct sockaddr_storage *storage;
      const struct sockaddr_in *v4;
      const struct sockaddr_in6 *v6;
   } u;
   const char *str               = NULL;
   char buf_v4[INET_ADDRSTRLEN]  = {0};
   char buf_v6[INET6_ADDRSTRLEN] = {0};
   char msg[512];

   msg[0] = '\0';

   u.storage = their_addr;

   switch (their_addr->ss_family)
   {
      case AF_INET:
         {
            struct sockaddr_in in;

            memset(&in, 0, sizeof(in));

            str           = buf_v4;
            in.sin_family = AF_INET;
            memcpy(&in.sin_addr, &u.v4->sin_addr, sizeof(struct in_addr));

            getnameinfo((struct sockaddr*)&in, sizeof(struct sockaddr_in),
                  buf_v4, sizeof(buf_v4),
                  NULL, 0, NI_NUMERICHOST);
         }
         break;
      case AF_INET6:
         {
            struct sockaddr_in6 in;
            memset(&in, 0, sizeof(in));

            str            = buf_v6;
            in.sin6_family = AF_INET6;
            memcpy(&in.sin6_addr, &u.v6->sin6_addr, sizeof(struct in6_addr));

            getnameinfo((struct sockaddr*)&in, sizeof(struct sockaddr_in6),
                  buf_v6, sizeof(buf_v6), NULL, 0, NI_NUMERICHOST);
         }
         break;
      default:
         break;
   }

   if (str)
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_GOT_CONNECTION_FROM_NAME),
            nick, str);
      runloop_msg_queue_push(msg, 1, 180, false);
      RARCH_LOG("%s\n", msg);
   }
   else
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_GOT_CONNECTION_FROM),
            nick);
      runloop_msg_queue_push(msg, 1, 180, false);
      RARCH_LOG("%s\n", msg);
   }
   RARCH_LOG("%s %u\n", msg_hash_to_str(MSG_CONNECTION_SLOT),
         slot);
}

#else
void netplay_log_connection(const struct sockaddr_storage *their_addr,
      unsigned slot, const char *nick)
{
   char msg[512];

   msg[0] = '\0';

   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_GOT_CONNECTION_FROM),
         nick);
   runloop_msg_queue_push(msg, 1, 180, false);
   RARCH_LOG("%s\n", msg);
   RARCH_LOG("%s %u\n",
         msg_hash_to_str(MSG_CONNECTION_SLOT), slot);
}

#endif

static bool netplay_init_socket_buffers(netplay_t *netplay)
{
   /* Make our packet buffer big enough for a save state and frames-many frames
    * of input data, plus the headers for each of them */
   size_t i;
   size_t packet_buffer_size = netplay->zbuffer_size +
      netplay->delay_frames * WORDS_PER_FRAME + (netplay->delay_frames+1)*3;
   netplay->packet_buffer_size = packet_buffer_size;

   for (i = 0; i < netplay->connections_size; i++)
   {
      struct netplay_connection *connection = &netplay->connections[i];
      if (connection->active)
      {
         if (connection->send_packet_buffer.data)
         {
            if (!netplay_resize_socket_buffer(&connection->send_packet_buffer,
                  packet_buffer_size) ||
                !netplay_resize_socket_buffer(&connection->recv_packet_buffer,
                  packet_buffer_size))
               return false;
         }
         else
         {
            if (!netplay_init_socket_buffer(&connection->send_packet_buffer,
                  packet_buffer_size) ||
                !netplay_init_socket_buffer(&connection->recv_packet_buffer,
                  packet_buffer_size))
               return false;
         }
      }
   }

   return true;
}

/**
 * netplay_try_init_serialization
 *
 * Try to initialize serialization. For quirky cores.
 *
 * Returns true if serialization is now ready, false otherwise.
 */
bool netplay_try_init_serialization(netplay_t *netplay)
{
   retro_ctx_serialize_info_t serial_info;
   size_t packet_buffer_size;

   if (netplay->state_size)
      return true;

   if (!netplay_init_serialization(netplay))
      return false;

   /* Check if we can actually save */
   serial_info.data_const = NULL;
   serial_info.data = netplay->buffer[netplay->self_ptr].state;
   serial_info.size = netplay->state_size;

   if (!core_serialize(&serial_info))
      return false;

   /* Once initialized, we no longer exhibit this quirk */
   netplay->quirks &= ~((uint64_t) NETPLAY_QUIRK_INITIALIZATION);

   return netplay_init_socket_buffers(netplay);
}

bool netplay_wait_and_init_serialization(netplay_t *netplay)
{
   int frame;

   if (netplay->state_size)
      return true;

   /* Wait a maximum of 60 frames */
   for (frame = 0; frame < 60; frame++) {
      if (netplay_try_init_serialization(netplay))
         return true;

#if defined(HAVE_THREADS)
      autosave_lock();
#endif
      core_run();
#if defined(HAVE_THREADS)
      autosave_unlock();
#endif
   }

   return false;
}

bool netplay_init_serialization(netplay_t *netplay)
{
   unsigned i;
   retro_ctx_size_info_t info;

   if (netplay->state_size)
      return true;

   core_serialize_size(&info);

   if (!info.size)
      return false;

   netplay->state_size = info.size;

   for (i = 0; i < netplay->buffer_size; i++)
   {
      netplay->buffer[i].state = calloc(netplay->state_size, 1);

      if (!netplay->buffer[i].state)
      {
         netplay->quirks |= NETPLAY_QUIRK_NO_SAVESTATES;
         return false;
      }
   }

   netplay->zbuffer_size = netplay->state_size * 2;
   netplay->zbuffer = (uint8_t *) calloc(netplay->zbuffer_size, 1);
   if (!netplay->zbuffer)
   {
      netplay->quirks |= NETPLAY_QUIRK_NO_TRANSMISSION;
      netplay->zbuffer_size = 0;
      return false;
   }

   return true;
}

static bool netplay_init_buffers(netplay_t *netplay, unsigned frames)
{
   size_t packet_buffer_size;

   if (!netplay)
      return false;

   /* * 2 + 1 because:
    * Self sits in the middle,
    * Other is allowed to drift as much as 'frames' frames behind
    * Read is allowed to drift as much as 'frames' frames ahead */
   netplay->buffer_size = frames * 2 + 1;

   netplay->buffer = (struct delta_frame*)calloc(netplay->buffer_size,
         sizeof(*netplay->buffer));

   if (!netplay->buffer)
      return false;

   if (!(netplay->quirks & (NETPLAY_QUIRK_NO_SAVESTATES|NETPLAY_QUIRK_INITIALIZATION)))
      netplay_init_serialization(netplay);

   return netplay_init_socket_buffers(netplay);
}

/**
 * netplay_new:
 * @direct_host          : Netplay host discovered from scanning.
 * @server               : IP address of server.
 * @port                 : Port of server.
 * @password             : Password required to connect.
 * @delay_frames         : Amount of delay frames.
 * @check_frames         : Frequency with which to check CRCs.
 * @cb                   : Libretro callbacks.
 * @nat_traversal        : If true, attempt NAT traversal.
 * @nick                 : Nickname of user.
 * @quirks               : Netplay quirks required for this session.
 *
 * Creates a new netplay handle. A NULL host means we're 
 * hosting (user 1).
 *
 * Returns: new netplay handle.
 **/
netplay_t *netplay_new(void *direct_host, const char *server, uint16_t port,
   const char *password, unsigned delay_frames, unsigned check_frames,
   const struct retro_callbacks *cb, bool nat_traversal, const char *nick,
   uint64_t quirks)
{
   netplay_t *netplay = (netplay_t*)calloc(1, sizeof(*netplay));
   if (!netplay)
      return NULL;

   netplay->listen_fd         = -1;
   netplay->tcp_port          = port;
   netplay->cbs               = *cb;
   netplay->connected_players = 0;
   netplay->is_server         = server == NULL;
   netplay->nat_traversal     = netplay->is_server ? nat_traversal : false;
   netplay->delay_frames      = delay_frames;
   netplay->check_frames      = check_frames;
   netplay->quirks            = quirks;
   netplay->self_mode         = netplay->is_server ?
                                NETPLAY_CONNECTION_PLAYING :
                                NETPLAY_CONNECTION_NONE;

   if (netplay->is_server)
   {
      netplay->connections = NULL;
      netplay->connections_size = 0;
   }
   else
   {
      netplay->connections = &netplay->one_connection;
      netplay->connections_size = 1;
      netplay->connections[0].fd = -1;
   }

   strlcpy(netplay->nick, nick[0] ? nick : RARCH_DEFAULT_NICK, sizeof(netplay->nick));
   strlcpy(netplay->password, password ? password : "", sizeof(netplay->password));

   if (!init_socket(netplay, direct_host, server, port))
   {
      free(netplay);
      return NULL;
   }

   if (!netplay_init_buffers(netplay, delay_frames))
   {
      free(netplay);
      return NULL;
   }

   if (!netplay->is_server)
   {
      netplay_handshake_init_send(netplay, &netplay->connections[0]);
      netplay->connections[0].mode = netplay->self_mode = NETPLAY_CONNECTION_INIT;
   }

   /* FIXME: Not really the right place to do this, socket initialization needs
    * to be fixed in general */
   if (netplay->is_server)
   {
      if (!socket_nonblock(netplay->listen_fd))
         goto error;
   }
   else
   {
      if (!socket_nonblock(netplay->connections[0].fd))
         goto error;
   }

   return netplay;

error:
   if (netplay->listen_fd >= 0)
      socket_close(netplay->listen_fd);

   if (netplay->connections && netplay->connections[0].fd >= 0)
      socket_close(netplay->connections[0].fd);

   free(netplay);
   return NULL;
}

/**
 * netplay_free:
 * @netplay              : pointer to netplay object
 *
 * Frees netplay handle.
 **/
void netplay_free(netplay_t *netplay)
{
   size_t i;

   if (netplay->listen_fd >= 0)
      socket_close(netplay->listen_fd);

   for (i = 0; i < netplay->connections_size; i++)
   {
      struct netplay_connection *connection = &netplay->connections[i];
      if (connection->active)
      {
         socket_close(connection->fd);
         netplay_deinit_socket_buffer(&connection->send_packet_buffer);
         netplay_deinit_socket_buffer(&connection->recv_packet_buffer);
      }
   }

   if (netplay->connections && netplay->connections != &netplay->one_connection)
      free(netplay->connections);

   if (netplay->nat_traversal)
      natt_free(&netplay->nat_traversal_state);

   if (netplay->buffer)
   {
      for (i = 0; i < netplay->buffer_size; i++)
         if (netplay->buffer[i].state)
            free(netplay->buffer[i].state);

      free(netplay->buffer);
   }

   if (netplay->zbuffer)
      free(netplay->zbuffer);

   if (netplay->compression_stream)
      netplay->compression_backend->stream_free(netplay->compression_stream);

   if (netplay->addr)
      freeaddrinfo_retro(netplay->addr);

   free(netplay);
}
