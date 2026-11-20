# vscp-demo-esp32-wifi-blinky

The [VSCP](https://www.vscp.org) Blinky demo node on an ESP32 with Wi-Fi. It has an LED that can be turned on and off or blink, a button that sends events, a millisecond counter and a decision matrix, as described in the [Blinky documentation](https://github.com/grodansparadis/vscp/wiki/Blinky-Documentation) and the [Blinky MDF](https://eurosource.se/blinky0.json).

The node can be reached in four ways at the same time:

| Transport | Default | VSCP |
| --- | --- | --- |
| VSCP link protocol (TCP/IP) | Port 9598, user `vscp`, password `secret`, up to 3 clients | [Text protocol](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_tcpiplink): login, `send`, `retr`, `rcvloop`, filters, ... |
| UDP | Port 33333, events sent to 255.255.255.255 (broadcast) | [VSCP frames](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_over_udp), one per datagram, not encrypted |
| Multicast | Group 224.0.23.158, port 44444 | [VSCP frames](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_multicast) on the VSCP multicast group |
| MQTT | Off; broker set in the configuration | [VSCP JSON events](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_over_mqtt), published on `vscp/{guid}/{class}/{type}`, received on `vscp/{guid}/in` |

Events received over any of them are handled by the node (register read/write and the other protocol events) and run through the decision matrix. Events from the node are sent over all of them.

## Hardware

Any ESP32 board. Most ESP32 DevKit boards have a blue LED on GPIO2 and the BOOT button on GPIO0, which are the defaults. Other pins are set with `idf.py menuconfig` (VSCP Blinky → Board). The button pulls the GPIO low when it is pressed.

## Build and flash

Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) 6.0 or later. The vscp-firmware code is fetched when the project is configured, and the MQTT client and cJSON come from the ESP component registry.

```bash
git clone https://github.com/grodansparadis/vscp-demo-esp32-wifi-blinky.git
cd vscp-demo-esp32-wifi-blinky
cp sdkconfig.defaults.local.example sdkconfig.defaults.local   # then set the Wi-Fi SSID and password
idf.py set-target esp32
idf.py build flash monitor
```

`sdkconfig.defaults.local` is not committed, so the Wi-Fi password stays on your computer. All settings are also in `idf.py menuconfig` under **VSCP Blinky**: Wi-Fi, pins, link protocol port/user/password/number of clients, UDP port and remote host, multicast group/port/TTL, MQTT broker/user/password/topics and SNTP server. When you change `sdkconfig.defaults.local`, delete `sdkconfig` so the new values are used.

The monitor shows the IP address the node gets:

```
I (1890) main: Got IP address 192.168.1.50
I (1900) link: VSCP link protocol server on port 9598
I (1900) udp: UDP: port 33333, events to 255.255.255.255
I (1900) udp: Multicast: group 224.0.23.158 port 44444
```

## The node

### GUID

The GUID is made from the Wi-Fi MAC address: `FF:FF:FF:FF:FF:FF:FF:FE:<MAC>:00:00` ([Ethernet/MAC address based GUID](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_globally_unique_identifiers)). Get it with `ggid` over the link protocol.

### Registers

| Register (page 0) | Use |
| --- | --- |
| 0, 1 | Zone and subzone of the node [saved] |
| 2 | Status: bit 1 LED on, bit 0 button pressed. Writing bit 1 turns the LED on/off (when the LED is enabled) |
| 3 | Control [saved, default 0xCE]: bit 7 LED enabled, bit 6 button enabled, bit 3 send TURN-ON on press and TURN-OFF on release, bit 2 send START on press, bit 1 send STOP on release, bit 0 millisecond counter running |
| 4, 5 | Blink interval in ms, MSB first [saved, default 500]. Zero stops blinking |
| 6-9 | Millisecond counter, MSB first. Writing any of them clears it. When it wraps, alarm bit 7 is set |
| 10-12 | Byte 0, zone and subzone of the button events [saved] |
| 0x60-0x7F | Decision matrix: four Level I rows of eight bytes (origin, flags, class mask, class filter, type mask, type filter, action, parameter) [saved] |

The standard registers are at 0xFFFFFF80 (Level II). The node is a Level II node, but it uses Level I decision matrix rows, as in the MDF, and it also answers CLASS1.PROTOCOL register read/write for nickname 0x10 (the default nickname, saved when it is changed).

Saved registers are written to NVS one second after the last change, so a burst of writes is one save.

### Decision matrix actions

| Action | |
| --- | --- |
| 0 | No operation |
| 1 | LED off/on/toggle (parameter 0/1/2) |
| 2 | LED on |
| 3 | LED off |
| 4 | Toggle the LED |
| 5 | Start blinking (interval in registers 4-5) |
| 6 | Stop blinking |
| 7 | Clear the millisecond counter |

