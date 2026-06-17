/*
 * TerraGuard AI — FRDM-MCXN947
 * sensor_bus: センサ共通基盤
 *   - 外部I²Cバス(LPI2C2/FLEXCOMM2, J8 pin3/4) の初期化・アドレススキャン
 *   - オンボード温度センサ P3T1755DP(I3C1) の初期化・読み出し
 *   - I3C 低レベルアクセス（WrSensor/RdSensor/動的アドレス割当）
 *   - 共通ユーティリティ（float温度のシリアル出力）
 *
 * I2C/I3C のアドレス・ベース等のマクロは app.h に定義済み。
 *
 * Copyright 2022, 2025 NXP / 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _SENSOR_BUS_H_
#define _SENSOR_BUS_H_

#include <stddef.h>
#include <stdbool.h>
#include "fsl_common.h"
#include "fsl_p3t1755.h"

/*******************************************************************************
 * 共通ユーティリティ
 ******************************************************************************/
/* float 温度[℃] を "%d.%02d C" 形式で出力する（debug_console_lite は %f 非対応）。
   負値も符号付きで正しく出す。label は値の前に付与する文字列。 */
void sensor_print_temp_c(const char *label, float celsius);

/* 生バイト列をデバッグUART(VCOM)へそのまま送信する（PRINTF を介さない）。
   バイナリフレーム送出用。PRINTF の書式変換コストを避け高速に送れる。 */
void sensor_write_raw(const uint8_t *data, size_t len);

/*******************************************************************************
 * 外部I²C(LPI2C2)
 ******************************************************************************/
/* 外部I²Cマスタ(LPI2C2)を初期化する。低レベル Start/Stop を使った後の
   バス状態クリーンアップ目的で複数回呼んでよい。 */
void sensor_i2c_master_init(void);

/* I²Cバス(0x08〜0x77)をスキャンし、ACK を返すアドレスを列挙して出力する。
   MLX90640(0x33) が見つかれば疎通OKとみなす。 */
void sensor_i2c_bus_scan(void);

/*******************************************************************************
 * I3C 低レベルアクセス（P3T1755 ドライバから利用）
 ******************************************************************************/
/* I3C でセンサのレジスタへ書き込み */
status_t sensor_i3c_write(uint8_t deviceAddress, uint32_t regAddress, uint8_t *regData, size_t dataSize);

/* I3C でセンサのレジスタから読み出し */
status_t sensor_i3c_read(uint8_t deviceAddress, uint32_t regAddress, uint8_t *regData, size_t dataSize);

/*******************************************************************************
 * オンボード温度センサ P3T1755（I3C1）
 ******************************************************************************/
/* I3Cマスタ初期化 → P3T1755 への動的アドレス割当 → ドライバ初期化までを行う。
   成功で kStatus_Success。 */
status_t sensor_p3t1755_init(void);

/* P3T1755 から温度[℃] を読み出す。成功で kStatus_Success。 */
status_t sensor_p3t1755_read(double *temperature);

#endif /* _SENSOR_BUS_H_ */
