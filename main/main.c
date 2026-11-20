/*
  main.c - VSCP Blinky demo for the ESP32 (Wi-Fi)

  Starts Wi-Fi, the VSCP node (blinky.c) and the transports: VSCP link
  protocol server (TCP/IP), UDP, multicast and MQTT.

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include "blinky.h"

static const char *TAG = "main";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_events;

static void
wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
  if ((WIFI_EVENT == base) && (WIFI_EVENT_STA_START == id)) {
    esp_wifi_connect();
  }
  else if ((WIFI_EVENT == base) && (WIFI_EVENT_STA_DISCONNECTED == id)) {
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    ESP_LOGW(TAG, "Wi-Fi disconnected, connecting again");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_connect();
  }
  else if ((IP_EVENT == base) && (IP_EVENT_STA_GOT_IP == id)) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;
    ESP_LOGI(TAG, "Got IP address " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
  }
}

static void
wifi_start(void)
{
  s_wifi_events = xEventGroupCreate();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

  wifi_config_t wifi_config = { 0 };
  strncpy((char *) wifi_config.sta.ssid, CONFIG_BLINKY_WIFI_SSID, sizeof(wifi_config.sta.ssid));
  strncpy((char *) wifi_config.sta.password, CONFIG_BLINKY_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
  wifi_config.sta.threshold.authmode = CONFIG_BLINKY_WIFI_PASSWORD[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  // No power save: multicast and broadcast frames are not missed, faster replies
  esp_wifi_set_ps(WIFI_PS_NONE);
}

void
app_main(void)
{
  esp_err_t rv = nvs_flash_init();
  if ((ESP_ERR_NVS_NO_FREE_PAGES == rv) || (ESP_ERR_NVS_NEW_VERSION_FOUND == rv)) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    rv = nvs_flash_init();
  }
  ESP_ERROR_CHECK(rv);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // GUID from the Wi-Fi MAC address: FF:FF:FF:FF:FF:FF:FF:FE:<MAC>:00:00
  // https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_globally_unique_identifiers
  uint8_t guid[16];
  uint8_t mac[6];
  memset(guid, 0xff, 7);
  guid[7] = 0xfe;
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
  memcpy(guid + 8, mac, 6);
  guid[14] = 0;
  guid[15] = 0;

  // The node works (LED, button) also without a network
  vscp_core_start(guid);

  if (0 == strlen(CONFIG_BLINKY_WIFI_SSID)) {
    ESP_LOGE(TAG, "No Wi-Fi SSID set. Set it in sdkconfig.defaults.local or with idf.py menuconfig.");
    return;
  }

  wifi_start();
  ESP_LOGI(TAG, "Connecting to %s", CONFIG_BLINKY_WIFI_SSID);
  xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

#ifdef CONFIG_BLINKY_SNTP_ENABLE
  esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_BLINKY_SNTP_SERVER);
  esp_netif_sntp_init(&sntp);
#endif

  link_server_start();
  udp_transport_start();
  mqtt_transport_start();
}
