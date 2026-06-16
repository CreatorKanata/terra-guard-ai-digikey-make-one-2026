/*
 * TerraGuard AI — FRDM-MCXN947
 * 1) 外部I²Cバス(LPI2C2/FLEXCOMM2, J8 pin3/4)を初期化してアドレススキャンし、
 *    サーマルセンサ MLX90640(0x33) 等が ACK を返すか疎通確認する。
 * 2) オンボード温度センサ P3T1755DP の値を I3C で読み、1秒周期でシリアル出力する。
 *
 * - 外部I²C: LPI2C2 / FLEXCOMM2（P4_0=SDA, P4_1=SCL）。J8 pin1=3.3V/pin2=GND/pin3=SCL/pin4=SDA。
 * - 温度センサ: P3T1755DP（I3C1, 動的アドレス割当, レジスタアクセスは 0x08）
 * - 出力: MCU-Link 仮想COM (PRINTF / デバッグUART, 115200)
 *
 * NXP の I3C master_read_sensor_p3t1755 / lpi2c polling_b2b_master サンプルをベースに移植。
 *
 * Copyright 2022, 2025 NXP / 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include "fsl_debug_console.h"
#include "fsl_p3t1755.h"
#include "fsl_i3c.h"
#include "fsl_lpi2c.h"
#include "board.h"
#include "app.h"
#include <MLX90640_API.h>
#include <MLX90640_I2C_Driver.h>

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

/* --- MLX90640 サーマルセンサ設定 --- */
#define MLX90640_REFRESH_2HZ  0x03U  /* Control1 のリフレッシュレート: 0x03=2Hz */
#define MLX90640_EMISSIVITY   0.95f  /* 放射率（一般物体・生物体向け推奨） */
#define MLX90640_TR_OFFSET    8.0f   /* 反射温度 tr = Ta - 8℃（公式サンプル推奨） */
#define MLX90640_FRAME_WORDS  834U   /* GetFrameData が要求するワード数 */

/*******************************************************************************
 * Variables
 ******************************************************************************/
volatile status_t g_completionStatus;
volatile bool g_masterCompletionFlag;
i3c_master_handle_t g_i3c_m_handle;
p3t1755_handle_t p3t1755Handle;

/* MLX90640 用バッファ（大きいのでファイルスコープ static に置く）
 *   eeData    : EEPROM ダンプ 832 ワード
 *   mlxParams : 展開済み校正パラメータ（約 2.5KB）
 *   frameData : 1サブページ分の生フレーム 834 ワード
 *   mlxTo     : 変換後の温度[℃] 768 画素 */
static uint16_t       s_mlxEeData[832];
static paramsMLX90640 s_mlxParams;
static uint16_t       s_mlxFrame[MLX90640_FRAME_WORDS];
static float          s_mlxTo[768];

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

/* 外部I²Cマスタ(LPI2C2)を初期化する */
static void i2c_master_init(void)
{
    lpi2c_master_config_t masterConfig;

    /* デフォルト設定: 7bitアドレス, 2線オープンドレイン, ignoreAck=false */
    LPI2C_MasterGetDefaultConfig(&masterConfig);
    masterConfig.baudRate_Hz = EXAMPLE_I2C_BAUDRATE_HZ;
    LPI2C_MasterInit(EXAMPLE_I2C_MASTER, &masterConfig, I2C_MASTER_CLOCK_FREQUENCY);
}

/* 指定アドレスに START(書き込み) を送り、ACK が返れば true。
   バスにデバイスが居るかを非破壊で確認するための定番手法。 */
static bool i2c_probe_addr(uint8_t addr7)
{
    status_t result = LPI2C_MasterStart(EXAMPLE_I2C_MASTER, addr7, kLPI2C_Write);
    if (result == kStatus_Success)
    {
        /* START 自体は成功。STOP の戻りで ACK/NACK を判定する。 */
        result = LPI2C_MasterStop(EXAMPLE_I2C_MASTER);
    }
    else
    {
        /* START 段階で NACK 等。念のため STOP でバスを解放する。 */
        (void)LPI2C_MasterStop(EXAMPLE_I2C_MASTER);
    }
    return (result == kStatus_Success);
}

/* I²Cバス(0x08〜0x77)をスキャンし、ACK を返すアドレスを列挙する。
   MLX90640(0x33) が見つかれば疎通OKとみなす。 */
static void i2c_bus_scan(void)
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

/* float 温度[℃] を "%d.%02d ℃" 形式で出力する（debug_console_lite は %f 非対応）。
   負値も符号付きで正しく出す。 */
static void print_temp_c(const char *label, float celsius)
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

/* MLX90640 を初期化（2Hz/Chess 設定 → EEPROM 読み出し → 校正パラメータ展開）。
   成功で true。 */