The LED actions work when control bit 7 is set. On, off and toggle stop blinking.

### Events from the node

- CLASS1.INFORMATION ON/OFF (20/3, 20/4) when the LED is turned on or off by an action or a status register write (not for each blink). Data: index 0, zone, subzone
- CLASS1.CONTROL TURN-ON (30/5) when the button is pressed and TURN-OFF (30/6) when it is released (control bit 3). Data: registers 10-12
- CLASS1.INFORMATION START (20/25) on press and STOP (20/24) on release (control bits 2 and 1). Data: 0, button zone, button subzone
- CLASS2.INFORMATION node heartbeat (1026/2) and CLASS2.PROTOCOL capabilities (1024/20) every minute
- Responses to the protocol events (register read/write, who is there, ...)

The time in the events is the Unix time in nanoseconds from SNTP, or the time since the start until the time is set.

## Try it

### VSCP link protocol

```
$ telnet 192.168.1.50 9598
Welcome to the VSCP ESP32 Wi-Fi Blinky demo node
...
+OK
user vscp
+OK - User name accepted, password please
pass secret
+OK - Ready to work.
send 0,0,9,0,,0,-,0x10,3
+OK - Success.
retr
0,0,10,0,...,FF:FF:FF:FF:FF:FF:FF:FE:24:0A:C4:12:34:56:00:00,3,206
+OK - Success.
```

`send 0,0,9,...,0x10,3` reads register 3 (control) of node 0x10; the answer is CLASS1.PROTOCOL read/write response (0/10) with the register and its value (206 = 0xCE). `rcvloop` sends every event from the node as it comes. [VSCP Works](https://github.com/grodansparadis/vscp-works-qt) can be used with a tcp/ip connection to the node, and the MDF lets it show and set the registers by name.

### MQTT

Enable MQTT and set the broker. Each event is published as one line of JSON, as made by the vscp-firmware helper:

```json
{"head":352,"class":0,"type":10,"obid":0,"timestamp_ns":"0x18e9c5ece59f9580","guid":"FF:FF:FF:FF:FF:FF:FF:FE:24:0A:C4:12:34:56:00:00","datetime":"2026-11-20T14:01:09","data":[3,206]}
```

Events sent to the node can use these keys or the `vscpClass`, `vscpType`, `vscpData`, ... keys.

```
CONFIG_BLINKY_MQTT_ENABLE=y
CONFIG_BLINKY_MQTT_URI="mqtt://192.168.1.9:1883"
```

```bash
mosquitto_sub -h 192.168.1.9 -t 'vscp/#' -v
mosquitto_pub -h 192.168.1.9 -t 'vscp/FF:FF:FF:FF:FF:FF:FF:FE:24:0A:C4:12:34:56:00:00/in' \
  -m '{"vscpHead":0,"vscpClass":0,"vscpType":9,"vscpData":[16,3]}'   # read register 3
```

### Test script

[tools/blinky_test.py](tools/blinky_test.py) tests a node over each transport (only the Python standard library; MQTT uses `mosquitto_pub`/`mosquitto_sub`):

```bash
tools/blinky_test.py --host 192.168.1.50 link
tools/blinky_test.py --host 192.168.1.50 udp
tools/blinky_test.py multicast
tools/blinky_test.py --host 192.168.1.50 --broker 192.168.1.9 mqtt
tools/blinky_test.py --host 192.168.1.50 --broker 192.168.1.9 all
```

## Layout

| | |
| --- | --- |
| `main/main.c` | Wi-Fi, GUID, SNTP, starts the node and the transports |
| `main/blinky.c` | The node: Level II firmware callbacks, registers, LED, button, counter, decision matrix actions. All VSCP work is done in one task |
| `main/link_server.c` | VSCP link protocol server |
| `main/udp_transport.c` | UDP and multicast |
| `main/mqtt_transport.c` | MQTT |
| `main/Kconfig.projbuild` | Settings (menuconfig) |
| `tools/blinky_test.py` | Test script |

The VSCP code is from [vscp-firmware](https://github.com/grodansparadis/vscp-firmware) (helper, link protocol and Level II firmware), fetched by CMake at the commit in [CMakeLists.txt](CMakeLists.txt). Use `idf.py -DFETCHCONTENT_SOURCE_DIR_VSCP_FIRMWARE=<path> build` to build with a local checkout.

## Other Blinky demos

See the [Blinky page](https://github.com/grodansparadis/vscp/wiki/Blinky) for the other implementations, for example [vscp-demo-link-stm32f401-wiz-ip20-blinky](https://github.com/grodansparadis/vscp-demo-link-stm32f401-wiz-ip20-blinky).

## License

MIT, see [LICENSE](LICENSE).
