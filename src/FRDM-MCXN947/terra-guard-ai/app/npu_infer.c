/*
 * TerraGuard AI — FRDM-MCXN947
 * npu_infer: eIQ Neutron NPU カラス検出推論（2クラス: not_crow / crow）の実装
 *
 * 入力テンソル整形は tools/ml/build_trainset.py と厳密に一致させる:
 *   ch0 thermal_abs : サーマル(℃) を [T_VMIN,T_VMAX] で 0..1
 *   ch1 thermal_fg  : サーマル前景(℃,>=0) を [0,T_FG_MAX] で 0..1
 *   ch2 distance    : 距離(mm) を [0,D_VMAX] で 0..1（無効/負値は遠方=D_VMAX相当）
 *   ch3 distance_fg : 距離前景(mm,>=0) を [0,D_FG_MAX] で 0..1
 * 形状合わせ: サーマル24×32→上下に4行ずつ0pad で32×32 / 距離8×8→最近傍4倍で32×32。
 * 向き: 収集器(collect_dataset.py / dual_viewer_web.py)はサーマルを行方向に上下反転
 *       (arr[::-1,:])してから保存するので、ここでも同じ上下反転を行う。距離は反転なし。
 *
 * Copyright 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "npu_infer.h"
#include "model.h"
#include "fsl_debug_console.h"

/* 正規化レンジ（build_trainset.py と一致させること）。 */
#define T_VMIN   15.0f
#define T_VMAX   45.0f
#define T_FG_MAX 5.0f
#define D_VMAX   4000.0f
#define D_FG_MAX 500.0f

/* センサ生フレームの形状。 */
#define TH 24   /* thermal rows */
#define TW 32   /* thermal cols */
#define DG 8    /* distance grid */

/* モデル入力（32×32×4）。 */
#define IN_H 32
#define IN_W 32
#define IN_C 4
#define IN_SIZE (IN_H * IN_W * IN_C)

/* 入力量子化（学習時の int8 量子化に一致。scale=1/255, zero_point=-128）。
   float x(0..1) → int8 q = round(x*255) - 128 = round(x/scale) + zp。
   model.cpp は per-tensor int8 を前提とするため固定値で再現する。 */
#define IN_SCALE 0.003921568859f
#define IN_ZP    (-128)

static bool s_ready;

static inline float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static inline int8_t quantize01(float x01)
{
    /* q = round(x/scale) + zp。x01 は 0..1 にクリップ済み前提。 */
    int q = (int)(x01 / IN_SCALE + 0.5f) + IN_ZP;
    if (q < -128) q = -128;
    if (q > 127)  q = 127;
    return (int8_t)q;
}

bool npu_infer_init(void)
{
    tensor_dims_t dims;
    tensor_type_t type;
    uint8_t *in = MODEL_GetInputTensorData(&dims, &type);

    s_ready = false;
    if (in == NULL)
    {
        PRINTF("npu_infer: 入力テンソル取得失敗\r\n");
        return false;
    }
    /* 期待: [1,32,32,4] int8。 */
    if (type != kTensorType_INT8 || dims.size != 4 ||
        dims.data[1] != IN_H || dims.data[2] != IN_W || dims.data[3] != IN_C)
    {
        PRINTF("npu_infer: 入力形状/型が想定外 (size=%d [%d,%d,%d,%d] type=%d)\r\n",
               (int)dims.size, (int)dims.data[0], (int)dims.data[1],
               (int)dims.data[2], (int)dims.data[3], (int)type);
        return false;
    }
    s_ready = true;
    return true;
}

bool npu_infer_run(const float *thermal24x32, const float *thermalFg24x32,
                   const int16_t *dist8x8, const int16_t *distFg8x8,
                   npu_result_t *out)
{
    if (!s_ready || out == NULL)
    {
        return false;
    }

    tensor_dims_t dims;
    tensor_type_t type;
    int8_t *in = (int8_t *)MODEL_GetInputTensorData(&dims, &type);
    if (in == NULL)
    {
        return false;
    }

    /* 入力テンソルは [H=32][W=32][C=4] の行優先。in[(y*IN_W + x)*IN_C + c]。 */
    const int padTop = (IN_H - TH) / 2;   /* = 4 */

    for (int y = 0; y < IN_H; y++)
    {
        for (int x = 0; x < IN_W; x++)
        {
            float ch0, ch1, ch2, ch3;

            /* --- ch0/ch1: サーマル（上下4行0pad、行を上下反転） --- */
            if (y < padTop || y >= padTop + TH)
            {
                ch0 = 0.0f; /* pad 領域は 0（学習時の constant pad と一致） */
                ch1 = 0.0f;
            }
            else
            {
                int tr = y - padTop;          /* 0..23（pad除いた行） */
                int trFlip = (TH - 1) - tr;    /* 上下反転（収集器の arr[::-1,:] と一致） */
                int tidx = trFlip * TW + x;    /* x はそのまま（列は反転しない） */
                float tc = thermal24x32[tidx];
                float tf = thermalFg24x32[tidx];
                ch0 = clamp01((tc - T_VMIN) / (T_VMAX - T_VMIN));
                ch1 = clamp01((tf - 0.0f) / (T_FG_MAX - 0.0f));
            }

            /* --- ch2/ch3: 距離（8×8→最近傍4倍, 反転なし） --- */
            int dr = y / (IN_H / DG);   /* y/4 → 0..7 */
            int dc = x / (IN_W / DG);   /* x/4 → 0..7 */
            int didx = dr * DG + dc;
            float dmm = (float)dist8x8[didx];
            if (dmm < 0.0f) dmm = D_VMAX;     /* 無効ゾーンは遠方相当（build_trainset と一致） */
            float dfg = (float)distFg8x8[didx];
            if (dfg < 0.0f) dfg = 0.0f;
            ch2 = clamp01((dmm - 0.0f) / (D_VMAX - 0.0f));
            ch3 = clamp01((dfg - 0.0f) / (D_FG_MAX - 0.0f));

            int base = (y * IN_W + x) * IN_C;
            in[base + 0] = quantize01(ch0);
            in[base + 1] = quantize01(ch1);
            in[base + 2] = quantize01(ch2);
            in[base + 3] = quantize01(ch3);
        }
    }

    if (MODEL_RunInference() != kStatus_Success)
    {
        return false;
    }

    /* 出力 [1,2] int8。逆量子化して softmax 確率に戻す（per-tensor int8）。
       出力スケールは学習時 scale=1/256, zp=-128（make時のログ参照）。 */
    int8_t *outq = (int8_t *)MODEL_GetOutputTensorData(&dims, &type);
    if (outq == NULL || dims.data[dims.size - 1] < 2)
    {
        return false;
    }
    const float outScale = 0.00390625f; /* 1/256 */
    const int   outZp    = -128;
    float p0 = ((int)outq[0] - outZp) * outScale; /* not_crow */
    float p1 = ((int)outq[1] - outZp) * outScale; /* crow */

    out->p_crow     = p1;
    out->crow       = (p1 >= p0);
    out->confidence = out->crow ? p1 : p0;
    return true;
}
