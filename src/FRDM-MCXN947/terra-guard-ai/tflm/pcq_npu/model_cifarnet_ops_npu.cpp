/*
 * Copyright 2022 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tensorflow/lite/micro/kernels/micro_ops.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/kernels/neutron/neutron.h"

// 自作モデル（TerraGuard カラス検出 2クラス: not_crow/crow, 入力24×24×4）用の op resolver。
// neutron-converter(mcxn94x, 3.0.0) が model_data.h 冒頭に出力したスニペットに合わせる:
//   Softmax + Slice + NEUTRON_GRAPH(custom) の 3 op。
//   ※ converter 3.1.3 では 2op だったが、SDKドライバ(3.0.0)に一致させるため
//      3.0.0 で再変換した結果 Slice が追加された。
tflite::MicroOpResolver &MODEL_GetOpsResolver()
{
    static tflite::MicroMutableOpResolver<3> s_microOpResolver;

    s_microOpResolver.AddSoftmax();
    s_microOpResolver.AddSlice();
    s_microOpResolver.AddCustom(tflite::GetString_NEUTRON_GRAPH(),
        tflite::Register_NEUTRON_GRAPH());

    return s_microOpResolver;
}
