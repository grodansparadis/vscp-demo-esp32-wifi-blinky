set(VSCP_SRC "${VSCP_FIRMWARE_DIR}/src")

idf_component_register(
    SRCS "main.c"
         "blinky.c"
         "link_server.c"
         "udp_transport.c"
         "mqtt_transport.c"
         "${VSCP_SRC}/vscp-fifo.c"
         "${VSCP_SRC}/vscp-crc.c"
         "${VSCP_SRC}/vscp-firmware-helper.c"
         "${VSCP_SRC}/vscp-firmware-level2.c"
         "${VSCP_SRC}/vscp-link-protocol.c"
    INCLUDE_DIRS "." "${VSCP_SRC}"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_gpio esp_timer lwip
    PRIV_REQUIRES)

# The vscp-firmware sources are not written for -Werror with all IDF warnings
set_source_files_properties(
    "${VSCP_SRC}/vscp-fifo.c" "${VSCP_SRC}/vscp-crc.c" "${VSCP_SRC}/vscp-firmware-helper.c"
    "${VSCP_SRC}/vscp-firmware-level2.c" "${VSCP_SRC}/vscp-link-protocol.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-error;-Wno-format;-Wno-unused-variable;-Wno-unused-function")
