/*
 * TerraGuard AI — FRDM-MCXN947
 * tof_vl53l5cx: ToF距離センサ VL53L5CX モジュールの実装
 *
 * ST 公式 ULD(BSD-3) を利用。LPI2C プラットフォーム層は vendor/vl53l5cx を参照。
 *
 * Copyright 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tof_vl53l5cx.h"
#include "sensor_bus.h"
#include "fsl_debug_console.h"
#include "vl53l5cx_api.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define VL53_RESOLUTION   VL53L5CX_RESOLUTION_8X8 /* 8×8 = 64ゾーン */
/* レートを下げて積分時間を確保すると、遠距離・弱反射ゾーンの信頼度(status)が上がり
   target_status==255(低信頼)が減る。背景監視用途なら低レートで十分。 */
#define VL53_FREQ_HZ      10U /* 8×8。15→10Hzにして積分時間を確保 */
#define VL53_INTEG_MS     20U /* 積分時間[ms]（2〜1000。< 1000/freq - 4） */
#define VL53_GRID         8   /* 1辺のゾーン数 */
#define VL53_ZONES        64  /* 総ゾーン数 */
#define VL53_STATUS_VALID 5U  /* target_status==5 が有効測距 */

/* ちらつき対策（ホールド＋タイムアウト）:
   信頼度の低いゾーン(status!=5)は瞬間的にゴミ距離(例: 2000mm→300mm)を返す。
   そのフレームの値は捨て、ゾーンごとに保持した「最後の有効値」で穴埋めする。
   ただし無効が VL53_HOLD_MAX フレーム連続したら、古い値を引きずらないよう
   「最大レンジ(VL53_DIST_MAX)」に倒す。
   遠距離・弱反射ゾーンは「対象なし＝遠い背景」なので、番兵(-1)ではなく
   最大レンジで出すと、ヒートマップが連続的になり害鳥検出にも都合がよい。
   ※-1 を出すと一部 PRINTF 実装の %d が unsigned 化けして 4294967295 になる罠もある。 */
#define VL53_HOLD_MAX     5      /* この回数まで前回有効値でホールド（5フレーム≒0.5s@10Hz） */
#define VL53_DIST_MAX     4000   /* VL53L5CX の実用上限[mm]。無効/対象なしはこの値に倒す */

/*******************************************************************************
 * Variables
 ******************************************************************************/
/* VL53L5CX 用（Configuration は temp_buffer 等を内包し数KB。必ず static に置く） */
static VL53L5CX_Configuration s_vl53Dev;
static VL53L5CX_ResultsData   s_vl53Results;

/* ホールド済み距離マップ（出力用）。poll で更新し print_* で参照する。 */
static int16_t s_distHeld[VL53_ZONES];        /* ゾーンごとのホールド済み距離[mm]（無効は -1） */
static uint8_t s_invalidCnt[VL53_ZONES];      /* ゾーンごとの無効連続フレーム数 */

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void vl53_apply_hold(void);

/*******************************************************************************
 * Code
 ******************************************************************************/
bool tof_vl53l5cx_setup(void)
{
    uint8_t status, isAlive;

    /* スキャン(低レベル Start/Stop)後のバス状態をクリーンにするため LPI2C を再初期化。 */
    sensor_i2c_master_init();

    /* ULD はアドレスを 8bit 表記で持つ（platform 層で >>1 して 7bit に変換）。 */
    s_vl53Dev.platform.address = VL53L5CX_DEFAULT_I2C_ADDRESS; /* 0x52 */

    status = vl53l5cx_is_alive(&s_vl53Dev, &isAlive);
    if (status != 0U || isAlive == 0U)
    {
        PRINTF("VL53L5CX: 応答なし (status=%d, alive=%d)\r\n", status, isAlive);
        return false;
    }
    PRINTF("VL53L5CX: is_alive OK。ファームウェア転送中(約84KB)...\r\n");

    /* FW(~84KB)をI²Cで転送。WrMulti のチャンク分割で実施。数十ms〜数百ms。 */
    status = vl53l5cx_init(&s_vl53Dev);
    if (status != 0U)
    {
        PRINTF("VL53L5CX: init(FW転送)失敗 (status=%d)\r\n", status);
        return false;
    }

    status = vl53l5cx_set_resolution(&s_vl53Dev, VL53_RESOLUTION);
    if (status != 0U)
    {
        PRINTF("VL53L5CX: 解像度設定失敗 (status=%d)\r\n", status);
        return false;
    }

    /* 積分時間を設定（frequency の前に。低信頼ゾーン(status255)を減らす）。
       8×8 では integration < (1000/freq - 4)ms の制約あり。 */
    status = vl53l5cx_set_integration_time_ms(&s_vl53Dev, VL53_INTEG_MS);
    if (status != 0U)
    {
        PRINTF("VL53L5CX: 積分時間設定失敗 (status=%d)\r\n", status);
        return false;
    }

    status = vl53l5cx_set_ranging_frequency_hz(&s_vl53Dev, VL53_FREQ_HZ);
    if (status != 0U)
    {
        PRINTF("VL53L5CX: フレームレート設定失敗 (status=%d)\r\n", status);
        return false;
    }

    status = vl53l5cx_start_ranging(&s_vl53Dev);
    if (status != 0U)
    {
        PRINTF("VL53L5CX: 測距開始失敗 (status=%d)\r\n", status);
        return false;
    }

    /* ホールド状態を初期化（全ゾーン「対象なし＝最大レンジ」・カウンタ満杯） */
    for (int i = 0; i < VL53_ZONES; i++)
    {
        s_distHeld[i]   = VL53_DIST_MAX;
        s_invalidCnt[i] = VL53_HOLD_MAX;
    }

    PRINTF("VL53L5CX: 初期化完了 (8x8 / %dHz)\r\n", VL53_FREQ_HZ);
    return true;
}

