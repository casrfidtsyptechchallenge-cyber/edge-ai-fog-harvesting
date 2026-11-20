# AeroIon-Edge M3 test bench for the NXP FRDM-MCXA156 (MCUXpresso SDK, "freestanding" project).
# Board setup (pins, clocks, debug console) is borrowed from the SDK's hello_world for this board;
# the safety core and the message code come unchanged from the ESP32 firmware in ../node.
cmake_minimum_required(VERSION 3.22.0)

include(${SdkRootDirPath}/cmake/extension/mcux.cmake)
project(mcx_bench LANGUAGES C CXX ASM PROJECT_BOARD_PORT_PATH ${board_root}/${board}/demo_apps/hello_world)
include(${SdkRootDirPath}/CMakeLists.txt)

set(AEROION_NODE ${CMAKE_CURRENT_LIST_DIR}/../node)
if(NOT EXISTS ${AEROION_NODE}/lib/safety/safety_core.cpp)
  message(FATAL_ERROR "firmware/node not found next to this project (expected ${AEROION_NODE})")
endif()

mcux_add_source(
    BASE_PATH ${CMAKE_CURRENT_LIST_DIR}
    SOURCES source/main.cpp
            source/bench_board.c
            source/bench_console.cpp
)
mcux_add_source(
    BASE_PATH ${AEROION_NODE}
    SOURCES lib/safety/fog_detector.cpp
            lib/safety/safety_core.cpp
            lib/telemetry/telemetry_json.cpp
)
mcux_add_include(
    BASE_PATH ${CMAKE_CURRENT_LIST_DIR}
    INCLUDES source
             third_party
)
mcux_add_include(
    BASE_PATH ${AEROION_NODE}
    INCLUDES include
             lib/safety
             lib/telemetry
             lib/sensing
)

string(TIMESTAMP AEROION_BUILD_EPOCH_S "%s" UTC)  # message timestamps start from the build time
mcux_add_macro("ARDUINOJSON_USE_LONG_LONG=1 ARDUINOJSON_ENABLE_STD_STREAM=0 ARDUINOJSON_ENABLE_STD_STRING=0 AEROION_BUILD_EPOCH_S=${AEROION_BUILD_EPOCH_S}")

# The default 1 KB heap is too small for the JSON messages.
mcux_add_linker_symbol(SYMBOLS "__heap_size__=0x4000 __stack_size__=0x2000")

mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)