
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

# センサアプリケーション層（センサごとに分割）
#   sensor_bus       : I3C/P3T1755/LPI2C 共通基盤・バススキャン・温度出力
#   thermal_mlx90640 : MLX90640 サーマルセンサ
#   tof_vl53l5cx     : VL53L5CX ToF距離センサ
#   bg_subtract      : 背景差分（背景モデル+前景抽出+候補判定）
mcux_add_source(
    SOURCES app/sensor_bus.c
            app/sensor_bus.h
            app/thermal_mlx90640.c
            app/thermal_mlx90640.h
            app/tof_vl53l5cx.c
            app/tof_vl53l5cx.h
            app/bg_subtract.c
            app/bg_subtract.h
            app/npu_infer.c
            app/npu_infer.h
)

# MLX90640 サーマルセンサ: Melexis 公式 API(Apache-2.0) + 自前 LPI2C ドライバ層(BSD-3)
mcux_add_source(
    SOURCES vendor/mlx90640/MLX90640_API.c
            vendor/mlx90640/MLX90640_API.h
            vendor/mlx90640/MLX90640_I2C_Driver.h
            vendor/mlx90640/mlx90640_i2c_lpi2c.c
)

# VL53L5CX ToF距離センサ: ST 公式 ULD(BSD-3) + 自前 LPI2C プラットフォーム層(BSD-3)
mcux_add_source(
    SOURCES vendor/vl53l5cx/vl53l5cx_api.c
            vendor/vl53l5cx/vl53l5cx_api.h
            vendor/vl53l5cx/vl53l5cx_buffers.h
            vendor/vl53l5cx/platform.h
            vendor/vl53l5cx/vl53l5cx_platform_lpi2c.c
)

# eIQ Neutron NPU 推論（TensorFlow Lite Micro）。
#   tflm/model.cpp       : MODEL_Init / RunInference / 入出力テンソル取得（SDK共通を移植）
#   tflm/model/model.h   : 推論API宣言
#   tflm/pcq_npu/        : 学習済みカラスモデル(model_data.h) + op resolver(3op)
# ※ output_postproc.cpp / get_top_n.cpp は cifar10 固有（labels.h/chgui.h 依存）。
#   2クラス判定は本体側で独自に行うため取り込まない。
mcux_add_source(
    SOURCES tflm/model.cpp
            tflm/model/model.h
            tflm/pcq_npu/model_data.h
            tflm/pcq_npu/model_cifarnet_ops_npu.cpp
)

mcux_add_include(
    INCLUDES led_blinky
             app
             vendor/mlx90640
             vendor/vl53l5cx
             tflm/model
             tflm/pcq_npu
)
mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)

mcux_add_include(INCLUDES .)

# --- NPU モデル配置のためのリンカ差し替え ---
# model_data.h は `__attribute__((section(".model")))` で flash 配置を要求するが、
# デフォルトリンカ(MCXN947_cm33_core0_flash.ld)には .model セクション定義が無く、
# 先頭(m_interrupts)に落ちてあふれる。tflm_label_image の専用リンカは .model を
# m_flash1(0x00100000) に置く定義を持つので、これに差し替える（cifar10 と同手法）。
mcux_remove_armgcc_linker_script(
    BASE_PATH ${SdkRootDirPath}
    LINKER ${device_root}/MCX/MCXN/MCXN947/gcc/MCXN947_cm33_core0_flash.ld
)
mcux_add_armgcc_linker_script(
    BASE_PATH ${SdkRootDirPath}
    LINKER ${board_root}/${board}/eiq_examples/tflm_label_image/cm33_core0/gcc/${CONFIG_MCUX_TOOLCHAIN_LINKER_DEVICE_PREFIX}_flash.ld
)

# TFLite Micro / Neutron(CMSIS-DSP) が要求するマクロ。
# ※ cifar10 は SDK_DEBUGCONSOLE_UART も付けるが、既存のシリアル出力(PUTCHARバイナリ
#   送出)挙動を変えないため、ここでは ARM_MATH/FPU のみとする。
mcux_add_macro(
    CC "-DARM_MATH_CM33 -D__FPU_PRESENT=1"
    CX "-DARM_MATH_CM33 -D__FPU_PRESENT=1"
)

# MLX90640_API.c は ExtractParameters 内で float[768](3KB) のローカル配列を複数使う。
# デフォルトスタック 0x800(2KB) では溢れて HardFault するため 0x4000(16KB) に拡張する。
mcux_add_linker_symbol(SYMBOLS __stack_size__=0x4000)
