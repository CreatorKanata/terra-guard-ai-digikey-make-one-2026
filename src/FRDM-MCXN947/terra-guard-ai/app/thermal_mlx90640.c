/*
 * TerraGuard AI — FRDM-MCXN947
 * thermal_mlx90640: サーマルセンサ MLX90640 モジュールの実装
 *
 * Melexis 公式 API(Apache-2.0) を利用。LPI2C ドライバ層は vendor/mlx90640 を参照。
 *
 * Copyright 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "thermal_mlx90640.h"
#include "sensor_bus.h"
#include "fsl_debug_console.h"
#include "app.h"
#include <MLX90640_API.h>
#include <MLX90640_I2C_Driver.h>

/*******************************************************************************
 * Definitions
 ******************************************************************************/
/* Control1 リフレッシュレート設定値（サブページ1枚あたりの速度）:
   0x00=0.5Hz 0x01=1Hz 0x02=2Hz 0x03=4Hz 0x04=8Hz 0x05=16Hz 0x06=32Hz 0x07=64Hz。
   Chess は subpage 0/1 の2枚で1完成フレームなので、完成フレームレートはこの半分。
   I²C 1MHz では 1サブページ(~1.7KB) を ~17ms で読めるので高レートが現実的。
   実測では 16Hz(完成8Hz狙い)で 7.2fps が最良。32Hz に上げると data-ready と
   ポーリング間隔(VL53と交互)の同期がずれてサブページを取り逃し、逆に 5.9fps に低下した。
   よって 16Hz を採用する。VL53(10Hz)との同時動作でも 1MHz 帯域に余裕がある。 */
#define MLX90640_REFRESH_16HZ 0x05U
#define MLX90640_EMISSIVITY   0.95f  /* 放射率（一般物体・生物体向け推奨） */
#define MLX90640_TR_OFFSET    8.0f   /* 反射温度 tr = Ta - 8℃（公式サンプル推奨） */
#define MLX90640_FRAME_WORDS  834U   /* GetFrameData が要求するワード数 */

/*******************************************************************************
 * Variables
 ******************************************************************************/
/* MLX90640 用バッファ（大きいのでファイルスコープ static に置く）
 *   eeData    : EEPROM ダンプ 832 ワード
 *   mlxParams : 展開済み校正パラメータ（約 2.5KB）
 *   frameData : 1サブページ分の生フレーム 834 ワード
 *   mlxTo     : 変換後の温度[℃] 768 画素 */
static uint16_t       s_mlxEeData[832];
static paramsMLX90640 s_mlxParams;
static uint16_t       s_mlxFrame[MLX90640_FRAME_WORDS];
static float          s_mlxTo[768];

/* 完成フレーム検出用: 出揃ったサブページを bit0=subpage0 / bit1=subpage1 で記録。
   両ビットが立てば 0/1 が揃い、s_mlxTo[768] 全体が最新になっている。 */
static uint8_t        s_subpageSeen;

/*******************************************************************************
 * Code
 ******************************************************************************/
bool thermal_mlx90640_setup(void)
{
    int err;

    /* スキャン(低レベル Start/Stop)後のバス状態をクリーンにするため LPI2C を再初期化。 */
    sensor_i2c_master_init();

    /* リフレッシュレート 16Hz（サブページ）。実測で完成 ~7fps と最良（定義の補足参照）。 */
    err = MLX90640_SetRefreshRate(MLX90640_I2C_ADDR, MLX90640_REFRESH_16HZ);
    if (err != MLX90640_NO_ERROR)
    {
        PRINTF("MLX90640: リフレッシュレート設定失敗 (err=%d)\r\n", err);
        return false;
    }

    /* Chess パターン（工場デフォルト・推奨） */
    err = MLX90640_SetChessMode(MLX90640_I2C_ADDR);
    if (err != MLX90640_NO_ERROR)
    {
        PRINTF("MLX90640: Chessモード設定失敗 (err=%d)\r\n", err);
        return false;
    }

    /* EEPROM(0x2400〜) 全ダンプ（大容量のため I2CRead 内で分割読み出し） */
    err = MLX90640_DumpEE(MLX90640_I2C_ADDR, s_mlxEeData);
    if (err != MLX90640_NO_ERROR)
    {
        PRINTF("MLX90640: EEPROM読み出し失敗 (err=%d)\r\n", err);
        return false;
    }

    /* 校正パラメータ展開（float[768] のローカル配列を使うためスタック拡張済み） */
    err = MLX90640_ExtractParameters(s_mlxEeData, &s_mlxParams);
    if (err != MLX90640_NO_ERROR)
    {
        /* broken/outlier pixel の警告(>0)は致命ではないので継続する。 */
        PRINTF("MLX90640: パラメータ展開で警告 (err=%d、継続)\r\n", err);
    }

    PRINTF("MLX90640: 初期化完了 (2Hz / Chess / 放射率0.95 / tr=Ta-8)\r\n");
    return true;
}

