# AeroIon-Edge node firmware for the NXP MCX A156, the node's safety controller (FRDM-MCXA156 board,
# MCUXpresso SDK "freestanding" project). Board setup comes from the SDK's hello_world for this board, the
# clock / USB serial / LED / watchdog glue from ../mcx_bench, and all the logic from the shared code in ../node/lib.
cmake_minimum_required(VERSION 3.22.0)

include(${SdkRootDirPath}/cmake/extension/mcux.cmake)
project(mcx_node LANGUAGES C CXX ASM PROJECT_BOARD_PORT_PATH ${board_root}/${board}/demo_apps/hello_world)
include(${SdkRootDirPath}/CMakeLists.txt)

set(AEROION_NODE ${CMAKE_CURRENT_LIST_DIR}/../node)
set(AEROION_BENCH ${CMAKE_CURRENT_LIST_DIR}/../mcx_bench)
if(NOT EXISTS ${AEROION_NODE}/lib/drivers/board_io.h)
  message(FATAL_ERROR "firmware/node/lib/drivers not found (expected ${AEROION_NODE}/lib/drivers)")
endif()
if(NOT EXISTS ${AEROION_BENCH}/source/bench_board.c)
  message(FATAL_ERROR "firmware/mcx_bench not found next to this project (expected ${AEROION_BENCH})")
endif()

mcux_add_source(
    BASE_PATH ${CMAKE_CURRENT_LIST_DIR}
    SOURCES source/main.cpp
            source/mcx_io.c
            source/mcx_uart.c
            source/node_console.cpp
            source/node_sensors.cpp
)
mcux_add_source(
    BASE_PATH ${AEROION_BENCH}
    SOURCES source/bench_board.c
)
mcux_add_source(
    BASE_PATH ${AEROION_NODE}
    SOURCES lib/drivers/collector_current_driver.cpp
            lib/drivers/ina219_driver.cpp
            lib/drivers/optical_fog_driver.cpp
            lib/drivers/switch_drivers.cpp
            lib/edge_ai/arc_guard.cpp
            lib/edge_ai/arc_novelty.cpp
            lib/edge_ai/arc_predictor.cpp
            lib/link/command_handler.cpp
            lib/link/node_link.cpp
            lib/link/unix_clock.cpp
            lib/rfid/allowlist_sync.cpp
            lib/rfid/badge_list.cpp
            lib/rfid/mfrc522.cpp
            lib/rfid/rc522_reader.cpp
            lib/sensing/sensor_registry.cpp
            lib/safety/fog_detector.cpp
            lib/safety/safety_core.cpp
            lib/telemetry/telemetry_json.cpp
)
mcux_add_include(
    BASE_PATH ${CMAKE_CURRENT_LIST_DIR}
    INCLUDES source
)
mcux_add_include(
    BASE_PATH ${AEROION_BENCH}
    INCLUDES source
             third_party
)
mcux_add_include(
    BASE_PATH ${AEROION_NODE}
    INCLUDES include
             lib/drivers
             lib/edge_ai
             lib/link
             lib/rfid
             lib/safety
             lib/sensing
             lib/telemetry
)

string(TIMESTAMP AEROION_BUILD_EPOCH_S "%s" UTC)  # message timestamps start from the build time
# I2C_RETRY_TIMES: an I2C transfer gives up instead of waiting forever if the bus is stuck.
mcux_add_macro("ARDUINOJSON_USE_LONG_LONG=1 ARDUINOJSON_ENABLE_STD_STREAM=0 ARDUINOJSON_ENABLE_STD_STRING=0 AEROION_BUILD_EPOCH_S=${AEROION_BUILD_EPOCH_S} I2C_RETRY_TIMES=20000U")

# The default 1 KB heap is too small for the JSON messages; a 64-badge SYNC_ALLOWLIST needs the most.
mcux_add_linker_symbol(SYMBOLS "__heap_size__=0x6000 __stack_size__=0x2000")

mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)