#!/usr/bin/env python3
"""
TerraGuard AI — 収集した .npz 群を学習用テンソルへ変換する。

collect_dataset.py が貯めた 1サンプル=1.npz（生の向き補正済みデータ）を読み、
モデル入力 [32, 32, 4]（float32, 0..1 正規化）とラベル [N] にまとめて
X.npy / y.npy / meta.json として書き出す。

チャンネル構成（将来の 32×32×4 最終仕様に合わせる）:
    ch0: thermal_abs  生サーマル(℃) を [T_VMIN,T_VMAX] で 0..1 正規化
    ch1: thermal_fg   サーマル前景(℃,>=0) を [0,T_FG_MAX] で 0..1
    ch2: distance     距離(mm) を [0,D_VMAX] で 0..1（NaN/無効は 0=遠方相当）
    ch3: distance_fg  距離前景(mm closer,>=0) を [0,D_FG_MAX] で 0..1

形状合わせ:
    サーマル: ファーム側で 90度右回転済み [32,24] → 中央24行 crop で [24,24]
             → 32×32 の中央へ四辺4ずつ 0 padding。
    距離     8×8  → 最近傍4倍 upsample して 32×32（回転しない）。

ラベル:
    not_crow → 0,  crow → 1   （出力 [1,2] の Softmax を想定）

int8 量子化は TFLite converter が representative_dataset で行うため、ここでは
float32(0..1) で出す。representative_dataset には X.npy をそのまま使える。

使い方:
    tools/.venv/bin/python tools/ml/build_trainset.py \
        --raw dataset/raw --out dataset/built

SPDX-License-Identifier: BSD-3-Clause
"""
import argparse
import json
import os

import numpy as np

# 正規化レンジ（ビューアの既定表示レンジに整合。屋外運用で要調整）。
T_VMIN, T_VMAX = 15.0, 45.0     # サーマル温度[℃]
T_FG_MAX = 5.0                  # サーマル前景[Δ℃]
D_VMAX = 4000.0                 # 距離[mm]
D_FG_MAX = 500.0               # 距離前景[mm closer]

IN_H, IN_W = 32, 32
LABEL_TO_ID = {"not_crow": 0, "crow": 1}


def crop_pad_thermal(arr32x24: np.ndarray) -> np.ndarray:
    """ 回転済みサーマル [32,24]（高さ32×幅24）を中央 24 行 crop → [24,24] にし、
        32×32 の中央へ四辺 4 ずつ 0 padding する。

        サーマルはファーム側(thermal_mlx90640.c rotate_cw90)で 90度右回転済みなので、
        npz には [32,24]（回転後の向き）で保存される。ここでは回転は行わない。
        ファーム側 npu_infer.c の crop/pad（中央24行→32×32中央pad）と厳密一致させること。
    """
    crop_top = (arr32x24.shape[0] - 24) // 2     # = 4（上下4行カット）
    cropped = arr32x24[crop_top:crop_top + 24, :]  # [24,24]
    pad = (IN_H - 24) // 2                          # = 4（四辺）
    return np.pad(cropped, ((pad, pad), (pad, pad)), mode="constant")  # [32,32]


def upsample_dist(arr8x8: np.ndarray) -> np.ndarray:
    """ [8,8] を最近傍 4 倍して [32,32] に。 """
    return np.kron(arr8x8, np.ones((IN_H // 8, IN_W // 8), dtype=arr8x8.dtype))


def normalize(x: np.ndarray, lo: float, hi: float) -> np.ndarray:
    """ [lo,hi] を 0..1 にクリップ正規化。 """
    return np.clip((x - lo) / (hi - lo), 0.0, 1.0).astype(np.float32)


def sample_to_tensor(npz) -> np.ndarray:
    """ 1 サンプル(.npz) を [32,32,4] float32(0..1) へ。 """
    thermal = np.nan_to_num(npz["thermal"].astype(np.float32), nan=T_VMIN)
    thermal_fg = np.nan_to_num(npz["thermal_fg"].astype(np.float32), nan=0.0)
    distance = npz["distance"].astype(np.float32)
    distance = np.nan_to_num(distance, nan=D_VMAX)   # 無効=遠方相当
    distance_fg = np.nan_to_num(npz["distance_fg"].astype(np.float32), nan=0.0)

    ch0 = crop_pad_thermal(normalize(thermal, T_VMIN, T_VMAX))
    ch1 = crop_pad_thermal(normalize(thermal_fg, 0.0, T_FG_MAX))
    ch2 = upsample_dist(normalize(distance, 0.0, D_VMAX))
    ch3 = upsample_dist(normalize(distance_fg, 0.0, D_FG_MAX))
    return np.stack([ch0, ch1, ch2, ch3], axis=-1)   # [32,32,4]


def main():
    ap = argparse.ArgumentParser(description="収集 .npz → 学習テンソル")
    ap.add_argument("--raw", default="dataset/raw", help="collect_dataset.py の出力")
    ap.add_argument("--out", default="dataset/built", help="X.npy/y.npy の出力先")
    args = ap.parse_args()

    files = sorted(f for f in os.listdir(args.raw) if f.endswith(".npz"))
    if not files:
        raise SystemExit(f"{args.raw} に .npz がありません")

    X, y, skipped = [], [], 0
    for f in files:
        with np.load(os.path.join(args.raw, f), allow_pickle=True) as npz:
            label = str(npz["label"])
            if label not in LABEL_TO_ID:
                skipped += 1
                continue
            X.append(sample_to_tensor(npz))
            y.append(LABEL_TO_ID[label])

    X = np.asarray(X, dtype=np.float32)
    y = np.asarray(y, dtype=np.int32)

    os.makedirs(args.out, exist_ok=True)
    np.save(os.path.join(args.out, "X.npy"), X)
    np.save(os.path.join(args.out, "y.npy"), y)

    n_crow = int((y == 1).sum())
    n_not = int((y == 0).sum())
    meta = {
        "num_samples": int(len(y)),
        "input_shape": list(X.shape[1:]),
        "channels": ["thermal_abs", "thermal_fg", "distance", "distance_fg"],
        "label_to_id": LABEL_TO_ID,
        "counts": {"not_crow": n_not, "crow": n_crow},
        "norm": {"T_VMIN": T_VMIN, "T_VMAX": T_VMAX, "T_FG_MAX": T_FG_MAX,
                 "D_VMAX": D_VMAX, "D_FG_MAX": D_FG_MAX},
        "skipped_unlabeled": skipped,
    }
    with open(os.path.join(args.out, "meta.json"), "w") as fp:
        json.dump(meta, fp, ensure_ascii=False, indent=2)

    print(f"X={X.shape} y={y.shape}  crow={n_crow} not_crow={n_not} "
          f"skipped={skipped}")
    print(f"→ {args.out}/X.npy, y.npy, meta.json")


if __name__ == "__main__":
    main()
