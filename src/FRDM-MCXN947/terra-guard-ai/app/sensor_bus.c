/*
 * TerraGuard AI — FRDM-MCXN947
 * sensor_bus: センサ共通基盤の実装
 *
 * NXP の I3C master_read_sensor_p3t1755 / lpi2c polling_b2b_master サンプルをベースに移植。
 *
 * Copyright 2022, 2025 NXP / 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include "sensor_bus.h"
#include "fsl_debug_console.h"
#include "fsl_i3c.h"
#include "fsl_lpi2c.h"
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
static volatile status_t   s_completionStatus;
static volatile bool       s_masterCompletionFlag;
static i3c_master_handle_t s_i3c_m_handle;
static p3t1755_handle_t    s_p3t1755Handle;

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void i3c_master_callback(I3C_Type *base, i3c_master_handle_t *handle, status_t status, void *userData);

static const i3c_master_transfer_callback_t s_masterCallback = {
    .slave2Master = NULL, .ibiCallback = NULL, .transferComplete = i3c_master_callback};

/*******************************************************************************
 * 共通ユーティリティ
 ******************************************************************************/
void sensor_print_temp_c(const char *label, float celsius)
{
    int32_t centi = (int32_t)(celsius * 100.0f + (celsius >= 0 ? 0.5f : -0.5f));
    int32_t ip    = centi / 100;
    int32_t fp    = centi % 100;
    if (fp < 0)
    {
        fp = -fp;
    }
    /* -0.xx のとき整数部が 0 でも符号を保持 */
    if (centi < 0 && ip == 0)
    {
        PRINTF("%s-0.%02d C", label, fp);
    }
    else
    {
        PRINTF("%s%d.%02d C", label, ip, fp);
    }
}

void sensor_write_raw(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        /* DbgConsole_Putchar は1バイトをブロッキング送信する（PUTCHAR マクロの実体）。
           書式変換が無いぶん PRINTF("%d") より大幅に軽い。 */
        (void)PUTCHAR((int)data[i]);
    }
}

/*******************************************************************************
 * I3C コールバック / 低レベルアクセス
 ******************************************************************************/
static void i3c_master_callback(I3C_Type *base, i3c_master_handle_t *handle, status_t status, void *userData)
{
    if (status == kStatus_Success)
    {
        s_masterCompletionFlag = true;
    }
    s_completionStatus = status;
}

status_t sensor_i3c_write(uint8_t deviceAddress, uint32_t regAddress, uint8_t *regData, size_t dataSize)
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

    s_masterCompletionFlag = false;
    s_completionStatus     = kStatus_Success;
    result                 = I3C_MasterTransferNonBlocking(EXAMPLE_MASTER, &s_i3c_m_handle, &masterXfer);
    if (kStatus_Success != result)
    {
        return result;
    }

    while (!s_masterCompletionFlag)
    {
        timeout++;
        if ((s_completionStatus != kStatus_Success) || (timeout > I3C_TIME_OUT_INDEX))
        {
            break;
        }
    }

    if (timeout == I3C_TIME_OUT_INDEX)
    {
        result = kStatus_Timeout;
    }
    result = s_completionStatus;

    return result;
}

status_t sensor_i3c_read(uint8_t deviceAddress, uint32_t regAddress, uint8_t *regData, size_t dataSize)
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

    s_masterCompletionFlag = false;
    s_completionStatus     = kStatus_Success;
    result                 = I3C_MasterTransferNonBlocking(EXAMPLE_MASTER, &s_i3c_m_handle, &masterXfer);
    if (kStatus_Success != result)
    {
        return result;
    }

    while (!s_masterCompletionFlag)
    {
        timeout++;
        if ((s_completionStatus != kStatus_Success) || (timeout > I3C_TIME_OUT_INDEX))
        {
            break;
        }
    }

    if (timeout == I3C_TIME_OUT_INDEX)
    {
        result = kStatus_Timeout;
    }
    result = s_completionStatus;

    return result;
}

/* P3T1755 へ I3C 動的アドレスを割り当てる（RSTDAA → SETDASA） */
static status_t p3t1755_set_dynamic_address(void)
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

/*******************************************************************************
 * 外部I²C(LPI2C2)
 ******************************************************************************/
/* 指定ボーレートで LPI2C マスタを初期化する内部ヘルパ。 */
static void i2c_master_init_baud(uint32_t baudHz)
{
    lpi2c_master_config_t masterConfig;

    /* デフォルト設定: 7bitアドレス, 2線オープンドレイン, ignoreAck=false */
    LPI2C_MasterGetDefaultConfig(&masterConfig);
    masterConfig.baudRate_Hz = baudHz;
    LPI2C_MasterInit(EXAMPLE_I2C_MASTER, &masterConfig, I2C_MASTER_CLOCK_FREQUENCY);
}

void sensor_i2c_master_init(void)
{
    /* 通常運用は app.h の EXAMPLE_I2C_BAUDRATE_HZ（MLX90640 32Hz 用に 1MHz）。 */
    i2c_master_init_baud(EXAMPLE_I2C_BAUDRATE_HZ);
}

