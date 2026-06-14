/*
 * TerraGuard AI — FRDM-MCXN947
 * オンボード温度センサ P3T1755DP の値を I3C で読み、1秒周期でシリアル出力する。
 *
 * - 温度センサ: P3T1755DP（I3C1, 動的アドレス割当, レジスタアクセスは 0x08）
 * - 出力: MCU-Link 仮想COM (PRINTF / デバッグUART, 115200)
 *
 * NXP の I3C master_read_sensor_p3t1755 サンプルをベースに移植。
 *
 * Copyright 2022, 2025 NXP / 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include "fsl_debug_console.h"
#include "fsl_p3t1755.h"
#include "fsl_i3c.h"
#include "board.h"
#include "app.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/
/* I3C ボーレート（サンプルの Kconfig デフォルト値を直接定義） */
#define EXAMPLE_I2C_BAUDRATE    400000U
#define EXAMPLE_I3C_OD_BAUDRATE 1500000U
#define EXAMPLE_I3C_PP_BAUDRATE 4000000U

#define I3C_TIME_OUT_INDEX 100000000U

/* P3T1755 のレジスタアクセス用アドレス（動的アドレス割当後） */
#define SENSOR_ADDR 0x08U
#define CCC_RSTDAA  0x06U
#define CCC_SETDASA 0x87

/*******************************************************************************
 * Variables
 ******************************************************************************/
volatile status_t g_completionStatus;
volatile bool g_masterCompletionFlag;
i3c_master_handle_t g_i3c_m_handle;
p3t1755_handle_t p3t1755Handle;

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void i3c_master_callback(I3C_Type *base, i3c_master_handle_t *handle, status_t status, void *userData);

const i3c_master_transfer_callback_t masterCallback = {
    .slave2Master = NULL, .ibiCallback = NULL, .transferComplete = i3c_master_callback};

/*******************************************************************************
 * Code
 ******************************************************************************/
static void i3c_master_callback(I3C_Type *base, i3c_master_handle_t *handle, status_t status, void *userData)
{
    if (status == kStatus_Success)
    {
        g_masterCompletionFlag = true;
    }
    g_completionStatus = status;
}

/* I3C でセンサのレジスタへ書き込み */
status_t I3C_WriteSensor(uint8_t deviceAddress, uint32_t regAddress, uint8_t *regData, size_t dataSize)
{
    status_t result                  = kStatus_Success;
    i3c_master_transfer_t masterXfer = {0};
    uint32_t timeout                 = 0U;

    masterXfer.slaveAddress   = deviceAddress;
    masterXfer.direction      = kI3C_Write;
    masterXfer.busType        = kI3C_TypeI3CSdr;
    masterXfer.subaddress     = regAddress;
    masterXfer.subaddressSize = 1;
    masterXfer.data           = regData;
    masterXfer.dataSize       = dataSize;
    masterXfer.flags          = kI3C_TransferDefaultFlag;

    g_masterCompletionFlag = false;
    g_completionStatus     = kStatus_Success;
    result                 = I3C_MasterTransferNonBlocking(EXAMPLE_MASTER, &g_i3c_m_handle, &masterXfer);
    if (kStatus_Success != result)
    {
        return result;
    }

    while (!g_masterCompletionFlag)
    {
        timeout++;
        if ((g_completionStatus != kStatus_Success) || (timeout > I3C_TIME_OUT_INDEX))
        {
            break;
        }
    }

    if (timeout == I3C_TIME_OUT_INDEX)
    {
        result = kStatus_Timeout;
    }
    result = g_completionStatus;

    return result;
}

/* I3C でセンサのレジスタから読み出し */
status_t I3C_ReadSensor(uint8_t deviceAddress, uint32_t regAddress, uint8_t *regData, size_t dataSize)
{
    status_t result                  = kStatus_Success;
    i3c_master_transfer_t masterXfer = {0};
    uint32_t timeout                 = 0U;

    masterXfer.slaveAddress   = deviceAddress;
    masterXfer.direction      = kI3C_Read;
    masterXfer.busType        = kI3C_TypeI3CSdr;
    masterXfer.subaddress     = regAddress;
    masterXfer.subaddressSize = 1;
    masterXfer.data           = regData;
    masterXfer.dataSize       = dataSize;
    masterXfer.flags          = kI3C_TransferDefaultFlag;

    g_masterCompletionFlag = false;
    g_completionStatus     = kStatus_Success;
    result                 = I3C_MasterTransferNonBlocking(EXAMPLE_MASTER, &g_i3c_m_handle, &masterXfer);
    if (kStatus_Success != result)
    {
        return result;
    }

    while (!g_masterCompletionFlag)
    {
        timeout++;
        if ((g_completionStatus != kStatus_Success) || (timeout > I3C_TIME_OUT_INDEX))
        {
            break;
        }
    }

    if (timeout == I3C_TIME_OUT_INDEX)
    {
        result = kStatus_Timeout;
    }
    result = g_completionStatus;

    return result;
}

