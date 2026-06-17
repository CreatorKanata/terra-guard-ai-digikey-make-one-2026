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
    BOARD_InitI2CPins();    /* LPI2C2 (P4_0/P4_1) ピン */
    BOARD_InitBootClocks(); /* PLL150M / FRO_HF 等を確定（I3CがPLL0を使うため必須） */

    /* 外部I²Cセンサ用 FLEXCOMM2(LPI2C2) のクロック源（J8 pin3/4 = J2 pin18/20, P4_0/P4_1）。
       FRO12M(12MHz) では 1MHz SCL を 12分周でしか作れず波形が FM+ 規格を満たさず、
       MLX90640/VL53L5CX(ともに最大1MHz)が高速で ACK できなかった。
       FRO_HF(48MHz) を FRO_HF_DIV で ÷2=24MHz にして FC2 へ供給し、1MHz SCL を
       24分周で余裕を持って生成する。BootClocks で FRO_HF が確定した後にアタッチする。 */
    CLOCK_SetClkDiv(kCLOCK_DivFrohfClk, 2U);   /* FRO_HF_DIV = FRO_HF/2 = 24MHz */
    CLOCK_SetClkDiv(kCLOCK_DivFlexcom2Clk, 1U);
    CLOCK_AttachClk(kFRO_HF_DIV_to_FLEXCOMM2);

    BOARD_InitDebugConsole();
}
/*${function:end}*/
