/*
  mqtt_transport.c - VSCP JSON events over MQTT

  Events from the node are published, one VSCP JSON event per message, on
  the publish topic (default vscp/{guid}/{class}/{type}). VSCP JSON events
  published on the subscribe topic (default vscp/{guid}/in) are given to the
  node.

  https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_over_mqtt

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <sdkconfig.h>

#include "vscp-compiler.h"
#include "vscp-projdefs.h"

#include <vscp-firmware-helper.h>
#include <vscp.h>

#include "blinky.h"

#ifdef CONFIG_BLINKY_MQTT_ENABLE

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <mqtt_client.h>

static const char *TAG = "mqtt";

#define MQTT_JSON_MAX  2048 // A VSCP JSON event with 512 data bytes fits
#define MQTT_TOPIC_MAX 128

static esp_mqtt_client_handle_t s_client;
static volatile int s_connected;
static char s_guid[48];                   // GUID as a string, FF:FF:...
static char s_sub_topic[MQTT_TOPIC_MAX]; // Subscribe topic
static char *s_rx;                        // Received message (may come in parts)
static QueueHandle_t s_txq;               // Copies of events from the node

// Make a topic from a template: {guid}, {class} and {type} are replaced
static void
make_topic(char *topic, size_t size, const char *tmpl, const vscp_event_t *pev)
{
  size_t pos = 0;
  topic[0]   = '\0';
  while (*tmpl && (pos < size - 1)) {
    const char *value = NULL;
    char num[8];
    if (0 == strncmp(tmpl, "{guid}", 6)) {
      value = s_guid;
      tmpl += 6;
    }
    else if ((NULL != pev) && (0 == strncmp(tmpl, "{class}", 7))) {
      snprintf(num, sizeof(num), "%u", pev->vscp_class);
      value = num;
      tmpl += 7;
    }
    else if ((NULL != pev) && (0 == strncmp(tmpl, "{type}", 6))) {
      snprintf(num, sizeof(num), "%u", pev->vscp_type);
      value = num;
      tmpl += 6;
    }
    if (NULL != value) {
      pos += snprintf(topic + pos, size - pos, "%s", value);
      if (pos >= size) {
        pos = size - 1;
      }
    }
    else {
      topic[pos++] = *tmpl++;
      topic[pos]   = '\0';
    }
  }
}

// A VSCP JSON event from the broker: give it to the node
static void
handle_message(const char *json)
{
  vscp_event_t *pev = vscp_fwhlp_newEvent();
  if (NULL == pev) {
    return;
  }
  int rv = vscp_fwhlp_parse_json(pev, json);
  if (VSCP_ERROR_SUCCESS != rv) {
    ESP_LOGW(TAG, "Not a VSCP JSON event (%d): %.80s", rv, json);
    vscp_fwhlp_deleteEvent(&pev);
    return;
  }
  ESP_LOGD(TAG, "Event %d/%d", pev->vscp_class, pev->vscp_type);
  if (VSCP_ERROR_SUCCESS != vscp_core_post(pev)) {
    vscp_fwhlp_deleteEvent(&pev);
  }
}

static void
mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;

  switch ((esp_mqtt_event_id_t) event_id) {

    case MQTT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "Connected to %s, subscribing to %s", CONFIG_BLINKY_MQTT_URI, s_sub_topic);
      esp_mqtt_client_subscribe(s_client, s_sub_topic, 0);
      s_connected = 1;
      break;

    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "Disconnected");
      s_connected = 0;
      break;

    case MQTT_EVENT_DATA:
      // A long message comes in parts
      if ((event->total_data_len >= MQTT_JSON_MAX) ||
          (event->current_data_offset + event->data_len > event->total_data_len)) {
        ESP_LOGW(TAG, "Message too long (%d bytes)", event->total_data_len);
        break;
      }
      memcpy(s_rx + event->current_data_offset, event->data, event->data_len);
      if (event->current_data_offset + event->data_len == event->total_data_len) {
        s_rx[event->total_data_len] = '\0';
        handle_message(s_rx);
      }
      break;

    case MQTT_EVENT_ERROR:
      ESP_LOGW(TAG, "Error");
      break;

    default:
      break;
  }
}

// Publish an event (MQTT send task)
static void
publish(const vscp_event_t *pev, char *json)
{
  char topic[MQTT_TOPIC_MAX];
  if (VSCP_ERROR_SUCCESS != vscp_fwhlp_create_json(json, MQTT_JSON_MAX, pev)) {
    ESP_LOGW(TAG, "Could not make JSON for event %d/%d", pev->vscp_class, pev->vscp_type);
    return;
  }
  // One line: the helper pretty prints (no string in the event has tabs or newlines)
  char *w = json;
  for (char *r = json; *r; r++) {
    if (('\n' != *r) && ('\t' != *r) && ('\r' != *r)) {
      *w++ = *r;
    }
  }
  *w = '\0';
  make_topic(topic, sizeof(topic), CONFIG_BLINKY_MQTT_PUBLISH_TOPIC, pev);
  if (esp_mqtt_client_publish(s_client, topic, json, 0, 0, 0) < 0) {
    ESP_LOGW(TAG, "Could not publish event %d/%d", pev->vscp_class, pev->vscp_type);
  }
}

/*
  Events are published from this task: esp_mqtt_client_publish() sends at
  once (esp_mqtt_client_enqueue() waits for the next poll of the MQTT task,
  up to a second) and the node is not held up by the network.
*/
static void
send_task(void *arg)
{
  char *json = malloc(MQTT_JSON_MAX);
  for (;;) {
    vscp_event_t *pev;
    if (pdTRUE == xQueueReceive(s_txq, &pev, portMAX_DELAY)) {
      if (s_connected && (NULL != json)) {
        publish(pev, json);
      }
      vscp_fwhlp_deleteEvent(&pev);
    }
  }
}

