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
 *   mlxToRaw  : 変換後の温度[℃] 768画素（センサ生の向き = 行24×列32）
 *   mlxTo     : 90度右回転（時計回り）→ 中央24行 crop 後の温度[℃] 576画素（24×24）。
 *               センサを物理的に90度回転して取り付けたため、取得直後に「回転＋24×24
 *               中央クロップ」を行い、以降の全消費者（bin送出・統計・背景差分・NPU）に
 *               正立かつ正方の24×24画像を渡す。送出も収集もこの24×24で統一する。 */
static uint16_t       s_mlxEeData[832];
static paramsMLX90640 s_mlxParams;
static uint16_t       s_mlxFrame[MLX90640_FRAME_WORDS];
static float          s_mlxToRaw[768];
static float          s_mlxTo[THERMAL_OUT_PIXELS]; /* 24×24 = 576 */

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
    /* CalculateTo は該当サブページの画素だけ s_mlxToRaw（センサ生の向き 32×24）に書く。
       回転は両サブページが揃った完成フレーム時に poll_frame でまとめて行う。 */
    MLX90640_CalculateTo(s_mlxFrame, &s_mlxParams, MLX90640_EMISSIVITY, tr, s_mlxToRaw);

    return sp;
}

/* センサ生フレーム src（行24×列32, 行優先 index=r*32+c）を
   90度右回転（時計回り）してから中央24行を crop し、正方の 24×24（576画素,
   行優先 index=oi*24+j）を dst に書き込む。

   90度右回転（生 行R=24 × 列C=32 → 回転後 行C=32 × 列R=24）:
     rot[i][j] = src[(R-1-j)][i]   （i=0..31 回転後行, j=0..23 回転後列）
   中央24行 crop（回転後の行32のうち上下4行を捨て、行 4..27 を採用）:
     出力行 oi = i - 4 （0..23）、すなわち i = oi + 4
   合成:
     dst[oi*24 + j] = src[(R-1-j)*C + (oi+4)]   （R=24, C=32） */
static void rotate_crop(const float *src, float *dst)
{
    const int R = 24; /* 生の行数 */
    const int C = 32; /* 生の列数 */
    const int CROP_TOP = 4; /* 回転後32行のうち上を4行カット */
    for (int oi = 0; oi < THERMAL_OUT_H; oi++)      /* 出力行 0..23 */
    {
        for (int j = 0; j < THERMAL_OUT_W; j++)     /* 出力列 0..23 */
        {
            dst[oi * THERMAL_OUT_W + j] = src[(R - 1 - j) * C + (oi + CROP_TOP)];
        }
    }
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

    /* 0/1 の両方が揃ったら完成フレーム。次フレームのために状態をクリア。
       完成した生フレーム(行24×列32)を 90度右回転＋中央24行crop して
       s_mlxTo(24×24) を更新する。以降の全消費者（get_frame/stats/bin送出）は
       この 24×24 を参照する。 */
    if (s_subpageSeen == 0x03U)
    {
        s_subpageSeen = 0U;
        rotate_crop(s_mlxToRaw, s_mlxTo);
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
    for (int i = 0; i < THERMAL_OUT_PIXELS; i++)
    {
        float v = s_mlxTo[i];
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
        sum += v;
    }
    float avg = sum / (float)THERMAL_OUT_PIXELS;
    /* 出力は 24×24。中心画素: 行12・列12 → index = 12*24 + 12 = 300 */
    float center = s_mlxTo[(THERMAL_OUT_H / 2) * THERMAL_OUT_W + (THERMAL_OUT_W / 2)];

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
    for (int i = 0; i < THERMAL_OUT_PIXELS; i++)
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
    /* magic(2) + Ta(2) + 576画素(1152) = 1156バイト。リトルエンディアンで詰める。
       回転＋24×24crop 済みの s_mlxTo をそのまま送る（受信側も 24×24 で解釈）。 */
    static uint8_t buf[2 + 2 + THERMAL_OUT_PIXELS * 2];
    size_t pos = 0;

    buf[pos++] = 0xAAU;
    buf[pos++] = 0x55U;

    int16_t taC = centi_i16(ta);
    buf[pos++] = (uint8_t)(taC & 0xFF);
    buf[pos++] = (uint8_t)((taC >> 8) & 0xFF);

    for (int i = 0; i < THERMAL_OUT_PIXELS; i++)
    {
        int16_t c  = centi_i16(s_mlxTo[i]);
        buf[pos++] = (uint8_t)(c & 0xFF);
        buf[pos++] = (uint8_t)((c >> 8) & 0xFF);
    }

    sensor_write_raw(buf, pos);
}
