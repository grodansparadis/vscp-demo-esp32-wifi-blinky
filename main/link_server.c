/*
  link_server.c - VSCP link protocol server (TCP/IP)

  Each client gets a task and a link protocol context. Events the client
  sends are given to the node (vscp_core_post); events from the node are put
  in the out queue of every logged in client whose filter lets them through,
  and the client gets them with 'retr' or in a 'rcvloop'.

  https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_tcpiplink

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <errno.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <lwip/sockets.h>
#include <sdkconfig.h>

#include "vscp-compiler.h"
#include "vscp-projdefs.h"

#include <vscp-fifo.h>
#include <vscp-firmware-helper.h>
#include <vscp-link-protocol.h>
#include <vscp.h>

#include "blinky.h"

#ifdef CONFIG_BLINKY_LINK_ENABLE

static const char *TAG = "link";

#define LINK_FIFO_SIZE     32
#define LINK_TASK_STACK    8192
#define LINK_TASK_PRIORITY 4
#define LINK_RECV_TIMEOUT  20   // ms; the receive loop also runs this often
#define LINK_RCVLOOP_OK_MS 1000 // '+OK' this often in a receive loop without events

#define LINK_WELCOME                                                                                                   \
  "Welcome to the VSCP ESP32 Wi-Fi Blinky demo node\r\n"                                                               \
  "Copyright (C) 2026 Ake Hedman, the VSCP project, https://www.vscp.org\r\n"                                         \
  "+OK\r\n"

#define LINK_MSG_MAX_CLIENTS "-OK - Max number of clients reached. Disconnecting.\r\n"

typedef struct {
  vscp_link_ctx_t link; // Link protocol context
  int used;             // The slot has a client
  int close;            // Close the connection
  SemaphoreHandle_t lock; // For the out queue (the core task writes it)
} client_t;

static client_t s_clients[CONFIG_BLINKY_LINK_MAX_CLIENTS];
static SemaphoreHandle_t s_lock; // For the used flags

static uint32_t
millis(void)
{
  return (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// Delete every event in a client's out queue
static void
drain(client_t *c)
{
  vscp_event_t *pev;
  xSemaphoreTake(c->lock, portMAX_DELAY);
  while (vscp_fifo_read(&c->link.fifoEventsOut, &pev)) {
    vscp_fwhlp_deleteEvent(&pev);
  }
  xSemaphoreGive(c->lock);
}

// ****************************************************************************
//                           Link protocol callbacks
// ****************************************************************************

static int
cb_write_client(vscp_link_ctx_t *pctx, const char *msg)
{
  size_t len = strlen(msg);
  while (len) {
    int n = send(pctx->sock, msg, len, 0);
    if (n < 0) {
      if ((EAGAIN == errno) || (EWOULDBLOCK == errno)) {
        vTaskDelay(1);
        continue;
      }
      ((client_t *) pctx->user_data)->close = 1;
      return VSCP_ERROR_WRITE_ERROR;
    }
    msg += n;
    len -= n;
  }
  return VSCP_ERROR_SUCCESS;
}

static int
cb_disconnect(vscp_link_ctx_t *pctx)
{
  ((client_t *) pctx->user_data)->close = 1;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_quit(vscp_link_ctx_t *pctx)
{
  cb_write_client(pctx, VSCP_LINK_MSG_GOODBYE);
  ((client_t *) pctx->user_data)->close = 1;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_welcome(vscp_link_ctx_t *pctx)
{
  return cb_write_client(pctx, LINK_WELCOME);
}

static int
cb_check_user(vscp_link_ctx_t *pctx, const char *arg)
{
  while (*arg && (' ' == *arg)) {
    arg++;
  }
  strncpy(pctx->user, arg, sizeof(pctx->user) - 1);
  pctx->user[sizeof(pctx->user) - 1] = '\0';
  return cb_write_client(pctx, VSCP_LINK_MSG_USENAME_OK);
}

static int
cb_check_password(vscp_link_ctx_t *pctx, const char *arg)
{
  if ('\0' == pctx->user[0]) {
    cb_write_client(pctx, VSCP_LINK_MSG_NEED_USERNAME);
    return VSCP_ERROR_CREDENTIALS;
  }
  while (*arg && (' ' == *arg)) {
    arg++;
  }
  if ((0 == strcmp(pctx->user, CONFIG_BLINKY_LINK_USER)) && (0 == strcmp(arg, CONFIG_BLINKY_LINK_PASSWORD))) {
    pctx->bValidated = 1;
    pctx->privLevel  = 15;
    ESP_LOGI(TAG, "Client %d: logged in as %s", pctx->id, pctx->user);
    return cb_write_client(pctx, VSCP_LINK_MSG_PASSWORD_OK);
  }
  pctx->user[0]    = '\0';
  pctx->bValidated = 0;
  pctx->privLevel  = 0;
  ESP_LOGW(TAG, "Client %d: wrong user or password", pctx->id);
  cb_write_client(pctx, VSCP_LINK_MSG_PASSWORD_ERROR);
  return VSCP_ERROR_CREDENTIALS;
}

static int
cb_check_authenticated(vscp_link_ctx_t *pctx)
{
  return pctx->bValidated ? VSCP_ERROR_SUCCESS : VSCP_ERROR_INVALID_PERMISSION;
}

static int
cb_check_privilege(vscp_link_ctx_t *pctx, uint8_t priv)
{
  return (pctx->privLevel >= priv) ? VSCP_ERROR_SUCCESS : VSCP_ERROR_INVALID_PERMISSION;
}

// An event from the client: give it to the node (which owns it on success)
static int
cb_send(vscp_link_ctx_t *pctx, vscp_event_t *pev)
{
  uint16_t size = pev->sizeData;
  if (VSCP_ERROR_SUCCESS != vscp_core_post(pev)) {
    pctx->statistics.cntOverruns++;
    return VSCP_ERROR_TRM_FULL; // The library deletes the event
  }
  pctx->statistics.cntReceiveFrames++;
  pctx->statistics.cntReceiveData += size;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_chkdata(vscp_link_ctx_t *pctx, uint16_t *pcount)
{
  client_t *c = (client_t *) pctx->user_data;
  xSemaphoreTake(c->lock, portMAX_DELAY);
  *pcount = LINK_FIFO_SIZE - vscp_fifo_getFree(&pctx->fifoEventsOut);
  xSemaphoreGive(c->lock);
  return VSCP_ERROR_SUCCESS;
}

static int
cb_clrall(vscp_link_ctx_t *pctx)
{
  drain((client_t *) pctx->user_data);
  return VSCP_ERROR_SUCCESS;
}

static int
cb_retr(vscp_link_ctx_t *pctx, vscp_event_t **ppev)
{
  client_t *c = (client_t *) pctx->user_data;
  xSemaphoreTake(c->lock, portMAX_DELAY);
  int ok = vscp_fifo_read(&pctx->fifoEventsOut, ppev);
  xSemaphoreGive(c->lock);
  if (!ok || (NULL == *ppev)) {
    return VSCP_ERROR_RCV_EMPTY;
  }
  pctx->statistics.cntTransmitFrames++;
  pctx->statistics.cntTransmitData += (*ppev)->sizeData;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_rcvloop(vscp_link_ctx_t *pctx, vscp_event_t **ppev)
{
  int rv = cb_retr(pctx, ppev);
  if (VSCP_ERROR_SUCCESS == rv) {
    pctx->last_rcvloop_time = millis();
    return rv;
  }
  // '+OK' every second when there are no events
  if ((millis() - pctx->last_rcvloop_time) >= LINK_RCVLOOP_OK_MS) {
    pctx->last_rcvloop_time = millis();
    return VSCP_ERROR_TIMEOUT;
  }
  return rv;
}

static int
cb_enable_rcvloop(vscp_link_ctx_t *pctx, int enable)
{
  pctx->bRcvLoop          = enable ? 1 : 0;
  pctx->last_rcvloop_time = millis();
  return VSCP_ERROR_SUCCESS;
}

static int
cb_get_rcvloop_status(vscp_link_ctx_t *pctx, int *pstatus)
{
  *pstatus = pctx->bRcvLoop;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_get_guid(vscp_link_ctx_t *pctx, uint8_t *pguid)
{
  memcpy(pguid, pctx->guid, 16);
  return VSCP_ERROR_SUCCESS;
}

static int
cb_set_guid(vscp_link_ctx_t *pctx, uint8_t *pguid)
{
  memcpy(pctx->guid, pguid, 16); // GUID of this channel
  return VSCP_ERROR_SUCCESS;
}

static int
cb_get_version(vscp_link_ctx_t *pctx, uint8_t *pversion)
{
  pversion[0] = THIS_FIRMWARE_MAJOR_VERSION;
  pversion[1] = THIS_FIRMWARE_MINOR_VERSION;
  pversion[2] = THIS_FIRMWARE_RELEASE_VERSION;
  pversion[3] = THIS_FIRMWARE_BUILD_VERSION;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_get_channel_id(vscp_link_ctx_t *pctx, uint16_t *pchid)
{
  *pchid = (uint16_t) pctx->id;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_wcyd(vscp_link_ctx_t *pctx, uint64_t *pwcyd)
{
  *pwcyd = VSCP_SERVER_CAPABILITY_TCPIP | VSCP_SERVER_CAPABILITY_DECISION_MATRIX | VSCP_SERVER_CAPABILITY_IP4;
#ifdef CONFIG_BLINKY_UDP_ENABLE
  *pwcyd |= VSCP_SERVER_CAPABILITY_UDP;
#endif
#ifdef CONFIG_BLINKY_MULTICAST_ENABLE
  *pwcyd |= VSCP_SERVER_CAPABILITY_MULTICAST_CHANNEL;
#endif
  if (CONFIG_BLINKY_LINK_MAX_CLIENTS > 1) {
    *pwcyd |= VSCP_SERVER_CAPABILITY_TWO_CONNECTIONS;
  }
  return VSCP_ERROR_SUCCESS;
}

static int
cb_statistics(vscp_link_ctx_t *pctx, vscp_statistics_t *ps)
{
  memcpy(ps, &pctx->statistics, sizeof(vscp_statistics_t));
  return VSCP_ERROR_SUCCESS;
}

static int
cb_info(vscp_link_ctx_t *pctx, vscp_status_t *ps)
{
  memcpy(ps, &pctx->status, sizeof(vscp_status_t));
  return VSCP_ERROR_SUCCESS;
}

static int
cb_set_filter(vscp_link_ctx_t *pctx, vscpEventFilter *pf)
{
  client_t *c = (client_t *) pctx->user_data;
  xSemaphoreTake(c->lock, portMAX_DELAY);
  pctx->filter.filter_class    = pf->filter_class;
  pctx->filter.filter_type     = pf->filter_type;
  pctx->filter.filter_priority = pf->filter_priority;
  memcpy(pctx->filter.filter_GUID, pf->filter_GUID, 16);
  xSemaphoreGive(c->lock);
  return VSCP_ERROR_SUCCESS;
}

static int
cb_set_mask(vscp_link_ctx_t *pctx, vscpEventFilter *pf)
{
  client_t *c = (client_t *) pctx->user_data;
  xSemaphoreTake(c->lock, portMAX_DELAY);
  pctx->filter.mask_class    = pf->mask_class;
  pctx->filter.mask_type     = pf->mask_type;
  pctx->filter.mask_priority = pf->mask_priority;
  memcpy(pctx->filter.mask_GUID, pf->mask_GUID, 16);
  xSemaphoreGive(c->lock);
  return VSCP_ERROR_SUCCESS;
}

static uint16_t
cb_get_interface_count(vscp_link_ctx_t *pctx)
{
  return 1;
}

static int
cb_get_interface(vscp_link_ctx_t *pctx, uint16_t idx, vscp_interface_info_t *pif)
{
  if (0 != idx) {
    return VSCP_ERROR_UNKNOWN_ITEM;
  }
  pif->idx  = idx;
  pif->type = VSCP_INTERFACE_TYPE_LEVEL2DRV;
  memcpy(pif->guid, vscp_core_guid(), 16);
  strncpy(pif->description, "Blinky node", sizeof(pif->description) - 1);
  return VSCP_ERROR_SUCCESS;
}

static int
cb_close_interface(vscp_link_ctx_t *pctx, uint8_t *pguid)
{
  return VSCP_ERROR_NOT_SUPPORTED;
}

static int
cb_shutdown(vscp_link_ctx_t *pctx)
{
  return VSCP_ERROR_NOT_SUPPORTED;
}

static int
cb_restart(vscp_link_ctx_t *pctx)
{
  cb_write_client(pctx, VSCP_LINK_MSG_OK);
  vscp_core_request_restart(); // Saves pending changes first
  return VSCP_ERROR_SUCCESS;
}

static const vscp_link_ops_t s_link_ops = {
  .write_client        = cb_write_client,
  .disconnect          = cb_disconnect,
  .quit                = cb_quit,
  .welcome             = cb_welcome,
  .check_user          = cb_check_user,
  .check_password      = cb_check_password,
  .check_authenticated = cb_check_authenticated,
  .check_privilege     = cb_check_privilege,
  .send                = cb_send,
  .chkdata             = cb_chkdata,
  .clrall              = cb_clrall,
  .retr                = cb_retr,
  .rcvloop             = cb_rcvloop,
  .enable_rcvloop      = cb_enable_rcvloop,
  .get_rcvloop_status  = cb_get_rcvloop_status,
  .get_guid            = cb_get_guid,
  .set_guid            = cb_set_guid,
  .get_version         = cb_get_version,
  .get_channel_id      = cb_get_channel_id,
  .wcyd                = cb_wcyd,
  .statistics          = cb_statistics,
  .info                = cb_info,
  .set_filter          = cb_set_filter,
  .set_mask            = cb_set_mask,
  .get_interface_count = cb_get_interface_count,
  .get_interface       = cb_get_interface,
  .close_interface     = cb_close_interface,
  .shutdown            = cb_shutdown,
  .restart             = cb_restart,
};

// ****************************************************************************
//                                   Tasks
// ****************************************************************************

static void
client_task(void *arg)
{
  client_t *c   = (client_t *) arg;
  char *buf     = malloc(THIS_FIRMWARE_TCPIP_LINK_MAX_BUFFER);
  size_t filled = 0;

  struct timeval tv = { .tv_sec = 0, .tv_usec = LINK_RECV_TIMEOUT * 1000 };
  setsockopt(c->link.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int on = 1;
  setsockopt(c->link.sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  setsockopt(c->link.sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

  if (NULL != buf) {
    vscp_link_connect(&c->link); // Welcome
  }

  while ((NULL != buf) && !c->close) {
    int n = recv(c->link.sock, buf + filled, THIS_FIRMWARE_TCPIP_LINK_MAX_BUFFER - 1 - filled, 0);
    if (0 == n) {
      break; // Closed by the client
    }
    if (n < 0) {
      if ((EAGAIN != errno) && (EWOULDBLOCK != errno)) {
        break;
      }
      n = 0;
    }
    filled += n;
    buf[filled] = '\0';

    // Handle every complete line
    char *eol;
    while (!c->close && (NULL != (eol = strchr(buf, '\n')))) {
      *eol        = '\0';
      size_t used = eol - buf + 1;
      vscp_link_parser(&c->link, buf);
      memmove(buf, buf + used, filled - used + 1);
      filled -= used;
    }
    if (filled >= THIS_FIRMWARE_TCPIP_LINK_MAX_BUFFER - 1) {
      filled = 0; // Too long line
      cb_write_client(&c->link, VSCP_LINK_MSG_BUFFER_FULL);
    }

    // Events to a client in a receive loop
    vscp_link_idle_worker(&c->link);
  }

  ESP_LOGI(TAG, "Client %d: disconnected", c->link.id);
  free(buf);
  shutdown(c->link.sock, SHUT_RDWR);
  close(c->link.sock);

  xSemaphoreTake(s_lock, portMAX_DELAY);
  c->link.bValidated = 0; // No more events to this client
  xSemaphoreGive(s_lock);
  drain(c);
  vscp_fifo_deinit(&c->link.fifoEventsOut);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  c->used = 0;
  xSemaphoreGive(s_lock);

  vTaskDelete(NULL);
}

static void
server_task(void *arg)
{
  static int next_id = 1;
  struct sockaddr_in addr = {
    .sin_family      = AF_INET,
    .sin_port        = htons(CONFIG_BLINKY_LINK_PORT),
    .sin_addr.s_addr = htonl(INADDR_ANY),
  };

  int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int on  = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if ((srv < 0) || (0 != bind(srv, (struct sockaddr *) &addr, sizeof(addr))) || (0 != listen(srv, 2))) {
    ESP_LOGE(TAG, "Failed to listen on port %d (errno %d)", CONFIG_BLINKY_LINK_PORT, errno);
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(TAG, "VSCP link protocol server on port %d", CONFIG_BLINKY_LINK_PORT);

  for (;;) {
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    int sock      = accept(srv, (struct sockaddr *) &peer, &len);
    if (sock < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    client_t *c = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONFIG_BLINKY_LINK_MAX_CLIENTS; i++) {
      if (!s_clients[i].used) {
        c       = &s_clients[i];
        c->used = 1;
        break;
      }
    }
    xSemaphoreGive(s_lock);

    if (NULL == c) {
      send(sock, LINK_MSG_MAX_CLIENTS, strlen(LINK_MSG_MAX_CLIENTS), 0);
      close(sock);
      continue;
    }

    ESP_LOGI(TAG, "Client %d: connected from %s", next_id, inet_ntoa(peer.sin_addr));

    SemaphoreHandle_t lock = c->lock;
    memset(&c->link, 0, sizeof(c->link));
    c->lock           = lock;
    c->close          = 0;
    c->link.ops       = &s_link_ops;
    c->link.id        = next_id++;
    c->link.sock      = sock;
    c->link.user_data = c;
    memcpy(c->link.guid, vscp_core_guid(), 16);
    vscp_fifo_init(&c->link.fifoEventsOut, LINK_FIFO_SIZE);

    if (pdPASS != xTaskCreate(client_task, "vscp-link", LINK_TASK_STACK, c, LINK_TASK_PRIORITY, NULL)) {
      ESP_LOGE(TAG, "No memory for a client task");
      vscp_fifo_deinit(&c->link.fifoEventsOut);
      close(sock);
      c->used = 0;
    }
  }
}

// ****************************************************************************
//                                Public API
// ****************************************************************************

void
link_server_send_event(const vscp_event_t *pev)
{
  if (NULL == s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < CONFIG_BLINKY_LINK_MAX_CLIENTS; i++) {
    client_t *c = &s_clients[i];
    if (!c->used || !c->link.bValidated) {
      continue;
    }
    xSemaphoreTake(c->lock, portMAX_DELAY);
    if (vscp_fwhlp_doLevel2Filter(pev, &c->link.filter)) {
      vscp_event_t *pnew = vscp_fwhlp_mkEventCopy(pev);
      if ((NULL == pnew) || !vscp_fifo_write(&c->link.fifoEventsOut, pnew)) {
        c->link.statistics.cntOverruns++;
        vscp_fwhlp_deleteEvent(&pnew);
      }
    }
    xSemaphoreGive(c->lock);
  }
  xSemaphoreGive(s_lock);
}

void
link_server_start(void)
{
  s_lock = xSemaphoreCreateMutex();
  for (int i = 0; i < CONFIG_BLINKY_LINK_MAX_CLIENTS; i++) {
    s_clients[i].lock = xSemaphoreCreateMutex();
  }
  xTaskCreate(server_task, "vscp-link-srv", 4096, NULL, LINK_TASK_PRIORITY, NULL);
}

#else // CONFIG_BLINKY_LINK_ENABLE

void
link_server_send_event(const vscp_event_t *pev)
{
}

void
link_server_start(void)
{
}

#endif
