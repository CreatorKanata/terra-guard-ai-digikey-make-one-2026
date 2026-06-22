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
/* 距離ばらつき(σ)はショットノイズ起因で、積分時間を長くすると統計的に小さくなる
   （σは概ね積分時間の平方根に反比例）。8×8では integration < (1000/freq - 4)ms。
   ただし全64ゾーンを各積分時間で測るため、長いほど実効fpsが落ちる
   （実測: 60ms→約4fps、20ms→約10fps）。レートとばらつきの妥協点として 33ms。
   ±5mm級はハード設定のみでは到達不可（積分を数百ms必要、fps<2）。 */
#define VL53_INTEG_MS     33U /* 積分時間[ms]（2〜1000。< 1000/freq - 4 = 96ms@10Hz） */
/* シャープナー: 隣接ゾーンへの反射漏れ込み(クロストーク)を抑え、物体エッジのばらつきを低減。
   0〜99%。まず控えめの20%。強すぎると遠距離の弱反射ターゲットを削りすぎるので注意。 */
#define VL53_SHARPENER_PCT 20U
#define VL53_GRID         8   /* 1辺のゾーン数 */
#define VL53_ZONES        64  /* 総ゾーン数 */
#define VL53_STATUS_VALID 5U  /* target_status==5 が最も高信頼な有効測距 */

/* ホールドで「有効測距」とみなす target_status の集合。
   5=100%有効、6/9=実用上有効、10=range複数だが実用可。
   5 のみに絞ると、5↔9/10 を行き来するゾーンで不必要にホールドが入り、
   距離に段差が出て背景差分がチラつく。受信側 STATUS_VALID={5,6,9,10} と一致させる。 */
static inline bool vl53_status_ok(uint8_t st)
{
    return (st == 5U) || (st == 6U) || (st == 9U) || (st == 10U);
}

/* ちらつき対策（ホールド＋タイムアウト）:
   信頼度の低いゾーン(status!=5)は瞬間的にゴミ距離(例: 2000mm→300mm)を返す。
   そのフレームの値は捨て、ゾーンごとに保持した「最後の有効値」で穴埋めする。
   ただし無効が VL53_HOLD_MAX フレーム連続したら、古い値を引きずらないよう
   「無効(VL53_DIST_INVALID = -1)」に倒す。受信側(ビューア)は -1 をグレー表示する。 */
#define VL53_HOLD_MAX     10     /* この回数まで前回有効値でホールド（10フレーム≒1.0s@10Hz） */
#define VL53_DIST_INVALID (-1)   /* ホールド切れ・対象なしゾーン。ビューアでグレー表示 */

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

    /* シャープナー: クロストーク低減で物体エッジの距離ばらつきを抑える。 */
    status = vl53l5cx_set_sharpener_percent(&s_vl53Dev, VL53_SHARPENER_PCT);
    if (status != 0U)
    {
        PRINTF("VL53L5CX: シャープナー設定失敗 (status=%d)\r\n", status);
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

    /* ホールド状態を初期化（全ゾーン無効=-1・カウンタ満杯） */
    for (int i = 0; i < VL53_ZONES; i++)
    {
        s_distHeld[i]   = VL53_DIST_INVALID;
        s_invalidCnt[i] = VL53_HOLD_MAX;
    }

    PRINTF("VL53L5CX: 初期化完了 (8x8 / %dHz / 積分%dms / シャープナー%d%%)\r\n",
           VL53_FREQ_HZ, VL53_INTEG_MS, VL53_SHARPENER_PCT);
    return true;
}

/* 生距離の妥当上限[mm]。これを超える値はゴミとみなし無効扱いにする。 */
#define VL53_DIST_SANE_MAX 4000

/* センサ取り付け向き補正: 8×8 ゾーンを左右反転する。
   生ゾーン i=row*8+col を、出力では同じ行の左右反転列 row*8+(7-col) に配置する。
   s_distHeld / s_invalidCnt（=DIST出力・get_frame・背景差分が見る側）を反転後の
   向きで保持する。STAT も print_frame 側で同じ反転を行い向きを揃える。 */
#define VL53_GRID 8
static inline int vl53_flip_h(int i)
{
    int row = i / VL53_GRID;
    int col = i % VL53_GRID;
    return row * VL53_GRID + (VL53_GRID - 1 - col);
}

