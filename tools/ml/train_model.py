#!/usr/bin/env python3
"""
TerraGuard AI — カラス検出 2クラス CNN の学習 + int8 TFLite 出力。

build_trainset.py が作った X.npy/y.npy（[N,32,32,4] float32, ラベル 0/1）で
小さな CNN を学習し、full-integer(int8) 量子化した .tflite を書き出す。
出力 .tflite はそのまま neutron_convert.sh(3.0.0) → make_model_data_h.py の
パイプラインに乗る（[[npu-converter-version-mismatch]] 参照）。

モデル方針（NPU対応op のみ。GlobalAveragePooling は MLIR で落ちるため
明示サイズ AveragePooling2D + Flatten で代替）:
    Input [32,32,4]
    Conv 8  3x3 relu → MaxPool2
    Conv 16 3x3 relu → MaxPool2
    Conv 24 3x3 relu → AvgPool(8,8) → Flatten
    Dense 2 softmax

int8 量子化は make_test_model.py と同じ手筋:
    SavedModel に export → from_saved_model → representative_dataset=学習データ →
    inference_input_type/output_type = int8。

⚠️ 実行は arch -arm64 の venv 必須（[[eiq-npu-workflow]]）:
    arch -arm64 tools/ml/.venv/bin/python tools/ml/train_model.py \
        --data dataset/built --out tools/ml/build/terra_guard_int8.tflite

SPDX-License-Identifier: BSD-3-Clause
"""
import argparse
import os

import numpy as np
import tensorflow as tf

NUM_CLASSES = 2


def build_model(in_shape):
    inputs = tf.keras.Input(shape=in_shape, name="terra_in")
    x = tf.keras.layers.Conv2D(8, 3, padding="same", activation="relu")(inputs)
    x = tf.keras.layers.MaxPooling2D(2)(x)          # 16x16x8
    x = tf.keras.layers.Conv2D(16, 3, padding="same", activation="relu")(x)
    x = tf.keras.layers.MaxPooling2D(2)(x)          # 8x8x16
    x = tf.keras.layers.Conv2D(24, 3, padding="same", activation="relu")(x)
    x = tf.keras.layers.AveragePooling2D(pool_size=(8, 8))(x)   # 1x1x24
    x = tf.keras.layers.Flatten()(x)
    outputs = tf.keras.layers.Dense(NUM_CLASSES, activation="softmax", name="cls")(x)
    return tf.keras.Model(inputs, outputs, name="terra_guard_crow")


def main():
    ap = argparse.ArgumentParser(description="カラス検出2クラスCNN学習+int8変換")
    ap.add_argument("--data", default="dataset/built", help="X.npy/y.npy のディレクトリ")
    ap.add_argument("--out", default="tools/ml/build/terra_guard_int8.tflite")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--batch", type=int, default=16)
    ap.add_argument("--val-split", type=float, default=0.2)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    X = np.load(os.path.join(args.data, "X.npy"))
    y = np.load(os.path.join(args.data, "y.npy"))
    print(f"data: X={X.shape} y={y.shape}  crow={int((y==1).sum())} "
          f"not_crow={int((y==0).sum())}")
    if len(np.unique(y)) < 2:
        raise SystemExit("クラスが1つしかありません。両クラスのデータを集めてください。")

    # シャッフルして train/val 分割（層化）。
    rng = np.random.default_rng(args.seed)
    idx = rng.permutation(len(y))
    X, y = X[idx], y[idx]
    n_val = int(len(y) * args.val_split)
    Xv, yv = X[:n_val], y[:n_val]
    Xt, yt = X[n_val:], y[n_val:]

    # クラス不均衡対策（背景が多くなりがち）。
    counts = np.bincount(yt, minlength=NUM_CLASSES)
    cw = {i: (len(yt) / (NUM_CLASSES * c) if c else 1.0) for i, c in enumerate(counts)}
    print(f"class_weight={cw}")

    model = build_model(X.shape[1:])
    model.summary()
    model.compile(optimizer="adam",
                  loss="sparse_categorical_crossentropy",
                  metrics=["accuracy"])
    model.fit(Xt, yt, validation_data=(Xv, yv) if n_val else None,
              epochs=args.epochs, batch_size=args.batch,
              class_weight=cw, verbose=2)

    # ---- int8 量子化（make_test_model.py と同じ手筋）----
    out_dir = os.path.dirname(args.out) or "."
    os.makedirs(out_dir, exist_ok=True)
    saved_dir = os.path.join(out_dir, "_saved_model_crow")
    model.export(saved_dir)

    def representative_dataset():
        # 学習データを代表データに使う（最大200サンプル）。
        for i in range(min(200, len(Xt))):
            yield [Xt[i:i + 1].astype(np.float32)]

    converter = tf.lite.TFLiteConverter.from_saved_model(saved_dir)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    tflite_model = converter.convert()

    with open(args.out, "wb") as f:
        f.write(tflite_model)
    print(f"\nWrote {args.out} ({len(tflite_model)} bytes)")

    # 入出力 dtype 確認。
    interp = tf.lite.Interpreter(model_content=tflite_model)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]
    print(f"input : {inp['dtype']} {inp['shape']} scale={inp['quantization']}")
    print(f"output: {out['dtype']} {out['shape']} scale={out['quantization']}")
    print("\n次: tools/ml/neutron_convert.sh で 3.0.0 変換 → make_model_data_h.py")


if __name__ == "__main__":
    main()
