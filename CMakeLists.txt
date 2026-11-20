# VSCP Blinky demo for the ESP32 (Wi-Fi)
cmake_minimum_required(VERSION 3.16)

# vscp-firmware sources, fetched at configure time.
# Use -DFETCHCONTENT_SOURCE_DIR_VSCP_FIRMWARE=<path> for a local checkout.
include(FetchContent)
FetchContent_Declare(
    vscp_firmware
    URL https://codeload.github.com/grodansparadis/vscp-firmware/tar.gz/f8bc40d0585df638ef7f742ff25106920acc3277
    URL_HASH SHA256=d33c3c64f02b4877fd9753cad2834d206028cc3ebdd2afa00621dd21d85de94b
    SOURCE_SUBDIR __sources_only__
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(vscp_firmware)
set(VSCP_FIRMWARE_DIR "${vscp_firmware_SOURCE_DIR}" CACHE INTERNAL "vscp-firmware sources")

# Local settings that are not committed (Wi-Fi credentials, broker, ...)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vscp-blinky)
