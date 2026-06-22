/*
 * TerraGuard AI — FRDM-MCXN947
 * npu_infer: eIQ Neutron NPU カラス検出推論（2クラス: not_crow / crow）
 *
 * センサのフレーム（サーマル回転+crop後24×24 + 距離8×8 + 各前景）を、学習時
 * (tools/ml/build_trainset.py) と同じ 24×24×4 int8 入力テンソルに整形し、
 * TFLite Micro(model.cpp) 経由で NPU 推論する。出力 [1,2] の argmax で判定。
 *
 * Copyright 2026 TerraGuard
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _NPU_INFER_H_
#define _NPU_INFER_H_

#include <stdbool.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* 推論結果。 */
typedef struct
{
    bool  crow;        /* カラスと判定したか（argmax==1） */
    float confidence;  /* 勝ちクラスの確信度 0..1（出力softmaxを逆量子化） */
    float p_crow;      /* crow クラスの確率 0..1 */
} npu_result_t;

/* model.cpp の MODEL_Init() 後に1回呼ぶ。入力/出力テンソルの形と量子化を取得・検証。
   返り値: 成功 true。入力が [1,24,24,4] int8 でなければ false。 */
bool npu_infer_init(void);

/* センサフレームから 24×24×4 入力を作り、NPU 推論して結果を返す。
   引数（getter が返すフレーム。サーマルはファームで回転＋24×24crop済み）:
     thermalRot   : サーマル[24*24] ℃（行優先, thermal_mlx90640_get_frame 由来）
     thermalFgRot : サーマル前景[24*24] ℃（bg_thermal_fg 由来, >=0）
     dist8x8      : 距離[64] mm（tof_vl53l5cx_get_frame 由来, 無効は負値）
     distFg8x8    : 距離前景[64] mm（bg_dist_fg 由来, >=0）
   返り値: 推論成功 true。out に結果。 */
bool npu_infer_run(const float *thermalRot, const float *thermalFgRot,
                   const int16_t *dist8x8, const int16_t *distFg8x8,
                   npu_result_t *out);

#if defined(__cplusplus)
}
#endif

#endif /* _NPU_INFER_H_ */