int thermal_mlx90640_data_ready(void)
{
    uint16_t statusRegister = 0;
    int err = MLX90640_I2CRead(MLX90640_I2C_ADDR, MLX90640_STATUS_REG, 1, &statusRegister);
    if (err != MLX90640_NO_ERROR)
    {
        return err; /* 負値（I²Cエラー） */
    }
    return MLX90640_GET_DATA_READY(statusRegister) ? 1 : 0;
}

int thermal_mlx90640_read_subframe(void)
{
    int sp = MLX90640_GetFrameData(MLX90640_I2C_ADDR, s_mlxFrame);
    if (sp < 0)
    {
        return sp;
    }

    float ta = MLX90640_GetTa(s_mlxFrame, &s_mlxParams);
    float tr = ta - MLX90640_TR_OFFSET; /* 反射温度 */
    MLX90640_CalculateTo(s_mlxFrame, &s_mlxParams, MLX90640_EMISSIVITY, tr, s_mlxTo);

    return sp;
}

int thermal_mlx90640_poll_frame(void)
{
    /* data-ready なサブページが無ければ何もしない（VL53 のポーリングを阻害しない）。 */
    int ready = thermal_mlx90640_data_ready();
    if (ready <= 0)
    {
        return ready; /* 0=未準備, <0=エラー */
    }

    /* 1サブページ取得・変換。CalculateTo は該当サブページの画素だけ s_mlxTo に書く。 */
    int sp = thermal_mlx90640_read_subframe();
    if (sp < 0)
    {
        s_subpageSeen = 0U; /* エラー時は揃い状態をリセット */
        return sp;
    }

    /* 出揃ったサブページを記録（sp は 0 または 1）。 */
    s_subpageSeen |= (uint8_t)(1U << (sp & 1));

    /* 0/1 の両方が揃ったら完成フレーム。次フレームのために状態をクリア。 */
    if (s_subpageSeen == 0x03U)
    {
        s_subpageSeen = 0U;
        return 1;
    }
    return 0;
}

float thermal_mlx90640_get_ta(void)
{
    return MLX90640_GetTa(s_mlxFrame, &s_mlxParams);
}

const float *thermal_mlx90640_get_frame(void)
{
    return s_mlxTo;
}

void thermal_mlx90640_print_stats(float ta)
{
    float vmin = s_mlxTo[0];
    float vmax = s_mlxTo[0];
    float sum  = 0.0f;
    for (int i = 0; i < 768; i++)
    {
        float v = s_mlxTo[i];
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
        sum += v;
    }
    float avg = sum / 768.0f;
    /* 中心画素: 行12(0-23)・列16(0-31) → index = 12*32 + 16 = 400 */
    float center = s_mlxTo[12 * 32 + 16];

    sensor_print_temp_c("\r\nMLX90640  Ta=", ta);
    sensor_print_temp_c("  min=", vmin);
    sensor_print_temp_c("  max=", vmax);
    sensor_print_temp_c("  avg=", avg);
    sensor_print_temp_c("  center=", center);
    PRINTF("\r\n");
}

void thermal_mlx90640_print_frame(float ta)
{
    PRINTF("FRAME,%d", (int)(ta * 100.0f + (ta >= 0 ? 0.5f : -0.5f)));
    for (int i = 0; i < 768; i++)
    {
        float v       = s_mlxTo[i];
        int32_t centi = (int32_t)(v * 100.0f + (v >= 0 ? 0.5f : -0.5f));
        PRINTF(",%d", centi);
    }
    PRINTF("\r\n");
}

/* float℃ を 1/100℃ の int16 に丸める（範囲は ±327.67℃ で温度的に十分）。 */
static int16_t centi_i16(float v)
{
    int32_t c = (int32_t)(v * 100.0f + (v >= 0 ? 0.5f : -0.5f));
    if (c > 32767) c = 32767;
    if (c < -32768) c = -32768;
    return (int16_t)c;
}

void thermal_mlx90640_send_frame_bin(float ta)
{
    /* magic(2) + Ta(2) + 768画素(1536) = 1540バイト。リトルエンディアンで詰める。 */
    static uint8_t buf[2 + 2 + 768 * 2];
    size_t pos = 0;

    buf[pos++] = 0xAAU;
    buf[pos++] = 0x55U;

    int16_t taC = centi_i16(ta);
    buf[pos++] = (uint8_t)(taC & 0xFF);
    buf[pos++] = (uint8_t)((taC >> 8) & 0xFF);

    for (int i = 0; i < 768; i++)
    {
        int16_t c  = centi_i16(s_mlxTo[i]);
        buf[pos++] = (uint8_t)(c & 0xFF);
        buf[pos++] = (uint8_t)((c >> 8) & 0xFF);
    }

    sensor_write_raw(buf, pos);
}
