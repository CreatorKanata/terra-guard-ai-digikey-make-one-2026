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
#include "fsl_lpuart.h"
#include "fsl_gpio.h"
#include "board.h"
#include "app.h"
#include "pin_mux.h"

#include "app/sensor_bus.h"
#include "app/thermal_mlx90640.h"
#include "app/tof_vl53l5cx.h"
#include "app/bg_subtract.h"
#include "app/npu_infer.h" /* eIQ Neutron NPU カラス検出推論（2クラス） */
#include "model.h"   /* eIQ Neutron NPU 推論（TFLite Micro）: MODEL_Init/RunInference 等 */

/*******************************************************************************
 * Code
 ******************************************************************************/

/* ステータスLED（P1_22 = GPIO1[22], J3 pin3）。
   配線はシンク駆動: P3V3(J3-4) → LED → 抵抗 → P1_22(J3-3)。
   GPIO=Low で点灯、High で消灯。 */
#define STATUS_LED_GPIO GPIO1
#define STATUS_LED_PIN  22U

/* 1 にするとセンサ処理を行わず、ステータスLED(P1_22)とオンボードLEDの
   1秒点滅テストだけを実行する。配線・GPIO動作の確認用。確認後は 0 に戻す。 */
#define STATUS_LED_BLINK_TEST 0

#if STATUS_LED_BLINK_TEST
/* SDK の demo_apps/led_blinky 準拠の SysTick ベース ms ディレイ（点滅テスト専用）。
   SysTick_Config は SysTick 割り込みを有効化するため、対応する
   SysTick_Handler を必ず定義する必要がある（未定義だとデフォルト
   ハンドラの無限ループに落ちてハングする）。 */
static volatile uint32_t g_systickCounter;

void SysTick_Handler(void)
{
    if (g_systickCounter != 0U)
    {
        g_systickCounter--;
    }
}

static void SysTick_DelayTicks(uint32_t n)
{
    g_systickCounter = n;
    while (g_systickCounter != 0U)
    {
    }
}
#endif /* STATUS_LED_BLINK_TEST */

/* ステータスLED 点灯/消灯（シンク駆動なので Low=点灯、High=消灯）。 */
static inline void status_led_set(bool on)
{
    GPIO_PinWrite(STATUS_LED_GPIO, STATUS_LED_PIN, on ? 0U : 1U);
}

/* ステータスLED(P1_22) の GPIO 出力初期化（消灯状態で開始）。
   MCXN947 は GPIO ペリフェラルにクロックゲートがあり、これを有効化しないと
   GPIO レジスタへの書き込みが効かない（LEDが光らない）。GPIO1[22] なので
   kCLOCK_Gpio1 を有効化する。 */
static void status_led_init(void)
{
    CLOCK_EnableClock(kCLOCK_Gpio1);
    BOARD_InitStatusLedPin(); /* P1_22 を GPIO(MuxAlt0) に設定 */
    const gpio_pin_config_t cfg = {
        .pinDirection = kGPIO_DigitalOutput,
        .outputLogic  = 1U, /* High=消灯 で開始 */
    };
    GPIO_PinInit(STATUS_LED_GPIO, STATUS_LED_PIN, &cfg);
}

#if STATUS_LED_BLINK_TEST
/* 1秒周期で外部ステータスLED(P1_22)とオンボードLED(RED)を同タイミングで
   点滅させる配線確認テスト。戻ってこない（無限ループ）。
   SDK の led_blinky と同じく SysTick 割り込み + SysTick_DelayTicks で待つ。 */
