/*
 * TerraGuard AI — FRDM-MCXN947
 * ボード初期化: クロック(UART/I3C) / ピン(UART/I3C) / デバッグコンソール。
 * NXP の master_read_sensor_p3t1755 サンプルに準拠。
 *
 * Copyright 2022-2023, 2025 NXP / 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*${header:start}*/
#include "pin_mux.h"
#include "clock_config.h"
#include "board.h"
/*${header:end}*/

/*${function:start}*/
void BOARD_InitHardware(void)
{
    /* デバッグコンソール(FLEXCOMM4)へ FRO12M をアタッチ */
    CLOCK_SetClkDiv(kCLOCK_DivFlexcom4Clk, 1);
    CLOCK_AttachClk(BOARD_DEBUG_UART_CLK_ATTACH);

    /* I3C1 へ PLL0 をアタッチ（150MHz / 6 = 25MHz） */
    CLOCK_SetClkDiv(kCLOCK_DivI3c1FClk, 6U);
    CLOCK_AttachClk(kPLL0_to_I3C1FCLK);

    BOARD_InitBootPins();   /* LED/UART等の基本ピン */
    BOARD_InitI3CPins();    /* I3C1 (P1_11/16/17) ピン */
    BOARD_InitBootClocks(); /* PLL150M（I3CがPLL0を使うため必須） */
    BOARD_InitDebugConsole();
}
/*${function:end}*/
