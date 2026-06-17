/*
 * TerraGuard AI — FRDM-MCXN947
 * VL53L5CX ULD 用プラットフォーム層（I²C）。バスは LPI2C2/FLEXCOMM2（J8 pin3/4）。
 *
 * ST 公式 ULD（vl53l5cx_api.c）から呼ばれる platform.h の6関数を NXP LPI2C で実装する:
 *   RdByte / WrByte / RdMulti / WrMulti / SwapBuffer / WaitMs
 *
 * 重要な実装方針（MLX90640 で確立した知見＋ST公式 HAL に準拠）:
 *  - VL53L5CX は 16bit レジスタアドレス・MSBファースト。
 *  - 大容量転送（FW 84KB を 0x8000 バイト単位で3回 WrMulti）は LPI2C 単発では
 *    ハングするため、CHUNK バイトずつ分割する。分割時は **書き込み/読み出しアドレスを
 *    進める**（ST公式 HAL と同じ。RegisterAddress + 既処理バイト数）。
 *  - ULD はアドレスを 8bit 表記(0x52)で持つ。LPI2C は 7bit を要求するので >>1 して渡す。
 *  - バス初期化（pin_mux + クロック + LPI2C_MasterInit）は led_blinky.c 側で実施済み。
 *
 * Copyright 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include "fsl_lpi2c.h"
#include "fsl_common.h"
#include "fsl_clock.h"
#include "app.h"
#include "platform.h"

/* 1トランザクションで扱う最大バイト数。FW転送(0x8000)はこの単位に分割される。
 * 安全側に 128 とする（LPI2C の FIFO/タイムアウト余裕を確保）。 */
#define VL53L5CX_I2C_CHUNK 128U

/* ULD の 8bit アドレス(0x52) を LPI2C の 7bit(0x29) に変換 */
static inline uint8_t vl53_addr7(const VL53L5CX_Platform *p)
{
    return (uint8_t)(p->address >> 1);
}

/* 16bit レジスタアドレス reg から 1バイト読む */
uint8_t RdByte(VL53L5CX_Platform *p_platform, uint16_t reg, uint8_t *p_value)
{
    lpi2c_master_transfer_t xfer = {0};
    xfer.flags          = kLPI2C_TransferDefaultFlag;
    xfer.slaveAddress   = vl53_addr7(p_platform);
    xfer.direction      = kLPI2C_Read;
    xfer.subaddress     = reg;
    xfer.subaddressSize = 2U;
    xfer.data           = p_value;
    xfer.dataSize       = 1U;

    return (LPI2C_MasterTransferBlocking(EXAMPLE_I2C_MASTER, &xfer) == kStatus_Success) ? 0U : 255U;
}

/* 16bit レジスタアドレス reg へ 1バイト書く */
uint8_t WrByte(VL53L5CX_Platform *p_platform, uint16_t reg, uint8_t value)
{
    lpi2c_master_transfer_t xfer = {0};
    xfer.flags          = kLPI2C_TransferDefaultFlag;
    xfer.slaveAddress   = vl53_addr7(p_platform);
    xfer.direction      = kLPI2C_Write;
    xfer.subaddress     = reg;
    xfer.subaddressSize = 2U;
    xfer.data           = &value;
    xfer.dataSize       = 1U;

    return (LPI2C_MasterTransferBlocking(EXAMPLE_I2C_MASTER, &xfer) == kStatus_Success) ? 0U : 255U;
}

/* 16bit レジスタアドレス reg から size バイト読む（CHUNK 分割、アドレスを進める） */
uint8_t RdMulti(VL53L5CX_Platform *p_platform, uint16_t reg, uint8_t *p_values, uint32_t size)
{
    uint8_t addr7 = vl53_addr7(p_platform);
    uint32_t done = 0;

    while (done < size)
    {
        uint32_t chunk = ((size - done) > VL53L5CX_I2C_CHUNK) ? VL53L5CX_I2C_CHUNK : (size - done);

        lpi2c_master_transfer_t xfer = {0};
        xfer.flags          = kLPI2C_TransferDefaultFlag;
        xfer.slaveAddress   = addr7;
        xfer.direction      = kLPI2C_Read;
        xfer.subaddress     = (uint32_t)(reg + done); /* チャンクごとにアドレスを進める */
        xfer.subaddressSize = 2U;
        xfer.data           = &p_values[done];
        xfer.dataSize       = chunk;

        if (LPI2C_MasterTransferBlocking(EXAMPLE_I2C_MASTER, &xfer) != kStatus_Success)
        {
            return 255U;
        }
        done += chunk;
    }
    return 0U;
}

/* 16bit レジスタアドレス reg へ size バイト書く（CHUNK 分割、アドレスを進める）。
 * FW転送(0x8000バイト×3)の肝。LPI2C の subaddress(2B) + データチャンクを1トランザクションで送る。 */
uint8_t WrMulti(VL53L5CX_Platform *p_platform, uint16_t reg, uint8_t *p_values, uint32_t size)
{
    uint8_t addr7 = vl53_addr7(p_platform);
    uint32_t done = 0;

    while (done < size)
    {
        uint32_t chunk = ((size - done) > VL53L5CX_I2C_CHUNK) ? VL53L5CX_I2C_CHUNK : (size - done);

        lpi2c_master_transfer_t xfer = {0};
        xfer.flags          = kLPI2C_TransferDefaultFlag;
        xfer.slaveAddress   = addr7;
        xfer.direction      = kLPI2C_Write;
        xfer.subaddress     = (uint32_t)(reg + done); /* チャンクごとにアドレスを進める */
        xfer.subaddressSize = 2U;
        xfer.data           = &p_values[done];
        xfer.dataSize       = chunk;

        if (LPI2C_MasterTransferBlocking(EXAMPLE_I2C_MASTER, &xfer) != kStatus_Success)
        {
            return 255U;
        }
        done += chunk;
    }
    return 0U;
}

/* uint32 単位のバイトスワップ（buffer の size は常に4の倍数）。
 * MCXN947 はリトルエンディアンだが、ULD はセンサのビッグエンディアン語を
 * ホスト語に直すためこの関数を必ず使う（ST公式実装と同じ）。 */
void SwapBuffer(uint8_t *buffer, uint16_t size)
{
    for (uint16_t i = 0; i < size; i += 4)
    {
        uint32_t tmp = ((uint32_t)buffer[i] << 24)
                     | ((uint32_t)buffer[i + 1] << 16)
                     | ((uint32_t)buffer[i + 2] << 8)
                     | ((uint32_t)buffer[i + 3]);
        *(uint32_t *)(&buffer[i]) = tmp;
    }
}

/* time_ms ミリ秒待つ */
uint8_t WaitMs(VL53L5CX_Platform *p_platform, uint32_t time_ms)
{
    (void)p_platform;
    SDK_DelayAtLeastUs((uint64_t)time_ms * 1000U, CLOCK_GetCoreSysClkFreq());
    return 0U;
}
