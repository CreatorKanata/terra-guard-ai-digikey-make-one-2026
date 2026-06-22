#!/usr/bin/env python3
"""
neutron-converter 疎通検証用の最小モデル生成スクリプト（初期検証専用・旧仕様）。

⚠️ これは「Keras → int8 TFLite → neutron-converter → model_data.h」の一気通貫が
通ることだけを確認するための **初期疎通テスト用**スクリプト。入力 [1,24,32,1]・
3クラスは旧設計で、学習もしない（ランダム重み）。
**本番のカラス検出モデル（2クラス not_crow/crow・入力24×24×4・実データ学習）は
`train_model.py` を使うこと。** 本スクリプトは neutron-converter 環境の動作確認や
回帰用に残してある。

使い方:
    tools/ml/.venv/bin/python tools/ml/make_test_model.py --out build/terra_guard_int8.tflite

SPDX-License-Identifier: BSD-3-Clause
"""
import argparse
import os

import numpy as np
import tensorflow as tf

H, W, C = 24, 32, 1
NUM_CLASSES = 3


def build_model() -> tf.keras.Model:
    """NPU対応op(Conv2D/ReLU/MaxPool/AvgPool/FC/Softmax)のみで構成した tiny CNN。"""
    inputs = tf.keras.Input(shape=(H, W, C), name="thermal_fg")
    x = tf.keras.layers.Conv2D(8, 3, padding="same", activation="relu")(inputs)
    x = tf.keras.layers.MaxPooling2D(2)(x)        # 12x16x8
    x = tf.keras.layers.Conv2D(16, 3, padding="same", activation="relu")(x)
    x = tf.keras.layers.MaxPooling2D(2)(x)        # 6x8x16
    # GlobalAveragePooling2D は TF2.16 の TFLite 変換器(MLIR)でクラッシュするため、
    # 明示サイズの AveragePooling2D + Flatten で等価に置き換える（NPUは AvgPool 対応op）。
    x = tf.keras.layers.AveragePooling2D(pool_size=(6, 8))(x)   # -> 1x1x16
    x = tf.keras.layers.Flatten()(x)                            # -> 16
    outputs = tf.keras.layers.Dense(NUM_CLASSES, activation="softmax", name="cls")(x)
    return tf.keras.Model(inputs, outputs, name="terra_guard_tiny")


def representative_dataset():
    """量子化キャリブレーション用のダミー代表データ（本番は実フレームに差し替え）。"""
    rng = np.random.default_rng(0)
    for _ in range(100):
        yield [rng.random((1, H, W, C), dtype=np.float32)]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="build/terra_guard_int8.tflite")
    args = ap.parse_args()

    model = build_model()
    model.summary()

    # TF2.16(Keras3) では from_keras_model 直変換が MLIR で落ちる
    # (ReadVariableOp: missing attribute 'value')。一旦 SavedModel に export し、
    # from_saved_model で変換すると安定する。
    saved_dir = os.path.join(os.path.dirname(args.out) or ".", "_saved_model")
    model.export(saved_dir)
    converter = tf.lite.TFLiteConverter.from_saved_model(saved_dir)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    # NPU は full-integer int8 のみ加速。入出力も int8 に固定する。
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8

    tflite_model = converter.convert()

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(tflite_model)
    print(f"\nWrote {args.out} ({len(tflite_model)} bytes)")

    # 入出力 dtype を確認（int8 になっているはず）
    interp = tf.lite.Interpreter(model_content=tflite_model)
    interp.allocate_tensors()
    print("input :", interp.get_input_details()[0]["dtype"], interp.get_input_details()[0]["shape"])
    print("output:", interp.get_output_details()[0]["dtype"], interp.get_output_details()[0]["shape"])


if __name__ == "__main__":
    main()
