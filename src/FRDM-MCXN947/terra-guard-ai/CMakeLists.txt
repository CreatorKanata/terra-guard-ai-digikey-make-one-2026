
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

# MLX90640 サーマルセンサ: Melexis 公式 API(Apache-2.0) + 自前 LPI2C ドライバ層(BSD-3)
mcux_add_source(
    SOURCES vendor/mlx90640/MLX90640_API.c
            vendor/mlx90640/MLX90640_API.h
            vendor/mlx90640/MLX90640_I2C_Driver.h
            vendor/mlx90640/mlx90640_i2c_lpi2c.c
)

mcux_add_include(
    INCLUDES led_blinky
             vendor/mlx90640
)
mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)

mcux_add_include(INCLUDES .)

# MLX90640_API.c は ExtractParameters 内で float[768](3KB) のローカル配列を複数使う。
# デフォルトスタック 0x800(2KB) では溢れて HardFault するため 0x4000(16KB) に拡張する。
mcux_add_linker_symbol(SYMBOLS __stack_size__=0x4000)
