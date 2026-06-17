/*
 * TerraGuard AI — FRDM-MCXN947
 * thermal_mlx90640: サーマルセンサ MLX90640(32×24, I²C 0x33) モジュール
 *   - 初期化（32Hz / Chess / 放射率0.95 / tr=Ta-8）
 *   - サブページ取得＋温度変換、両サブページ揃いでの完成フレーム検出
 *   - 統計/全画素フレームのシリアル出力
 *
 * Chess モードは subpage 0/1 を市松状に交互測定する。動く物体があると
 * 片方のサブページだけ先に更新されて市松模様になるため、0/1 の両方を
 * 取得して揃ってから1完成フレームとして扱う（thermal_mlx90640_poll_frame）。
 *
 * I²Cアドレス等は app.h の MLX90640_I2C_ADDR を使用。
 *
 * Copyright 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _THERMAL_MLX90640_H_
#define _THERMAL_MLX90640_H_

#include <stdbool.h>

/* MLX90640 を初期化（2Hz/Chess 設定 → EEPROM 読み出し → 校正パラメータ展開）。
   成功で true。内部で LPI2C を再初期化する。 */
bool thermal_mlx90640_setup(void);

/* 新しいサブフレームが準備できているかを非ブロッキングで確認する。
   ステータスレジスタの data-ready ビットだけを読む（バスを長時間占有しない）。
   戻り値:
     1  = 準備OK（thermal_mlx90640_read_subframe を呼べる）
     0  = まだ準備できていない
     <0 = I²Cエラー（MLX90640_API のエラーコード） */
int thermal_mlx90640_data_ready(void);

/* 1サブページ分のフレームを取得し、温度[℃] に変換する。
   data-ready 確認後に呼ぶこと（準備前に呼ぶとデータ準備までブロックする）。
   戻り値 >=0: サブページ番号(0/1)、<0: エラー。 */
int thermal_mlx90640_read_subframe(void);

/* 完成フレーム1枚（subpage 0/1 の両方が揃った状態）を非ブロッキングで進める。
   data-ready なサブページがあれば1枚取得・変換し、両サブページが出揃ったら 1 を返す。
   内部で data-ready 確認 → read_subframe を行うので、これ1つを毎ループ呼べばよい。
   戻り値:
     1  = 完成フレームが揃った（print_stats / print_frame で出力可能）
     0  = まだ揃っていない（このループでは出力しない）
     <0 = エラー（MLX90640_API のエラーコード） */
int thermal_mlx90640_poll_frame(void);

/* 直近に変換したフレームの周辺温度[℃]（Ta）を返す。 */
float thermal_mlx90640_get_ta(void);

/* 768画素(32×24)の温度から min/max/中心/平均を求めてシリアル出力する。 */
void thermal_mlx90640_print_stats(float ta);

/* 768画素(32×24)の温度を1フレーム分、機械可読な1行(テキストCSV)で出力する。
   形式: "FRAME,<Ta_centi>,<t0_centi>,...,<t767_centi>"（各値は 1/100℃ 整数）。
   （低速。高fpsには thermal_mlx90640_send_frame_bin を使う） */
void thermal_mlx90640_print_frame(float ta);

/* 768画素を1フレーム分、バイナリで高速送出する（PRINTF を介さない）。
   形式（リトルエンディアン、計1540バイト）:
     magic   : 0xAA 0x55                 （2B）
     Ta      : int16  Ta×100[1/100℃]    （2B）
     pixels  : int16 ×768  各 t×100      （1536B）
   テキスト行(DIST/STAT等)と同一シリアルに混在するが magic で識別できる。 */
void thermal_mlx90640_send_frame_bin(float ta);

#endif /* _THERMAL_MLX90640_H_ */
