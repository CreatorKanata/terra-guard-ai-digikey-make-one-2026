/*
 * TerraGuard AI — FRDM-MCXN947
 * bg_subtract: 背景差分（背景モデル + 前景抽出 + 候補判定）
 *
 * 単純な「直近Nフレーム平均」ではなく、起動時の初期背景 + ゆっくり更新する
 * 背景モデル方式。鳥候補がいないときだけ背景を EMA でゆっくり更新し、
 * 候補があるときは背景更新を凍結する（カラスが背景に吸収されるのを防ぐ）。
 *
 * 対象は2系統:
 *   - サーマル(MLX90640, 回転＋crop後 24×24=576画素, ℃) … 前景 = max(0, current - bg)（暖かい物体）
 *   - 距離(VL53L5CX, 8×8=64ゾーン, mm)        … 前景 = max(0, bg - current)（手前に出た物体）
 *
 * Copyright 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _BG_SUBTRACT_H_
#define _BG_SUBTRACT_H_

#include <stdbool.h>
#include <stdint.h>

#include "thermal_mlx90640.h" /* THERMAL_OUT_PIXELS (24×24=576) */

#define BG_THERMAL_PIXELS THERMAL_OUT_PIXELS /* 24×24=576（回転＋crop後） */
#define BG_DIST_ZONES     64  /* 8×8 */

/* 背景差分の状態を初期化する（起動時に1回）。両センサの背景モデルを未確立にする。 */
void bg_reset(void);

/* --- サーマル ---------------------------------------------------------- */
/* 新しいサーマル完成フレーム(768画素[℃])を投入する。
   初期背景の蓄積、または前景抽出（前景マップ・統計の更新）を行う。
   背景モデルの更新は bg_apply_update_policy() で別途行う（直近の生フレームを保持）。 */
void bg_thermal_update(const float *to768);

/* 直近サーマル前景マップ(768画素, ℃。背景より暖かい正の差分。負は0)を返す。 */
const float *bg_thermal_fg(void);

/* 直近サーマルの「背景確立済みか」。初期蓄積中は false。 */
bool bg_thermal_ready(void);

/* 直近サーマル前景の最大差分[℃]。 */
float bg_thermal_fg_max(void);

/* 直近サーマル前景の有効画素数（差分が閾値を超えた画素数）。 */
int bg_thermal_fg_area(void);

/* --- 距離 -------------------------------------------------------------- */
/* 新しい距離フレーム(64ゾーン[mm]、無効ゾーンは -1)を投入する。
   初期背景の蓄積、または前景抽出を行う。無効ゾーンは背景蓄積・更新の対象外（前景0）。
   背景モデルの更新は bg_apply_update_policy() で別途行う。 */
void bg_dist_update(const int16_t *dist64);

/* 直近距離前景マップ(64ゾーン, mm。背景より手前に出た正の量。負/無効は0)を返す。 */
const int16_t *bg_dist_fg(void);

/* 距離背景が確立済みか。 */
bool bg_dist_ready(void);

/* 直近距離前景の最大量[mm]。 */
int bg_dist_fg_max(void);

/* 直近距離前景の有効ゾーン数（前景量が閾値を超えたゾーン数）。 */
int bg_dist_fg_area(void);

/* --- 候補判定・背景更新 ----------------------------------------------- */
/* 直近フレームで「鳥候補あり」と判定されたか（サーマル前景 AND 距離前景）。
   候補ありのとき背景更新は凍結される。 */
bool bg_candidate_present(void);

/* 両センサの直近前景状態から候補判定を更新し、候補なしのときだけ背景モデルを
   EMA でゆっくり更新する（候補ありなら凍結）。各センサのフレーム取得後に毎ループ呼ぶ。
   背景が両方とも未確立のうちは何もしない。 */
void bg_apply_update_policy(void);

/* --- シリアル出力 ------------------------------------------------------ */
/* 距離前景マップをテキスト1行で出力する。
   形式: "DFG,<f0>,...,<f63>"（mm整数。背景より手前に出た量。0=前景なし）
   さらに候補判定の要約も出力: "DET,<cand>,<t_max>,<t_area>,<d_max>,<d_area>"
   （cand=0/1、t_max は ℃×100 整数、d_max は mm）。 */
void bg_dist_print_frame(void);

/* サーマル前景マップをバイナリで高速送出する（背景差分後の正の差分マップ）。
   形式（リトルエンディアン、計1538バイト）:
     magic   : 0xAA 0x56                 （2B。生サーマル 0xAA55 と区別）
     pixels  : int16 ×768  各 fg×100[1/100℃]（1536B）
   背景未確立のうちは送出しない（送出したら true を返す）。 */
bool bg_thermal_send_fg_bin(void);

#endif /* _BG_SUBTRACT_H_ */
