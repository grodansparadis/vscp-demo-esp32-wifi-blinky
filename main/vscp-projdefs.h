/*
  vscp-projdefs.h - Project definitions for the vscp-firmware code
  (firmware helper, VSCP link protocol and Level II firmware).
*/
#ifndef _VSCP_PROJDEFS_H_
#define _VSCP_PROJDEFS_H_

#include <esp_log.h>

// Log output from the vscp-firmware code (debug level)
#define LOGSTR(...) ESP_LOGD("vscp", __VA_ARGS__)

// Firmware helper features
#define VSCP_FWHLP_BINARY_FRAME_SUPPORT // UDP/multicast frames
#define VSCP_FWHLP_JSON_SUPPORT         // MQTT (VSCP JSON events, cJSON)

// Level II firmware
#define THIS_FIRMWARE_DEVICE_NAME     "VSCP ESP32 Wi-Fi blinky demo"
#define THIS_FIRMWARE_MAJOR_VERSION   (0u)
#define THIS_FIRMWARE_MINOR_VERSION   (1u)
#define THIS_FIRMWARE_RELEASE_VERSION (0u)
#define THIS_FIRMWARE_BUILD_VERSION   (0u)

#define THIS_FIRMWARE_ENABLE_ERROR_REPORTING

// VSCP link protocol
#define THIS_FIRMWARE_TCPIP_LINK_MAX_BUFFER         2048u
#define THIS_FIRMWARE_TCPIP_LINK_ENABLE_RCVLOOP_CMD 1

#endif // _VSCP_PROJDEFS_H_