void
mqtt_transport_send_event(const vscp_event_t *pev)
{
  if (!s_connected || (NULL == s_txq)) {
    return;
  }
  vscp_event_t *pnew = vscp_fwhlp_mkEventCopy(pev);
  if ((NULL != pnew) && (pdTRUE != xQueueSend(s_txq, &pnew, 0))) {
    vscp_fwhlp_deleteEvent(&pnew);
  }
}

void
mqtt_transport_start(void)
{
  const uint8_t *guid = vscp_core_guid();
  char client_id[40];

  s_rx = malloc(MQTT_JSON_MAX);
  if (NULL == s_rx) {
    return;
  }

  snprintf(s_guid, sizeof(s_guid), "%02X", guid[0]);
  for (int i = 1; i < 16; i++) {
    snprintf(s_guid + strlen(s_guid), sizeof(s_guid) - strlen(s_guid), ":%02X", guid[i]);
  }
  make_topic(s_sub_topic, sizeof(s_sub_topic), CONFIG_BLINKY_MQTT_SUBSCRIBE_TOPIC, NULL);
  snprintf(client_id,
           sizeof(client_id),
           "vscp-blinky-%02x%02x%02x%02x%02x%02x",
           guid[8],
           guid[9],
           guid[10],
           guid[11],
           guid[12],
           guid[13]);

  esp_mqtt_client_config_t cfg = {
    .broker.address.uri                  = CONFIG_BLINKY_MQTT_URI,
    .credentials.client_id               = client_id,
    .credentials.username                = (CONFIG_BLINKY_MQTT_USER[0] ? CONFIG_BLINKY_MQTT_USER : NULL),
    .credentials.authentication.password = (CONFIG_BLINKY_MQTT_PASSWORD[0] ? CONFIG_BLINKY_MQTT_PASSWORD : NULL),
    .session.keepalive                   = 60,
    .buffer.size                         = MQTT_JSON_MAX,
  };
  s_client = esp_mqtt_client_init(&cfg);
  if (NULL == s_client) {
    ESP_LOGE(TAG, "Could not start the MQTT client");
    return;
  }
  s_txq = xQueueCreate(16, sizeof(vscp_event_t *));
  xTaskCreate(send_task, "vscp-mqtt-tx", 4096, NULL, 4, NULL);
  esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
  esp_mqtt_client_start(s_client);
}

#else

void
mqtt_transport_send_event(const vscp_event_t *pev)
{
}

void
mqtt_transport_start(void)
{
}

#endif
