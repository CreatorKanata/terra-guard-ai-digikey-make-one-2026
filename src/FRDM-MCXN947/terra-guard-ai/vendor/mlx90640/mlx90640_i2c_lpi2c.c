/*
 * TerraGuard AI — FRDM-MCXN947
 * MLX90640 用 I²C ドライバ層（Melexis 公式 API のプラットフォーム依存部）。
 * バスは LPI2C2 / FLEXCOMM2（J8 pin3=SCL,4=SDA / P4_0/P4_1）。
 *
 * Melexis 公式 MLX90640_API.c から呼ばれる以下の関数を、NXP LPI2C ドライバで実装する:
 *   MLX90640_I2CInit / MLX90640_I2CGeneralReset / MLX90640_I2CRead /
 *   MLX90640_I2CWrite / MLX90640_I2CFreqSet
 *
 * MLX90640 は 16bit レジスタアドレス・16bit データ・MSB ファースト。
 * バス初期化（pin_mux + クロック + LPI2C_MasterInit）は led_blinky.c 側で実施済みのため、
 * 本ファイルの MLX90640_I2CInit は何もしない。
 *
 * Copyright 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include "fsl_lpi2c.h"
#include "app.h"
#include <MLX90640_API.h>
#include <MLX90640_I2C_Driver.h>

/* 公式 API の戻り値規約（MLX90640_API.h）:
 *   0  = MLX90640_NO_ERROR
 *   負 = エラー（NACK / 書き込み失敗）。API.c は -MLX90640_I2C_NACK_ERROR 等を比較に使う。 */

/* 連続リードは CHUNK ワードずつ分割して読む。各チャンクは16bitアドレスを
 * 指定し直して読むため、MLX90640 のメモリは連続アドレスとして正しく読める。
 *
 * チャンク拡大(32→256)を実測したが取得時間はほぼ不変だった。100kHz では
 * subaddress/Start/Stop のオーバーヘッドより I²C のデータ転送時間そのもの
 * (1サブページ ~1800B ≈ 160ms) が支配的で、チャンク数は律速でないため。
 * 実績のある 32 ワードに戻す（小さい static バッファで安定）。 */
#define MLX90640_READ_CHUNK_WORDS 32U
static uint8_t s_rxBuf[MLX90640_READ_CHUNK_WORDS * 2U];

/* バス初期化は led_blinky.c の i2c_master_init() で完了済み。ここでは何もしない。 */
void MLX90640_I2CInit(void)
{
}

/* I²C ジェネラルコールリセット（0x00 アドレスへ 0x06 を送る）。
 * 通常は使わないが、API 仕様として提供する。 */
int MLX90640_I2CGeneralReset(void)
{
    uint8_t cmd = 0x06U;
    lpi2c_master_transfer_t xfer = {0};

    xfer.flags         = kLPI2C_TransferDefaultFlag;
    xfer.slaveAddress  = 0x00U; /* general call address */
    xfer.direction     = kLPI2C_Write;
    xfer.subaddress    = 0U;
    xfer.subaddressSize = 0U;
    xfer.data          = &cmd;
    xfer.dataSize      = 1U;

    if (LPI2C_MasterTransferBlocking(EXAMPLE_I2C_MASTER, &xfer) != kStatus_Success)
    {
        return -MLX90640_I2C_NACK_ERROR;
    }
    return MLX90640_NO_ERROR;
}

/* 16bit レジスタアドレス startAddress から nMemAddressRead 個の 16bit ワードを読む。
 * LPI2C の単一トランザクションでの大容量リードを避けるため CHUNK ワードずつ分割。
 * 受信バイト列は MSB ファーストなので、ワードへ組み立てて data[] に格納する。 */
int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress, uint16_t nMemAddressRead, uint16_t *data)
{
    uint16_t remaining = nMemAddressRead;
    uint16_t addr      = startAddress;
    uint16_t outIdx    = 0;

    while (remaining > 0U)
    {
        uint16_t chunk = (remaining > MLX90640_READ_CHUNK_WORDS) ? MLX90640_READ_CHUNK_WORDS : remaining;

        lpi2c_master_transfer_t xfer = {0};
        xfer.flags          = kLPI2C_TransferDefaultFlag;
        xfer.slaveAddress   = slaveAddr;
        xfer.direction      = kLPI2C_Read;
        xfer.subaddress     = addr; /* 16bit レジスタアドレス（MSB first 送信） */
        xfer.subaddressSize = 2U;
        xfer.data           = s_rxBuf;
        xfer.dataSize       = (size_t)chunk * 2U;

        if (LPI2C_MasterTransferBlocking(EXAMPLE_I2C_MASTER, &xfer) != kStatus_Success)
        {
            return -MLX90640_I2C_NACK_ERROR;
        }

        for (uint16_t i = 0; i < chunk; i++)
        {
            data[outIdx + i] = ((uint16_t)s_rxBuf[2U * i] << 8) | (uint16_t)s_rxBuf[2U * i + 1U];
        }

        outIdx    += chunk;
        addr      += chunk; /* MLX90640 のメモリは16bitワード単位の連続アドレス */
        remaining -= chunk;
    }

    return MLX90640_NO_ERROR;
}

/* 16bit レジスタアドレス writeAddress へ 16bit データを書く（MSB ファースト）。
 * 書き込み後に読み戻して一致を確認する（公式実装に倣う）。 */
int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    uint8_t txData[2];
    txData[0] = (uint8_t)(data >> 8);   /* MSB */
    txData[1] = (uint8_t)(data & 0xFFU); /* LSB */

    lpi2c_master_transfer_t xfer = {0};
    xfer.flags          = kLPI2C_TransferDefaultFlag;
    xfer.slaveAddress   = slaveAddr;
    xfer.direction      = kLPI2C_Write;
    xfer.subaddress     = writeAddress; /* 16bit レジスタアドレス（MSB first 送信） */
    xfer.subaddressSize = 2U;
    xfer.data           = txData;
    xfer.dataSize       = 2U;

    if (LPI2C_MasterTransferBlocking(EXAMPLE_I2C_MASTER, &xfer) != kStatus_Success)
    {
        return -MLX90640_I2C_WRITE_ERROR;
    }

    /* 書き込み確認: 同アドレスを読み戻して一致を確認 */
    uint16_t readback = 0;
    int err = MLX90640_I2CRead(slaveAddr, writeAddress, 1, &readback);
    if (err != MLX90640_NO_ERROR)
    {
        return err;
    }
    if (readback != data)
    {
        return -MLX90640_I2C_WRITE_ERROR;
    }
    return MLX90640_NO_ERROR;
}

/* ボーレート変更。バス再初期化は led_blinky.c 側で行うため、ここでは未使用（no-op）。
 * EEPROM アクセスは 400kHz 以下が推奨だが、本実装は初期化時に 100kHz 固定で安全側。 */
void MLX90640_I2CFreqSet(int freq)
{
    (void)freq;
}