/* 指定アドレスに START(書き込み) を送り、ACK が返れば true。
   バスにデバイスが居るかを非破壊で確認する。
   LPI2C は START/転送が非同期なので、エラーフラグのクリア → START →
   NACK フラグ確認 → 必ず STOP、の順で「固まらない」ように堅牢化する。
   （旧実装は START の戻り値だけ見て STOP していたため、バスが clean でない
     起動直後などに probe が固まることがあった） */
static bool i2c_probe_addr(uint8_t addr7)
{
    /* 前回の残存エラー(NACK/Arb lost 等)をクリアしておく。 */
    uint32_t flags = LPI2C_MasterGetStatusFlags(EXAMPLE_I2C_MASTER);
    LPI2C_MasterClearStatusFlags(EXAMPLE_I2C_MASTER,
                                 flags & (kLPI2C_MasterNackDetectFlag |
                                          kLPI2C_MasterArbitrationLostFlag |
                                          kLPI2C_MasterFifoErrFlag |
                                          kLPI2C_MasterPinLowTimeoutFlag));

    status_t result = LPI2C_MasterStart(EXAMPLE_I2C_MASTER, addr7, kLPI2C_Write);
    if (result == kStatus_Success)
    {
        /* アドレス送出の完了（TX FIFO 空）を待ち、その時点の NACK 有無で判定。
           タイムアウトを設けてバスがロックしても固まらないようにする。 */
        uint32_t guard = 0U;
        bool nack = false;
        while (guard++ < 100000U)
        {
            uint32_t s = LPI2C_MasterGetStatusFlags(EXAMPLE_I2C_MASTER);
            if (s & kLPI2C_MasterNackDetectFlag)
            {
                nack = true;
                break;
            }
            if (s & kLPI2C_MasterTxReadyFlag) /* アドレス送出が捌けた=ACKされた */
            {
                break;
            }
        }
        result = nack ? kStatus_Fail : kStatus_Success;
    }

    /* どの経路でも必ず STOP でバスを解放（戻り値は問わない）。 */
    (void)LPI2C_MasterStop(EXAMPLE_I2C_MASTER);
    /* probe で立った NACK フラグを後段に持ち越さないようクリア。 */
    LPI2C_MasterClearStatusFlags(EXAMPLE_I2C_MASTER, kLPI2C_MasterNackDetectFlag);

    return (result == kStatus_Success);
}

void sensor_i2c_bus_scan(void)
{
    PRINTF("\r\n--- 外部I2Cバススキャン (LPI2C2 / J8 pin3=SCL,4=SDA) ---\r\n");

    int found = 0;
    for (uint8_t addr = 0x08U; addr <= 0x77U; addr++)
    {
        if (i2c_probe_addr(addr))
        {
            found++;
            PRINTF("  ACK: 0x%02X", addr);
            if (addr == MLX90640_I2C_ADDR)
            {
                PRINTF("  <- MLX90640 (サーマルセンサ) 検出");
            }
            PRINTF("\r\n");
        }
    }

    if (found == 0)
    {
        PRINTF("  デバイス未検出。配線(SDA/SCL/3.3V/GND)・電源・プルアップを確認。\r\n");
    }
    else
    {
        PRINTF("  検出デバイス数: %d\r\n", found);
    }
    PRINTF("--- スキャン完了 ---\r\n");
}

/*******************************************************************************
 * オンボード温度センサ P3T1755（I3C1）
 ******************************************************************************/
status_t sensor_p3t1755_init(void)
{
    status_t result = kStatus_Success;
    i3c_master_config_t masterConfig;
    p3t1755_config_t p3t1755Config;

    /* I3C マスタ初期化 */
    I3C_MasterGetDefaultConfig(&masterConfig);
    masterConfig.baudRate_Hz.i2cBaud          = EXAMPLE_I2C_BAUDRATE;
    masterConfig.baudRate_Hz.i3cPushPullBaud  = EXAMPLE_I3C_PP_BAUDRATE;
    masterConfig.baudRate_Hz.i3cOpenDrainBaud = EXAMPLE_I3C_OD_BAUDRATE;
    masterConfig.enableOpenDrainStop          = false;
    masterConfig.disableTimeout               = true;
    I3C_MasterInit(EXAMPLE_MASTER, &masterConfig, I3C_MASTER_CLOCK_FREQUENCY);

    /* I3C ハンドル作成 */
    I3C_MasterTransferCreateHandle(EXAMPLE_MASTER, &s_i3c_m_handle, &s_masterCallback, NULL);

    /* P3T1755 へ動的アドレス割り当て */
    result = p3t1755_set_dynamic_address();
    if (result != kStatus_Success)
    {
        PRINTF("P3T1755 動的アドレス割り当て失敗\r\n");
        return result;
    }

    /* P3T1755 ドライバ初期化 */
    p3t1755Config.writeTransfer = sensor_i3c_write;
    p3t1755Config.readTransfer  = sensor_i3c_read;
    p3t1755Config.sensorAddress = SENSOR_ADDR;
    p3t1755Config.oneshotMode   = false;
    P3T1755_Init(&s_p3t1755Handle, &p3t1755Config);

    return kStatus_Success;
}

status_t sensor_p3t1755_read(double *temperature)
{
    return P3T1755_ReadTemperature(&s_p3t1755Handle, temperature);
}
