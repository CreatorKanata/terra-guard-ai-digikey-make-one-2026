#!/usr/bin/env python3
"""
neutron-converter が吐く生 .h のバイト列を、SDK(tflm_cifar10)の model_data.h
の「枠」に流し込んで、SDK ビルドに差し替え可能な model_data.h を合成する。

なぜ単純コピーではダメか:
- SDK 版 model_data.h は配列を `__ALIGNED(16) __PLACEMENT`（`.model` セクション=
  flash 配置）で置く。NPU 版は flash 配置前提で、これが無いと RAM に乗らない/
  リンク配置が崩れる。生 .h は `aligned(16)` のみ(RAM 配置)。
- SDK 版は `kTensorArenaSize` / `MODEL_INPUT_MEAN` / `MODEL_INPUT_STD` /
  `MODEL_NAME` を定義する必要がある（model.cpp が参照）。生 .h には無い。

この合成器は:
  生 .h から  model_data[] の中身（バイト列）と model_data_len を抽出し、
  SDK の枠（#ifdef __arm__ … __PLACEMENT、各 #define）に流し込んで出力する。

使い方:
  python tools/ml/make_model_data_h.py \
      --src-h tools/ml/build/terra_guard_int8_mcxn94x.h \
      --out   tools/ml/build/model_data.h \
      --name  terra_guard_npu \
      --arena 65536

SPDX-License-Identifier: BSD-3-Clause
"""
import argparse
import re


HEADER_TMPL = """\
/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// 自作モデル（TerraGuard 3クラス: none/crow/human）を neutron-converter(mcxn94x)
// で NPU 変換し、tools/ml/make_model_data_h.py で SDK 枠に流し込んだもの。
// 元 op resolver スニペット（model_cifarnet_ops_npu.cpp を合わせること）:
{op_comment}

#ifdef __arm__
#include <cmsis_compiler.h>
#else
#define __ALIGNED(x) __attribute__((aligned(x)))
#endif

#if defined(MCXN947_cm33_core0_SERIES)
#define __PLACEMENT __attribute__((section(".model")))
#else
#define __PLACEMENT
#endif

#define MODEL_NAME "{name}"
// int8 入力をそのまま使う想定（前段で量子化済み）。MODEL_ConvertInput が
// (x - MEAN)/STD を行うため、恒等変換になるよう 0/1 にしておく。
#define MODEL_INPUT_MEAN 0.0f
#define MODEL_INPUT_STD 1.0f

constexpr int kTensorArenaSize = {arena};

static const uint8_t model_data[] __ALIGNED(16) __PLACEMENT = {{
{body}
}};

static const unsigned int model_data_len = {length};
"""


def extract_bytes_block(src_text: str) -> tuple[str, int]:
    """生 .h から model_data[] の { ... } 中身と model_data_len を取り出す。"""
    m = re.search(r"model_data\[\][^=]*=\s*\{(.*?)\};", src_text, re.S)
    if not m:
        raise SystemExit("model_data[] の配列本体が見つからない")
    body = m.group(1).strip("\n")
    ml = re.search(r"model_data_len\s*=\s*(\d+)", src_text)
    if not ml:
        # MODEL_SIZE からでも取れる
        ml = re.search(r"MODEL_SIZE\s+(\d+)", src_text)
    if not ml:
        raise SystemExit("model_data_len / MODEL_SIZE が見つからない")
    return body, int(ml.group(1))


def extract_op_comment(src_text: str) -> str:
    """生 .h 冒頭の op resolver スニペット（/* ... */）をそのまま引用する。"""
    m = re.search(r"/\*\s*\n//\s*Register operators.*?\*/", src_text, re.S)
    return m.group(0) if m else "// (op resolver スニペット抽出失敗)"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src-h", required=True, help="neutron-converter の生 .h")
    ap.add_argument("--out", required=True, help="出力 model_data.h")
    ap.add_argument("--name", default="terra_guard_npu", help="MODEL_NAME")
    ap.add_argument("--arena", type=int, default=65536, help="kTensorArenaSize(bytes)")
    args = ap.parse_args()

    with open(args.src_h, "r") as f:
        src = f.read()

    body, length = extract_bytes_block(src)
    op_comment = extract_op_comment(src)

    out = HEADER_TMPL.format(
        op_comment=op_comment,
        name=args.name,
        arena=args.arena,
        body=body,
        length=length,
    )
    with open(args.out, "w") as f:
        f.write(out)
    print(f"Wrote {args.out}  (model_data_len={length}, arena={args.arena})")


if __name__ == "__main__":
    main()