static bool mlx90640_setup(void)
{
    int err;

    /* スキャン(低レベル Start/Stop)後のバス状態をクリーンにするため LPI2C を再初期化。 */
    i2c_master_init();

    /* リフレッシュレート 2Hz */
    err = MLX90640_SetRefreshRate(MLX90640_I2C_ADDR, MLX90640_REFRESH_2HZ);
    if (err != MLX90640_NO_ERROR)
    {
        PRINTF("MLX90640: リフレッシュレート設定失敗 (err=%d)\r\n", err);
        return false;
    }

    /* Chess パターン（工場デフォルト・推奨） */
    err = MLX90640_SetChessMode(MLX90640_I2C_ADDR);
    if (err != MLX90640_NO_ERROR)
    {
        PRINTF("MLX90640: Chessモード設定失敗 (err=%d)\r\n", err);
        return false;
    }

    /* EEPROM(0x2400〜) 全ダンプ（大容量のため I2CRead 内で分割読み出し） */
    err = MLX90640_DumpEE(MLX90640_I2C_ADDR, s_mlxEeData);
    if (err != MLX90640_NO_ERROR)
    {
        PRINTF("MLX90640: EEPROM読み出し失敗 (err=%d)\r\n", err);
        return false;
    }

    /* 校正パラメータ展開（float[768] のローカル配列を使うためスタック拡張済み） */
    err = MLX90640_ExtractParameters(s_mlxEeData, &s_mlxParams);
    if (err != MLX90640_NO_ERROR)
    {
        /* broken/outlier pixel の警告(>0)は致命ではないので継続する。 */
        PRINTF("MLX90640: パラメータ展開で警告 (err=%d、継続)\r\n", err);
    }

    PRINTF("MLX90640: 初期化完了 (2Hz / Chess / 放射率0.95 / tr=Ta-8)\r\n");
    return true;
}

/* MLX90640 から1サブページ分のフレームを取得し、温度[℃] に変換する。
   戻り値 >=0: サブページ番号(0/1)、<0: エラー。 */
static int mlx90640_read_subframe(void)
{
    int sp = MLX90640_GetFrameData(MLX90640_I2C_ADDR, s_mlxFrame);
    if (sp < 0)
    {
        return sp;
    }

    float ta = MLX90640_GetTa(s_mlxFrame, &s_mlxParams);
    float tr = ta - MLX90640_TR_OFFSET; /* 反射温度 */
    MLX90640_CalculateTo(s_mlxFrame, &s_mlxParams, MLX90640_EMISSIVITY, tr, s_mlxTo);

    return sp;
}

/* 768画素(32×24)の温度から min/max/中心/平均を求めてシリアル出力する。 */
static void mlx90640_print_stats(float ta)
{
    float vmin = s_mlxTo[0];
    float vmax = s_mlxTo[0];
    float sum  = 0.0f;
    for (int i = 0; i < 768; i++)
    {
        float v = s_mlxTo[i];
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
        sum += v;
    }
    float avg = sum / 768.0f;
    /* 中心画素: 行12(0-23)・列16(0-31) → index = 12*32 + 16 = 400 */
    float center = s_mlxTo[12 * 32 + 16];

    print_temp_c("\r\nMLX90640  Ta=", ta);
    print_temp_c("  min=", vmin);
    print_temp_c("  max=", vmax);
    print_temp_c("  avg=", avg);
    print_temp_c("  center=", center);
    PRINTF("\r\n");
}

/* 768画素(32×24)の温度を1フレーム分、機械可読な1行で出力する。
   形式: "FRAME,<Ta_centi>,<t0_centi>,<t1_centi>,...,<t767_centi>\r\n"
   各値は 1/100℃ 単位の整数（Python側で /100 して℃に戻す）。
   行頭マーカ "FRAME," でstats等の人間向けログと区別できる。 */
static void mlx90640_print_frame(float ta)
{
    PRINTF("FRAME,%d", (int)(ta * 100.0f + (ta >= 0 ? 0.5f : -0.5f)));
    for (int i = 0; i < 768; i++)
    {
        float v       = s_mlxTo[i];
        int32_t centi = (int32_t)(v * 100.0f + (v >= 0 ? 0.5f : -0.5f));
        PRINTF(",%d", centi);
    }
    PRINTF("\r\n");
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

    PRINTF("\r\n=== TerraGuard AI : センサ疎通確認 ===\r\n");

    /* --- 外部I²C(LPI2C2/J8) 疎通確認: バススキャンで MLX90640 等の ACK を確認 --- */
    i2c_master_init();
    i2c_bus_scan();

    PRINTF("\r\n=== オンボード温度センサ P3T1755 (I3C) ===\r\n");

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

    /* --- MLX90640 サーマルセンサ初期化（フレーム取得の主役） --- */
    PRINTF("\r\n=== サーマルセンサ MLX90640 (I2C, 0x33) ===\r\n");
    bool mlxOk = mlx90640_setup();

    PRINTF("\r\n初期化完了。サーマルフレームと参考温度を表示します。\r\n");

    uint32_t frameCount = 0;
    while (1)
    {
        if (mlxOk)
        {
            /* 1サブページ取得 → 温度変換。Chess では 0/1 の2サブページで1フレーム。
               1サブページ揃うごとに統計を表示する（2Hz設定なのでサブページは約4回/秒）。 */
            int sp = mlx90640_read_subframe();
            if (sp < 0)
            {
                PRINTF("MLX90640: フレーム取得失敗 (err=%d)\r\n", sp);
                SDK_DelayAtLeastUs(500000, CLOCK_GetCoreSysClkFreq());
            }
            else
            {
                float ta = MLX90640_GetTa(s_mlxFrame, &s_mlxParams);
                mlx90640_print_stats(ta);
                mlx90640_print_frame(ta); /* Python ヒートマップ用の全画素1行 */
                frameCount++;
            }
        }
        else
        {
            SDK_DelayAtLeastUs(1000000, CLOCK_GetCoreSysClkFreq());
        }

        /* 約8サブページ（おおむね2秒）ごとに、参考としてオンボード温度センサ(I3C)も表示。 */
        if (!mlxOk || (frameCount % 8U == 0U))
        {
            result = P3T1755_ReadTemperature(&p3t1755Handle, &temperature);
            if (result == kStatus_Success)
            {
                print_temp_c("  [ref] オンボード温度センサ P3T1755 = ", (float)temperature);
                PRINTF("\r\n");
            }
        }
    }
}
