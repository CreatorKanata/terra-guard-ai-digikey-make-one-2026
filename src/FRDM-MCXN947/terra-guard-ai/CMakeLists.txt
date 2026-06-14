
# Copyright 2026 NXP
#
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.22.0)

cmake_path(APPEND conf_file_path ${CMAKE_CURRENT_LIST_DIR} ${board}_${core_id} prj.conf)
set(CONF_FILE ${conf_file_path} ${CONF_FILE})
include(${SdkRootDirPath}/cmake/extension/mcux.cmake)

project(terra-guard-ai LANGUAGES C CXX ASM PROJECT_BOARD_PORT_PATH ${board_root}/${board}/demo_apps/led_blinky)


include("${board}_${core_id}/board_files.cmake")
include(${SdkRootDirPath}/CMakeLists.txt)


mcux_add_source(
    SOURCES led_blinky.c
)

mcux_add_source(
    SOURCES led_blinky/peripherals.c
            led_blinky/peripherals.h
)

mcux_add_include(
    INCLUDES led_blinky
)
mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)

mcux_add_include(INCLUDES .)
