/*
 * Copyright 2022 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _APP_H_
#define _APP_H_

/*${header:start}*/
#include "pin_mux.h"
/*${header:end}*/

/*******************************************************************************
 * Definitions
 ******************************************************************************/
/*${macro:start}*/
#define BOARD_LED_GPIO     BOARD_LED_RED_GPIO
#define BOARD_LED_GPIO_PIN BOARD_LED_RED_GPIO_PIN

/* オンボード温度センサ P3T1755DP（I3C1, アドレス0x48） */
#define EXAMPLE_MASTER             I3C1
#define I3C_MASTER_CLOCK_FREQUENCY CLOCK_GetI3cClkFreq(1)
#define SENSOR_SLAVE_ADDR          0x48U

/* 外部I²Cセンサ用 LPI2C2 / FLEXCOMM2（J8 pin3/4 = J2 pin18/20, P4_0/P4_1）
   J8 pin1=3.3V, pin2=GND, pin3=SCL(P4_1), pin4=SDA(P4_0)。 */
#define EXAMPLE_I2C_MASTER_BASE      LPI2C2
#define EXAMPLE_I2C_MASTER           ((LPI2C_Type *)EXAMPLE_I2C_MASTER_BASE)
#define I2C_MASTER_CLOCK_FREQUENCY   CLOCK_GetLPFlexCommClkFreq(2u)
#define EXAMPLE_I2C_BAUDRATE_HZ      100000U /* 疎通確認は 100kHz で安全側に */
#define MLX90640_I2C_ADDR            0x33U   /* サーマルセンサ MLX90640 の I²Cアドレス */
/*${macro:end}*/

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
/*${prototype:start}*/
void BOARD_InitHardware(void);
/*${prototype:end}*/

#endif /* _APP_H_ */