/* ちらつき対策: ゾーンごとに status を見てホールド済み距離マップ(s_distHeld)を更新する。
   - status==5(高信頼)      : 今フレームの距離で更新、無効カウンタをリセット
   - status!=5 かつ猶予内    : 前回有効値を維持（無効カウンタを加算）
   - status!=5 かつ猶予超過  : 最大レンジ(VL53_DIST_MAX)へ倒す（古い値を引きずらない／
                               遠距離・対象なしとして連続的に表示される） */
static void vl53_apply_hold(void)
{
    for (int i = 0; i < VL53_ZONES; i++)
    {
        uint8_t st = s_vl53Results.target_status[VL53L5CX_NB_TARGET_PER_ZONE * i];
        int16_t d  = s_vl53Results.distance_mm[VL53L5CX_NB_TARGET_PER_ZONE * i];

        if (st == VL53_STATUS_VALID)
        {
            s_distHeld[i]   = (d > VL53_DIST_MAX) ? VL53_DIST_MAX : d; /* 上限クランプ */
            s_invalidCnt[i] = 0;
        }
        else if (s_invalidCnt[i] < VL53_HOLD_MAX)
        {
            /* 猶予内: 前回有効値をそのまま維持（s_distHeld[i] は変更しない） */
            s_invalidCnt[i]++;
        }
        else
        {
            /* 猶予超過: ホールド切れ。対象なし＝最大レンジに倒す。 */
            s_distHeld[i] = VL53_DIST_MAX;
        }
    }
}

int tof_vl53l5cx_poll(void)
{
    uint8_t isReady = 0;
    uint8_t st      = vl53l5cx_check_data_ready(&s_vl53Dev, &isReady);
    if (st != 0U)
    {
        return -(int)st;
    }
    if (isReady == 0U)
    {
        return 0;
    }

    st = vl53l5cx_get_ranging_data(&s_vl53Dev, &s_vl53Results);
    if (st != 0U)
    {
        return -(int)st;
    }

    /* ちらつき対策: 信頼度の低いゾーンを前回有効値でホールドする。 */
    vl53_apply_hold();
    return 1;
}

void tof_vl53l5cx_print_stats(void)
{
    /* 統計はホールド済みマップ(s_distHeld)で算出。無効ゾーンは最大レンジに倒して
       あるので全ゾーンを単純集計する。高信頼ゾーン数(status==5)は参考表示。 */
    int16_t vmin = 0, vmax = 0;
    long    sum = 0;
    int     validCnt = 0; /* 今フレームで status==5 だったゾーン数（参考） */
    for (int i = 0; i < VL53_ZONES; i++)
    {
        int16_t d = s_distHeld[i];
        if (i == 0 || d < vmin) vmin = d;
        if (i == 0 || d > vmax) vmax = d;
        sum += d;
        if (s_vl53Results.target_status[VL53L5CX_NB_TARGET_PER_ZONE * i] == VL53_STATUS_VALID)
        {
            validCnt++;
        }
    }
    int16_t center = s_distHeld[3 * VL53_GRID + 3];

    PRINTF("\r\nVL53L5CX  高信頼=%d/64  min=%dmm  max=%dmm  avg=%dmm  center=%dmm\r\n",
           validCnt, (int)vmin, (int)vmax, (int)(sum / VL53_ZONES), (int)center);
}

void tof_vl53l5cx_print_frame(void)
{
    /* 距離: ホールド済みマップを出力（ちらつき対策済み）。
       ホールド切れ・対象なしゾーンは最大レンジ(VL53_DIST_MAX=4000mm)で出力する。
       常に 0〜4000 の非負整数なので受信側で巨大値(4294967295)化けは起きない。 */
    PRINTF("DIST");
    for (int i = 0; i < VL53_ZONES; i++)
    {
        PRINTF(",%d", (int)s_distHeld[i]);
    }
    PRINTF("\r\n");

    /* 状態: 全ゾーンの target_status（受信側で信頼度フィルタ用） */
    PRINTF("STAT");
    for (int i = 0; i < VL53_ZONES; i++)
    {
        PRINTF(",%d", (int)s_vl53Results.target_status[VL53L5CX_NB_TARGET_PER_ZONE * i]);
    }
    PRINTF("\r\n");
}
