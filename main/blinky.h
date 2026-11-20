/*
  blinky.h - VSCP Blinky demo for the ESP32

  The node functionality follows the Blinky MDF
  (https://eurosource.se/blinky0.json): an LED that can be turned on/off or
  blink, a button that sends events, a millisecond counter and a four row
  decision matrix.

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#ifndef _BLINKY_H_
#define _BLINKY_H_

#include <stdint.h>

#include <vscp.h>
#include <vscp-firmware-helper.h>

// ---------------------------------------------------------------------------
//                                Registers
// ---------------------------------------------------------------------------

// User registers, page 0 ([P] = saved)
#define BLINKY_REG_ZONE               0  // Zone [P]
#define BLINKY_REG_SUBZONE            1  // Subzone [P]
#define BLINKY_REG_STATUS             2  // Status (bit 1 LED on, bit 0 button pressed)
#define BLINKY_REG_CONTROL            3  // Control [P]
#define BLINKY_REG_BLINK_INTERVAL_MSB 4  // Blink interval in ms, MSB [P]
#define BLINKY_REG_BLINK_INTERVAL_LSB 5  // Blink interval in ms, LSB [P]
#define BLINKY_REG_COUNTER_0          6  // Millisecond counter, MSB
#define BLINKY_REG_COUNTER_1          7  // Millisecond counter
#define BLINKY_REG_COUNTER_2          8  // Millisecond counter
#define BLINKY_REG_COUNTER_3          9  // Millisecond counter, LSB
#define BLINKY_REG_BUTTON_BYTE0       10 // Byte 0 of the button TURN-ON/OFF events [P]
#define BLINKY_REG_BUTTON_ZONE        11 // Zone of the button events [P]
#define BLINKY_REG_BUTTON_SUBZONE     12 // Subzone of the button events [P]
#define BLINKY_REG_DM_START           0x60 // Decision matrix, 4 rows of 8 bytes [P]

#define BLINKY_DM_ROWS     4
#define BLINKY_DM_ROW_SIZE 8

// Status register bits
#define BLINKY_STATUS_LED_ON 0x02u
#define BLINKY_STATUS_BTN_ON 0x01u

// Control register bits
#define BLINKY_CTRL_ENABLE_LED        0x80u // LED functionality (blinking, DM LED actions)
#define BLINKY_CTRL_ENABLE_BTN        0x40u // Button functionality
#define BLINKY_CTRL_ENABLE_BTN_TURNON 0x08u // TURN-ON on press, TURN-OFF on release
#define BLINKY_CTRL_ENABLE_BTN_START  0x04u // START on press
#define BLINKY_CTRL_ENABLE_BTN_STOP   0x02u // STOP on release
#define BLINKY_CTRL_ENABLE_MS_COUNTER 0x01u // Millisecond counter runs

// Alarm bits
#define BLINKY_ALARM_COUNTER_LIMIT 0x80u // The millisecond counter wrapped

// Defaults (as in the MDF)
#define BLINKY_DEFAULT_CONTROL                                                                                         \
  (BLINKY_CTRL_ENABLE_LED | BLINKY_CTRL_ENABLE_BTN | BLINKY_CTRL_ENABLE_BTN_TURNON | BLINKY_CTRL_ENABLE_BTN_START |    \
   BLINKY_CTRL_ENABLE_BTN_STOP) // 0xCE
#define BLINKY_DEFAULT_BLINK_INTERVAL 500u
#define BLINKY_DEFAULT_NICKNAME       0x10u

// Decision matrix actions
#define BLINKY_ACTION_NOOP          0 // No operation
#define BLINKY_ACTION_LED_STATE     1 // Parameter 0 = off, 1 = on, 2 = toggle
#define BLINKY_ACTION_LED_ON        2
#define BLINKY_ACTION_LED_OFF       3
#define BLINKY_ACTION_LED_TOGGLE    4
#define BLINKY_ACTION_START_BLINK   5 // Blink with the interval in registers 4-5
#define BLINKY_ACTION_STOP_BLINK    6
#define BLINKY_ACTION_CLEAR_COUNTER 7

#define BLINKY_ARG_LED_OFF    0
#define BLINKY_ARG_LED_ON     1
#define BLINKY_ARG_LED_TOGGLE 2

// Registers are saved this long after the last change
#define BLINKY_SAVE_DELAY_MS 1000u

// ---------------------------------------------------------------------------
//                                VSCP core
// ---------------------------------------------------------------------------

/*!
  Start the VSCP node: load the saved registers, set up the LED, button and
  millisecond counter and start the task that runs the Level II firmware.
  All VSCP work (protocol, decision matrix, register access) is done in that
  task; the transports give it the events they receive with vscp_core_post().

  @param guid GUID of the node
*/
void
vscp_core_start(const uint8_t *guid);

/*!
  Give an event received by a transport to the node.

  @param pev Event (allocated with vscp_fwhlp_newEvent). On success the node
             owns it and deletes it; on an error the caller still owns it.
  @return VSCP_ERROR_SUCCESS or VSCP_ERROR_FIFO_FULL
*/
int
vscp_core_post(vscp_event_t *pev);

/*!
  @return The GUID of the node (16 bytes)
*/
const uint8_t *
vscp_core_guid(void);

/*!
  Save pending register changes and restart the node (from the core task).
*/
void
vscp_core_request_restart(void);

/*!
  @return The time as a VSCP timestamp: nanoseconds since 1970 when the time
          is set (SNTP), else nanoseconds since the start.
*/
uint64_t
vscp_core_timestamp_ns(void);

// ---------------------------------------------------------------------------
//                     Transports (events from the node)
// ---------------------------------------------------------------------------

// Each one copies the event if it wants it; the caller keeps ownership.
void
link_server_send_event(const vscp_event_t *pev);
void
udp_transport_send_event(const vscp_event_t *pev);
void
mqtt_transport_send_event(const vscp_event_t *pev);

void
link_server_start(void);
void
udp_transport_start(void);
void
mqtt_transport_start(void);

#endif // _BLINKY_H_