static void status_led_blink_test(void)
{
    /* status_led_init() 内で GPIO1 クロックを有効化する。
       オンボードRED=GPIO0[10] 用に GPIO0 クロックも有効化する。
       MCXN947 は GPIO クロックゲートを有効化しないとレジスタ書き込みが効かない。 */
    CLOCK_EnableClock(kCLOCK_Gpio0);

    status_led_init();

    /* オンボードLED(RED = P0_10)を出力初期化。BOARD_InitPins で既に
       PIO0_10=GPIO に設定済み。LED_RED_INIT(1) で High(消灯)から開始。 */
    LED_RED_INIT(LOGIC_LED_OFF);

    PRINTF("\r\n=== LED 点滅テスト (外部P1_22 + オンボードRED, 1秒周期) ===\r\n");
    PRINTF("  外部配線: P3V3(J3-4) → LED → 抵抗 → P1_22(J3-3)。Low=点灯/High=消灯。\r\n");

    /* SysTick を 1ms 周期に設定。0以外なら失敗。 */
    if (SysTick_Config(SystemCoreClock / 1000U) != 0U)
    {
        PRINTF("  SysTick_Config 失敗。\r\n");
        while (1)
        {
        }
    }

    bool     on    = false;
    uint32_t count = 0;
    while (1)
    {
        SysTick_DelayTicks(500U); /* 500ms 待ち → トグルで1秒周期 */
        on = !on;
        status_led_set(on);       /* 外部LED(P1_22): Low=点灯 */
        LED_RED_TOGGLE();         /* オンボードLED(RED)も同時トグル */
        PRINTF("  LED %s (#%u)\r\n", on ? "ON " : "OFF", (unsigned int)count++);
    }
}
#endif /* STATUS_LED_BLINK_TEST */

/* カラス検出のデバウンス: crow 推論がこの回数以上「連続」したときだけ確定 crow とする。
   単発・短連続の誤検出スパイクを弾く。サーマル完成フレームは実測 約7fps なので
   10 ≒ 約1.4秒の連続 crow を要求する。静止背景で raw 判定が境界(p_crow≈0.5)で揺れても、
   10連続しなければ確定しないため誤点灯を抑える。解除は即時(not_crow が1回来たら落とす)。 */
#define CROW_CONFIRM_FRAMES 10

/* ホスト(ビューア)からの1文字コマンドを非ブロッキングで処理する。
   デバッグUART(LPUART4)の受信FIFOにバイトがある時だけ読む（ブロッキング
   GETCHAR と違いセンサのポーリングを止めない）。
   コマンド:
     'R' / 'r' : 背景モデルを今のタイミングで取り直す（bg_reset）。
                 ビューアの「背景リセット」ボタンから送られる。 */
static void poll_host_command(void)
{
    LPUART_Type *uart = (LPUART_Type *)BOARD_DEBUG_UART_BASEADDR;
    /* 受信データレジスタフル（1バイト以上受信済み）のときだけ読む。 */
    while (LPUART_GetStatusFlags(uart) & (uint32_t)kLPUART_RxDataRegFullFlag)
    {
        uint8_t ch = LPUART_ReadByte(uart);
        if (ch == 'R' || ch == 'r')
        {
            bg_reset();
            PRINTF("\r\n[bg] 背景リセット要求を受信 → 再確立中（約5〜6秒）\r\n");
        }
    }
}

