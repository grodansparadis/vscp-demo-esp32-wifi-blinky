# Changes in vscp-demo-esp32-wifi-blinky

Every change that goes into a release is listed here, newest release first.
Add each change under **Unreleased** when it is made. When a release is
tagged, rename **Unreleased** to the version and date, and start a new
**Unreleased** section above it.

## Unreleased

- First version: the VSCP Blinky node (LED, button, millisecond counter, four row decision matrix, registers saved in NVS) on an ESP32 with Wi-Fi
- Transports, all at the same time: VSCP link protocol server (TCP/IP, several clients), VSCP frames over UDP and on the VSCP multicast group, VSCP JSON events over MQTT
- GUID from the Wi-Fi MAC address, event time from SNTP
- `tools/blinky_test.py` tests a node over each transport
- vscp-firmware 58195ab: MQTT events are read with the short keys (`class`, `type`, `data`, ...) or the `vscpClass`, `vscpType`, `vscpData`, ... keys, `vscpObId` included
