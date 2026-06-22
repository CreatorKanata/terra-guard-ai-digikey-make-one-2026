/*
 * TerraGuard AI — FRDM-MCXN947
 * tof_vl53l5cx: ToF距離センサ VL53L5CX(8×8, I²C 0x29) モジュール
 *   - 初期化（is_alive → FW転送 init → 8×8 → 測距開始）
 *   - 新フレームのポーリング取得
 *   - 8×8 距離マップ/状態のシリアル出力
 *
 * Copyright 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _TOF_VL53L5CX_H_
#define _TOF_VL53L5CX_H_

#include <stdbool.h>
#include <stdint.h>

/* VL53L5CX を初期化（is_alive → FW転送 init → 8×8 → 積分時間 → レート → 測距開始）。
   成功で true。内部で LPI2C を再初期化する。 */
bool tof_vl53l5cx_setup(void);

/* 新しい測距データが準備できていれば取得して内部バッファへ格納する。
   戻り値:
     1  = 新フレームを取得した（print_frame で出力可能）
     0  = 新データ未準備（呼び出し側で短く待つ）
     <0 = エラー（status をそのまま負値化したもの: -(int)status） */
int tof_vl53l5cx_poll(void);

/* 直近に取得した 8×8 距離の人間向け統計を1行出力する（機械可読フレームとは別）。
   "VL53L5CX  高信頼=.. min=.. max=.. avg=.. center=.." */
void tof_vl53l5cx_print_stats(void);

/* 直近に取得した 8×8 距離マップと状態を機械可読形式で出力する。
   - "DIST,<z0>,...,<z63>"（mm整数。ちらつき対策でホールド済み。
      無効/対象なし/ホールド切れは -1。受信側は -1 をグレー(NaN)表示する）
   - "STAT,<s0>,...,<s63>"（生の target_status。5=有効。どのゾーンがホールドされたか照合用） */
void tof_vl53l5cx_print_frame(void);

/* 直近のホールド済み 8×8 距離マップ[mm]（無効ゾーンは -1）への読み取り専用ポインタを返す。
   背景差分(bg_subtract)等に渡す用。tof_vl53l5cx_poll() で新フレーム取得後に参照すること。 */
const int16_t *tof_vl53l5cx_get_frame(void);

#endif /* _TOF_VL53L5CX_H_ */