int main(void)
{
    double temperature;

    BOARD_InitHardware();

    PRINTF("\r\n=== TerraGuard AI : 2センサ同時動作（サーマル + 距離） ===\r\n");

#if STATUS_LED_BLINK_TEST
    /* ステータスLED 点滅テスト（戻ってこない）。配線確認が終わったら
       STATUS_LED_BLINK_TEST を 0 に戻すこと。 */
    status_led_blink_test();
#endif

    /* --- ステータスLED(P1_22) 初期化（消灯で開始）。確定crow検出時に点灯する。 --- */
    status_led_init();

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

    /* 背景差分の状態を初期化（背景モデルは最初の数十フレームで確立）。 */
    bg_reset();

    /* --- eIQ Neutron NPU 推論モデル(カラス検出 2クラス)を初期化 --- */
    PRINTF("\r\n=== eIQ Neutron NPU 推論モデル ===\r\n");
    bool npuOk = (MODEL_Init() == kStatus_Success);
    if (npuOk)
    {
        PRINTF("NPUモデル初期化OK: %s\r\n", MODEL_GetModelName());
        /* 入力テンソル(32×32×4 int8)の形を検証。NGなら推論を無効化。 */
        npuOk = npu_infer_init();
        if (!npuOk)
        {
            PRINTF("NPU推論の入力形状検証に失敗 → 推論を無効化\r\n");
        }
    }
    else
    {
        PRINTF("NPUモデル初期化失敗\r\n");
    }

    PRINTF("\r\n初期化完了。両センサのフレームを出力します。\r\n");
    PRINTF("  距離: DIST/STAT/DFG 行（8x8）  サーマル: 0xAA55(生)/0xAA56(前景) バイナリ（32x24）\r\n");
    PRINTF("  背景差分: 起動後 約5〜6秒で背景確立。候補判定は DET 行。\r\n");

    uint32_t distFrames    = 0; /* 距離フレーム数（統計/参考温度の間引き周期に使用） */
    uint32_t thermalFrames = 0; /* サーマル完成フレーム数（統計の間引き周期に使用） */
    uint8_t  crowStreak    = 0; /* crow 推論が連続した回数（時間方向デバウンス用） */

    while (1)
    {
        bool didWork = false;
        bool thermalNew = false;  /* 今ループでサーマル完成フレームが来たか（推論トリガ） */

        /* ホストからの背景リセット等コマンドを処理（非ブロッキング）。 */
        poll_host_command();

        /* --- 距離センサ: 新フレームがあれば出力（高頻度ポーリング） --- */
        if (vlOk)
        {
            int r = tof_vl53l5cx_poll();
            if (r == 1)
            {
                /* DIST/STAT（機械可読・ビューア必須）は毎回出す。
                   人間向け統計行はシリアル帯域を食うので約10フレームに1回に間引く。 */
                tof_vl53l5cx_print_frame();
                /* 背景差分: 距離フレームを投入し、前景マップと候補要約を出力。 */
                bg_dist_update(tof_vl53l5cx_get_frame());
                bg_dist_print_frame();
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
                /* 背景差分: サーマルフレームを投入し、前景マップをバイナリ送出(0xAA56)。 */
                bg_thermal_update(thermal_mlx90640_get_frame());
                (void)bg_thermal_send_fg_bin();
                didWork = true;
                thermalNew = true;
            }
            else if (r < 0)
            {
                PRINTF("MLX90640: フレーム取得失敗 (err=%d)\r\n", r);
            }
        }

        /* 背景差分: 何かフレームを処理したら、候補判定を更新し候補なしなら背景を
           EMA更新する（候補ありなら凍結）。両センサの前景状態を見て判定する。 */
        if (didWork)
        {
            bg_apply_update_policy();
        }

        /* --- NPU カラス検出推論 --- */
        /* サーマル完成フレームごとに、4ch(サーマル/前景/距離/距離前景)が揃った
           状態で 24×24×4 入力を作り推論する。背景確立前は前景が無いのでスキップ。
           時間方向デバウンス: 生の推論が単発でcrowに振れても誤検出になりやすいので、
           crow が CROW_CONFIRM_FRAMES 連続したときだけ「確定crow」とする。
           解除は即時(not_crow が1回来たら確定を落とす)。 */
        if (npuOk && thermalNew && bg_thermal_ready())
        {
            npu_result_t res;
            if (npu_infer_run(thermal_mlx90640_get_frame(), bg_thermal_fg(),
                              tof_vl53l5cx_get_frame(), bg_dist_fg(), &res))
            {
                if (res.crow)
                {
                    if (crowStreak < 0xFF) crowStreak++;
                }
                else
                {
                    crowStreak = 0;
                }
                bool confirmed = (crowStreak >= CROW_CONFIRM_FRAMES);

                /* ステータスLED(P1_22): 確定crow の間だけ点灯、解除されたら消灯。
                   デバウンス済みの confirmed をそのまま反映する（シンク駆動 Low=点灯）。 */
                status_led_set(confirmed);

                /* 機械可読: INFER,<確定crow 0/1>,<p_crow x1000>,<conf x1000>,<raw crow>,<streak>
                   確定crowはデバウンス後の判定。raw/streak はデバッグ・ビューア表示用。 */
                PRINTF("INFER,%d,%d,%d,%d,%d\r\n", confirmed ? 1 : 0,
                       (int)(res.p_crow * 1000.0f),
                       (int)(res.confidence * 1000.0f),
                       res.crow ? 1 : 0, (int)crowStreak);
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
