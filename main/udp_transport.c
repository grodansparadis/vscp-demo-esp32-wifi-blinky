/*
  udp_transport.c - VSCP frames over UDP and the VSCP multicast group

  Every datagram is one VSCP frame (version 0, not encrypted):
  packet type, header, timestamp, date/time, class, type, GUID, data size,
  data and CRC. Received frames are given to the node; events from the node
  are sent as frames to the UDP remote host (broadcast by default) and to
  the multicast group.

  https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_over_udp
  https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_multicast

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <errno.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <lwip/sockets.h>
#include <sdkconfig.h>

#include "vscp-compiler.h"
#include "vscp-projdefs.h"

#include <vscp-firmware-helper.h>
#include <vscp.h>

#include "blinky.h"

#if defined(CONFIG_BLINKY_UDP_ENABLE) || defined(CONFIG_BLINKY_MULTICAST_ENABLE)

static const char *TAG = "udp";

#define UDP_QUEUE_SIZE 16
#define UDP_MAX_FRAME  (1 + 35 + VSCP_MAX_DATA + 2) // Type, header, data, CRC
#define UDP_POLL_MS    10

static QueueHandle_t s_txq; // Copies of events from the node

typedef struct {
  const char *name;
  int sock;
  struct sockaddr_in to; // Events from the node are sent here
} channel_t;

static channel_t s_udp   = { .name = "UDP", .sock = -1 };
static channel_t s_mcast = { .name = "multicast", .sock = -1 };

static int
open_socket(uint16_t port)
{
  struct sockaddr_in addr = {
    .sin_family      = AF_INET,
    .sin_port        = htons(port),
    .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    return -1;
  }
  int on = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (0 != bind(sock, (struct sockaddr *) &addr, sizeof(addr))) {
    close(sock);
    return -1;
  }
  return sock;
}

// A frame from the network: give the event to the node
static void
receive(channel_t *ch)
{
  static uint8_t buf[UDP_MAX_FRAME + 64];
  struct sockaddr_in from;
  socklen_t fromlen = sizeof(from);
  int len           = recvfrom(ch->sock, buf, sizeof(buf), 0, (struct sockaddr *) &from, &fromlen);
  if (len <= 0) {
    return;
  }

  // The low nibble of the packet type is the encryption (not supported)
  if (buf[0] & 0x0f) {
    ESP_LOGW(TAG, "%s: encrypted frame from %s ignored", ch->name, inet_ntoa(from.sin_addr));
    return;
  }

  vscp_event_t *pev = vscp_fwhlp_newEvent();
  if (NULL == pev) {
    return;
  }
  int rv = vscp_fwhlp_getEventFromFrame(pev, buf, len);
  if (VSCP_ERROR_SUCCESS != rv) {
    ESP_LOGW(TAG, "%s: bad frame from %s (%d)", ch->name, inet_ntoa(from.sin_addr), rv);
    vscp_fwhlp_deleteEvent(&pev);
    return;
  }

  // Our own events (broadcast/multicast that comes back)
  if (0 == memcmp(pev->GUID, vscp_core_guid(), 16)) {
    vscp_fwhlp_deleteEvent(&pev);
    return;
  }

  ESP_LOGD(TAG, "%s: event %d/%d from %s", ch->name, pev->vscp_class, pev->vscp_type, inet_ntoa(from.sin_addr));
  if (VSCP_ERROR_SUCCESS != vscp_core_post(pev)) {
    vscp_fwhlp_deleteEvent(&pev);
  }
}

static void
transmit(const vscp_event_t *pev)
{
  static uint8_t frame[UDP_MAX_FRAME];
  size_t size = vscp_fwhlp_getFrameSizeFromEvent((vscp_event_t *) pev);
  if ((0 == size) || (size > sizeof(frame)) ||
      (VSCP_ERROR_SUCCESS != vscp_fwhlp_writeEventToFrame(frame, sizeof(frame), 0, pev))) {
    ESP_LOGW(TAG, "Could not make a frame for event %d/%d", pev->vscp_class, pev->vscp_type);
    return;
  }
  channel_t *channels[] = { &s_udp, &s_mcast };
  for (int i = 0; i < 2; i++) {
    if (channels[i]->sock >= 0) {
      if (sendto(channels[i]->sock, frame, size, 0, (struct sockaddr *) &channels[i]->to, sizeof(channels[i]->to)) < 0) {
        ESP_LOGD(TAG, "%s: send failed (errno %d)", channels[i]->name, errno);
      }
    }
  }
}

static void
udp_task(void *arg)
{
#ifdef CONFIG_BLINKY_UDP_ENABLE
  if ((s_udp.sock = open_socket(CONFIG_BLINKY_UDP_PORT)) < 0) {
    ESP_LOGE(TAG, "UDP: could not open port %d", CONFIG_BLINKY_UDP_PORT);
  }
  else {
    int on = 1;
    setsockopt(s_udp.sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    s_udp.to.sin_family      = AF_INET;
    s_udp.to.sin_port        = htons(CONFIG_BLINKY_UDP_PORT);
    s_udp.to.sin_addr.s_addr = inet_addr(CONFIG_BLINKY_UDP_REMOTE_HOST);
    ESP_LOGI(TAG, "UDP: port %d, events to %s", CONFIG_BLINKY_UDP_PORT, CONFIG_BLINKY_UDP_REMOTE_HOST);
  }
#endif

#ifdef CONFIG_BLINKY_MULTICAST_ENABLE
  if ((s_mcast.sock = open_socket(CONFIG_BLINKY_MULTICAST_PORT)) < 0) {
    ESP_LOGE(TAG, "Multicast: could not open port %d", CONFIG_BLINKY_MULTICAST_PORT);
  }
  else {
    struct ip_mreq mreq = { 0 };
    mreq.imr_multiaddr.s_addr = inet_addr(CONFIG_BLINKY_MULTICAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (0 != setsockopt(s_mcast.sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq))) {
      ESP_LOGE(TAG, "Multicast: could not join %s (errno %d)", CONFIG_BLINKY_MULTICAST_GROUP, errno);
    }
    uint8_t ttl  = CONFIG_BLINKY_MULTICAST_TTL;
    uint8_t loop = 0;
    setsockopt(s_mcast.sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(s_mcast.sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    s_mcast.to.sin_family      = AF_INET;
    s_mcast.to.sin_port        = htons(CONFIG_BLINKY_MULTICAST_PORT);
    s_mcast.to.sin_addr.s_addr = mreq.imr_multiaddr.s_addr;
    ESP_LOGI(TAG, "Multicast: group %s port %d", CONFIG_BLINKY_MULTICAST_GROUP, CONFIG_BLINKY_MULTICAST_PORT);
  }
#endif

  for (;;) {
    fd_set fds;
    FD_ZERO(&fds);
    int maxfd = -1;
    if (s_udp.sock >= 0) {
      FD_SET(s_udp.sock, &fds);
      maxfd = s_udp.sock;
    }
    if (s_mcast.sock >= 0) {
      FD_SET(s_mcast.sock, &fds);
      maxfd = (s_mcast.sock > maxfd) ? s_mcast.sock : maxfd;
    }
    if (maxfd < 0) {
      break;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = UDP_POLL_MS * 1000 };
    if (select(maxfd + 1, &fds, NULL, NULL, &tv) > 0) {
      if ((s_udp.sock >= 0) && FD_ISSET(s_udp.sock, &fds)) {
        receive(&s_udp);
      }
      if ((s_mcast.sock >= 0) && FD_ISSET(s_mcast.sock, &fds)) {
        receive(&s_mcast);
      }
    }

    // Events from the node
    vscp_event_t *pev;
    while (pdTRUE == xQueueReceive(s_txq, &pev, 0)) {
      transmit(pev);
      vscp_fwhlp_deleteEvent(&pev);
    }
  }

  ESP_LOGE(TAG, "No UDP sockets, the UDP task ends");
  vTaskDelete(NULL);
}

void
udp_transport_send_event(const vscp_event_t *pev)
{
  if (NULL == s_txq) {
    return;
  }
  vscp_event_t *pnew = vscp_fwhlp_mkEventCopy(pev);
  if ((NULL != pnew) && (pdTRUE != xQueueSend(s_txq, &pnew, 0))) {
    vscp_fwhlp_deleteEvent(&pnew);
  }
}

void
udp_transport_start(void)
{
  s_txq = xQueueCreate(UDP_QUEUE_SIZE, sizeof(vscp_event_t *));
  xTaskCreate(udp_task, "vscp-udp", 4096, NULL, 4, NULL);
}

#else

void
udp_transport_send_event(const vscp_event_t *pev)
{
}

void
udp_transport_start(void)
{
}

#endif