/* P3T1755 へ I3C 動的アドレスを割り当てる（RSTDAA → SETDASA） */
status_t p3t1755_set_dynamic_address(void)
{
    status_t result                  = kStatus_Success;
    i3c_master_transfer_t masterXfer = {0};
    uint8_t g_master_txBuff[1];

    /* 動的アドレスをリセット */
    g_master_txBuff[0]      = CCC_RSTDAA;
    masterXfer.slaveAddress = 0x7E;
    masterXfer.data         = g_master_txBuff;
    masterXfer.dataSize     = 1;
    masterXfer.direction    = kI3C_Write;
    masterXfer.busType      = kI3C_TypeI3CSdr;
    masterXfer.flags        = kI3C_TransferDefaultFlag;
    result                  = I3C_MasterTransferBlocking(EXAMPLE_MASTER, &masterXfer);
    if (result != kStatus_Success)
    {
        return result;
    }

    /* 動的アドレスを割り当て */
    memset(&masterXfer, 0, sizeof(masterXfer));
    g_master_txBuff[0]      = CCC_SETDASA;
    masterXfer.slaveAddress = 0x7E;
    masterXfer.data         = g_master_txBuff;
    masterXfer.dataSize     = 1;
    masterXfer.direction    = kI3C_Write;
    masterXfer.busType      = kI3C_TypeI3CSdr;
    masterXfer.flags        = kI3C_TransferNoStopFlag;
    result                  = I3C_MasterTransferBlocking(EXAMPLE_MASTER, &masterXfer);
    if (result != kStatus_Success)
    {
        return result;
    }

    memset(&masterXfer, 0, sizeof(masterXfer));
    g_master_txBuff[0]      = SENSOR_ADDR << 1;
    masterXfer.slaveAddress = SENSOR_SLAVE_ADDR;
    masterXfer.data         = g_master_txBuff;
    masterXfer.dataSize     = 1;
    masterXfer.direction    = kI3C_Write;
    masterXfer.busType      = kI3C_TypeI3CSdr;
    masterXfer.flags        = kI3C_TransferDefaultFlag;
    return I3C_MasterTransferBlocking(EXAMPLE_MASTER, &masterXfer);
}

/*!
 * @brief Main function
 */
int main(void)
{
    status_t result = kStatus_Success;
    i3c_master_config_t masterConfig;
    p3t1755_config_t p3t1755Config;
    double temperature;

    BOARD_InitHardware();

    PRINTF("\r\n=== TerraGuard AI : P3T1755 温度センサ (I3C) ===\r\n");

    /* I3C マスタ初期化 */
    I3C_MasterGetDefaultConfig(&masterConfig);
    masterConfig.baudRate_Hz.i2cBaud          = EXAMPLE_I2C_BAUDRATE;
    masterConfig.baudRate_Hz.i3cPushPullBaud  = EXAMPLE_I3C_PP_BAUDRATE;
    masterConfig.baudRate_Hz.i3cOpenDrainBaud = EXAMPLE_I3C_OD_BAUDRATE;
    masterConfig.enableOpenDrainStop          = false;
    masterConfig.disableTimeout               = true;
    I3C_MasterInit(EXAMPLE_MASTER, &masterConfig, I3C_MASTER_CLOCK_FREQUENCY);

    /* I3C ハンドル作成 */
    I3C_MasterTransferCreateHandle(EXAMPLE_MASTER, &g_i3c_m_handle, &masterCallback, NULL);

    /* P3T1755 へ動的アドレス割り当て */
    result = p3t1755_set_dynamic_address();
    if (result != kStatus_Success)
    {
        PRINTF("P3T1755 動的アドレス割り当て失敗\r\n");
    }

    /* P3T1755 ドライバ初期化 */
    p3t1755Config.writeTransfer = I3C_WriteSensor;
    p3t1755Config.readTransfer  = I3C_ReadSensor;
    p3t1755Config.sensorAddress = SENSOR_ADDR;
    p3t1755Config.oneshotMode   = false;
    P3T1755_Init(&p3t1755Handle, &p3t1755Config);

    PRINTF("初期化完了。1秒周期で温度を表示します。\r\n");

    while (1)
    {
        result = P3T1755_ReadTemperature(&p3t1755Handle, &temperature);
        if (result != kStatus_Success)
        {
            PRINTF("温度読み取り失敗\r\n");
        }
        else
        {
            /* debug_console_lite は %f 非対応のため、整数演算で小数2桁を出す。
               例: 28.81℃ → "28.81 C" */
            int32_t milliC = (int32_t)(temperature * 100.0); /* 1/100℃単位 */
            int32_t intPart  = milliC / 100;
            int32_t fracPart = milliC % 100;
            if (fracPart < 0)
            {
                fracPart = -fracPart;
            }
            PRINTF("Temperature: %d.%02d C\r\n", intPart, fracPart);
        }
        SDK_DelayAtLeastUs(1000000, CLOCK_GetCoreSysClkFreq());
    }
}
