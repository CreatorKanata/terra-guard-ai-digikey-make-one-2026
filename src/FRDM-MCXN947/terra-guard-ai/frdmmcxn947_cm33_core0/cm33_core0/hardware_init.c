/*
 * Copyright 2022 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*${header:start}*/
#include "pin_mux.h"
#include "peripherals.h"
#include "board.h"
/*${header:end}*/

/*${function:start}*/
void BOARD_InitHardware(void)
{
    CLOCK_EnableClock(kCLOCK_Gpio0);
    BOARD_InitPins();
    BOARD_BootClockFRO12M();
    /* SysTick を 0.5 秒間隔で割り込ませる（12MHz × 0.5s = 6,000,000）。
       SysTick_Handler で LED をトグルするので、点滅周期は 1 秒（ON 0.5s + OFF 0.5s）。 */
    SysTick_Config(6000000UL);
    LED_RED_INIT(LOGIC_LED_OFF);
}
/*${function:end}*/
