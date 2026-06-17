/*
 * TerraGuard AI — FRDM-MCXN947
 * メインオーケストレータ。サーマル(MLX90640)と距離(VL53L5CX)の2センサを
 * 同一の外部I²Cバス(LPI2C2/FC2)上で同時動作させ、両方のデータを同一シリアルへ
 * リアルタイム出力する。
 *
 *   1) 外部I²Cバス(LPI2C2/FLEXCOMM2, J8 pin3/4)を初期化してアドレススキャンし、
 *      MLX90640(0x33) / VL53L5CX(0x29) が ACK を返すか疎通確認する。
 *   2) オンボード温度センサ P3T1755DP(I3C) を初期化（参考値として時々表示）。
 *   3) 両センサを初期化し、1ループで:
 *        - VL53L5CX を高頻度ポーリング → 新フレームで DIST/STAT 出力
 *        - MLX90640 は data-ready を非ブロッキング確認 → 揃ったら FRAME 出力
 *
 * 両センサはアドレスが異なる(0x33 / 0x29)ため同一バスで共存できる。アクセスは
 * シーケンシャル（ブロッキング転送を順番に）なのでバス衝突は起きない。MLX の
 * フレーム取得は data-ready 確認後のみ実行し、VL53 のポーリングを阻害しない。
 *
 * 各センサのドライバは app/ 配下のモジュールに分離している:
 *   - app/sensor_bus.{c,h}       : I3C/P3T1755/LPI2C 共通基盤・バススキャン・温度出力
 *   - app/thermal_mlx90640.{c,h} : MLX90640 サーマルセンサ
 *   - app/tof_vl53l5cx.{c,h}     : VL53L5CX ToF距離センサ
 *
 * 出力フォーマット（同一シリアル上で行頭マーカで区別。受信側 dual_viewer.py が分離）:
 *   - "FRAME,<Ta_centi>,<t0>,...,<t767>"  サーマル32×24（1/100℃）
 *   - "DIST,<z0>,...,<z63>"               距離8×8（mm）
 *   - "STAT,<s0>,...,<s63>"               距離8×8の target_status
 *
 * - 外部I²C: LPI2C2 / FLEXCOMM2（P4_0=SDA, P4_1=SCL）。J8 pin1=3.3V/pin2=GND/pin3=SCL/pin4=SDA。
 * - 温度センサ: P3T1755DP（I3C1, 動的アドレス割当, レジスタアクセスは 0x08）
 * - 出力: MCU-Link 仮想COM (PRINTF / デバッグUART, 115200)
 *
 * Copyright 2022, 2025 NXP / 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fsl_debug_console.h"
#include "board.h"
#include "app.h"

#include "app/sensor_bus.h"
#include "app/thermal_mlx90640.h"
#include "app/tof_vl53l5cx.h"

/*******************************************************************************
 * Code
 ******************************************************************************/
int main(void)
{
    double temperature;

    BOARD_InitHardware();

    PRINTF("\r\n=== TerraGuard AI : 2センサ同時動作（サーマル + 距離） ===\r\n");

    /* --- 外部I²C(LPI2C2/J8) 疎通確認: バススキャンで MLX90640/VL53L5CX の ACK を確認 --- */
    sensor_i2c_master_init();
    sensor_i2c_bus_scan();

    /* --- オンボード温度センサ P3T1755 (I3C) 初期化 --- */
    PRINTF("\r\n=== オンボード温度センサ P3T1755 (I3C) ===\r\n");
    (void)sensor_p3t1755_init();

    /* --- 距離センサ VL53L5CX を先に初期化（FW転送が重いので最初に1回だけ） --- */
    PRINTF("\r\n=== ToF距離センサ VL53L5CX (I2C, 0x29) ===\r\n");
    bool vlOk = tof_vl53l5cx_setup();

    /* --- サーマルセンサ MLX90640 を初期化 --- */
    PRINTF("\r\n=== サーマルセンサ MLX90640 (I2C, 0x33) ===\r\n");
    bool mlxOk = thermal_mlx90640_setup();

    PRINTF("\r\n初期化完了。両センサのフレームを出力します。\r\n");
    PRINTF("  距離: DIST/STAT 行（8x8）  サーマル: FRAME 行（32x24）\r\n");

    uint32_t distFrames    = 0; /* 距離フレーム数（統計/参考温度の間引き周期に使用） */
    uint32_t thermalFrames = 0; /* サーマル完成フレーム数（統計の間引き周期に使用） */

    while (1)
    {
        bool didWork = false;

        /* --- 距離センサ: 新フレームがあれば出力（高頻度ポーリング） --- */
        if (vlOk)
        {
            int r = tof_vl53l5cx_poll();
            if (r == 1)
            {
                /* DIST/STAT（機械可読・ビューア必須）は毎回出す。
                   人間向け統計行はシリアル帯域を食うので約10フレームに1回に間引く。 */
                tof_vl53l5cx_print_frame();
                distFrames++;
                didWork = true;

                if (distFrames % 10U == 0U)
                {
                    tof_vl53l5cx_print_stats();
                }
                /* 約100距離フレームごとに、参考としてオンボード温度センサ(I3C)も表示。 */
                if (distFrames % 100U == 0U)
                {
                    if (sensor_p3t1755_read(&temperature) == kStatus_Success)
                    {
                        sensor_print_temp_c("  [ref] オンボード温度センサ P3T1755 = ", (float)temperature);
                        PRINTF("\r\n");
                    }
                }
            }
            else if (r < 0)
            {
                PRINTF("VL53L5CX: データ取得失敗 (status=%d)\r\n", -r);
            }
        }

        /* --- サーマルセンサ: 両サブページ(0/1)が揃った完成フレームのみ出力 --- */
        /* poll_frame は data-ready 確認込みで非ブロッキング。subpage 0/1 が揃ったら 1 を返す。
           完成フレームだけ描画することで、動体時の市松模様(片サブページ遅延)を解消する。 */
        if (mlxOk)
        {
            int r = thermal_mlx90640_poll_frame();
            if (r == 1)
            {
                float ta = thermal_mlx90640_get_ta();
                /* フレーム本体はバイナリ送出（PRINTF の書式変換コストを避け高速化）。
                   人間向け統計行は約10フレームに1回に間引く。 */
                thermalFrames++;
                if (thermalFrames % 10U == 0U)
                {
                    thermal_mlx90640_print_stats(ta);
                }
                thermal_mlx90640_send_frame_bin(ta); /* magic 0xAA55 + Ta + 768×int16 */
                didWork = true;
            }
            else if (r < 0)
            {
                PRINTF("MLX90640: フレーム取得失敗 (err=%d)\r\n", r);
            }
        }

        /* 両センサとも初期化失敗、または今ループで何も処理しなかった場合は短く待つ
           （VL53 10Hz / MLX 2Hz に対し過剰ポーリングでバスを飽和させない）。 */
        if (!didWork)
        {
            SDK_DelayAtLeastUs((!vlOk && !mlxOk) ? 1000000U : 5000U, CLOCK_GetCoreSysClkFreq());
        }
    }
}