/* ちらつき対策: ゾーンごとに status を見てホールド済み距離マップ(s_distHeld)を更新する。
   出力は左右反転(vl53_flip_h)した向きで s_distHeld に書く。
   - status==5(高信頼)      : 今フレームの距離で更新（妥当範囲外は無効扱い）、無効カウンタをリセット
   - status!=5 かつ猶予内    : 前回有効値を維持（無効カウンタを加算）
   - status!=5 かつ猶予超過  : 無効(-1)へ倒す（古い値を引きずらない。ビューアでグレー表示） */
static void vl53_apply_hold(void)
{
    for (int i = 0; i < VL53_ZONES; i++)
    {
        uint8_t st = s_vl53Results.target_status[VL53L5CX_NB_TARGET_PER_ZONE * i];
        int16_t d  = s_vl53Results.distance_mm[VL53L5CX_NB_TARGET_PER_ZONE * i];
        int     o  = vl53_flip_h(i); /* 左右反転した出力ゾーン */

        if (vl53_status_ok(st) && d >= 0 && d <= VL53_DIST_SANE_MAX)
        {
            s_distHeld[o]   = d;
            s_invalidCnt[o] = 0;
        }
        else if (s_invalidCnt[o] < VL53_HOLD_MAX)
        {
            /* 猶予内: 前回有効値をそのまま維持（s_distHeld[o] は変更しない） */
            s_invalidCnt[o]++;
        }
        else
        {
            /* 猶予超過: ホールド切れ。無効(-1)に倒す。 */
            s_distHeld[o] = VL53_DIST_INVALID;
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
    /* 統計はホールド済みマップ(s_distHeld)で算出。無効(-1)ゾーンは集計から除外する。 */
    int16_t vmin = 0, vmax = 0;
    long    sum = 0;
    int     validCnt = 0; /* 有効（ホールド済み距離を持つ）ゾーン数 */
    for (int i = 0; i < VL53_ZONES; i++)
    {
        int16_t d = s_distHeld[i];
        if (d == VL53_DIST_INVALID)
        {
            continue;
        }
        if (validCnt == 0 || d < vmin) vmin = d;
        if (validCnt == 0 || d > vmax) vmax = d;
        sum += d;
        validCnt++;
    }
    int     avg    = (validCnt > 0) ? (int)(sum / validCnt) : 0;
    int16_t center = s_distHeld[3 * VL53_GRID + 3];
    /* center が無効(-1)なら 0 として表示（このPRINTFは負値で化けるため）。 */
    int     centerOut = (center == VL53_DIST_INVALID) ? 0 : (int)center;

    if (validCnt == 0)
    {
        PRINTF("\r\nVL53L5CX  有効=0/64  全ゾーン無効\r\n");
    }
    else
    {
        PRINTF("\r\nVL53L5CX  有効=%d/64  min=%dmm  max=%dmm  avg=%dmm  center=%dmm\r\n",
               validCnt, (int)vmin, (int)vmax, avg, centerOut);
    }
}

void tof_vl53l5cx_print_frame(void)
{
    /* 距離: ホールド済みマップを出力（ちらつき対策済み）。
       ホールド切れ・対象なしゾーンは -1(VL53_DIST_INVALID) を出力する。
       受信側ビューアは -1 を NaN 扱いにしてグレー表示する。
       ※このPRINTF実装は %d に負値を渡すと unsigned 化け(-1→4294967295)するため、
         無効ゾーンは分岐して固定文字列 ",-1" を出す（巨大値を絶対に出さない）。 */
    PRINTF("DIST");
    for (int i = 0; i < VL53_ZONES; i++)
    {
        if (s_distHeld[i] == VL53_DIST_INVALID)
        {
            PRINTF(",-1");
        }
        else
        {
            PRINTF(",%d", (int)s_distHeld[i]);
        }
    }
    PRINTF("\r\n");

    /* 状態: 全ゾーンの target_status（受信側で信頼度フィルタ用）。
       距離(s_distHeld)と同じ左右反転の向きで出す。出力ゾーン o の値は
       生ゾーン i=vl53_flip_h(o)（反転は自己逆写像なので flip_h で戻せる）。 */
    PRINTF("STAT");
    for (int o = 0; o < VL53_ZONES; o++)
    {
        int i = vl53_flip_h(o);
        PRINTF(",%d", (int)s_vl53Results.target_status[VL53L5CX_NB_TARGET_PER_ZONE * i]);
    }
    PRINTF("\r\n");
}

const int16_t *tof_vl53l5cx_get_frame(void)
{
    return s_distHeld;
}
