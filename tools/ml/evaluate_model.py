#!/usr/bin/env python3
"""
TerraGuard AI — int8 TFLite モデルをホールドアウト検証セットで評価する。

split_holdout.py が分離した dataset/built/{X_holdout,y_holdout}.npy（学習に使って
いない未学習データ）を、学習出力の int8 TFLite（実機NPUと同じ量子化モデル）で
推論し、分類性能を表示する。

表示する指標:
  - 混同行列（実 vs 予測）
  - クラス別 precision / recall / F1
  - 全体 accuracy / マクロ平均
  - crow を陽性としたときの 偽陰性(FN: カラスを見逃し) / 偽陽性(FP: 誤検出)
  - 誤分類サンプルの一覧（インデックス・確信度）

int8 量子化は学習時に scale=1/255, zp=-128（入力）/ scale=1/256, zp=-128（出力）で
固定（ファーム npu_infer.c と一致）。ここでは TFLite interpreter が内部で量子化を
行うため、float32(0..1) をそのまま入力すればよい（input_type=int8 のモデルは
interpreter が量子化テンソルを期待するので、明示的に量子化して渡す）。

使い方:
    tools/.venv/bin/python tools/ml/evaluate_model.py \
        --tflite tools/ml/build/terra_guard_int8.tflite \
        --data dataset/built

SPDX-License-Identifier: BSD-3-Clause
"""
import argparse
import os

import numpy as np

try:
    import tensorflow as tf
    _Interpreter = tf.lite.Interpreter
except Exception:  # tflite_runtime のみの環境向けフォールバック
    from tflite_runtime.interpreter import Interpreter as _Interpreter

CLASS_NAMES = {0: "not_crow", 1: "crow"}


def load_interpreter(tflite_path):
    interp = _Interpreter(model_path=tflite_path)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]
    return interp, inp, out


def quantize(x_f32, scale, zero_point):
    """ float(0..1) → int8。q = round(x/scale) + zp。 """
    q = np.round(x_f32 / scale) + zero_point
    return np.clip(q, -128, 127).astype(np.int8)


def predict_all(interp, inp, out, X):
    """ N サンプルを1枚ずつ推論し、予測クラスと crow 確率(逆量子化)を返す。 """
    in_scale, in_zp = inp["quantization"]
    out_scale, out_zp = out["quantization"]
    preds = np.zeros(len(X), dtype=np.int64)
    p_crow = np.zeros(len(X), dtype=np.float32)
    for i in range(len(X)):
        xq = quantize(X[i:i + 1].astype(np.float32), in_scale, in_zp)
        interp.set_tensor(inp["index"], xq)
        interp.invoke()
        oq = interp.get_tensor(out["index"])[0].astype(np.int32)
        prob = (oq - out_zp) * out_scale  # 逆量子化 → [p_not_crow, p_crow]
        preds[i] = int(np.argmax(prob))
        p_crow[i] = float(prob[1])
    return preds, p_crow


def confusion(y_true, y_pred, num_classes=2):
    cm = np.zeros((num_classes, num_classes), dtype=np.int64)
    for t, p in zip(y_true, y_pred):
        cm[t, p] += 1
    return cm


def main():
    ap = argparse.ArgumentParser(description="int8 TFLite をホールドアウトで評価")
    ap.add_argument("--tflite", default="tools/ml/build/terra_guard_int8.tflite")
    ap.add_argument("--data", default="dataset/built",
                    help="X_holdout.npy/y_holdout.npy のディレクトリ")
    args = ap.parse_args()

    Xh = np.load(os.path.join(args.data, "X_holdout.npy"))
    yh = np.load(os.path.join(args.data, "y_holdout.npy"))
    print(f"検証セット: X={Xh.shape}  "
          f"crow={int((yh == 1).sum())} not_crow={int((yh == 0).sum())}")
    print(f"モデル: {args.tflite}\n")

    interp, inp, out = load_interpreter(args.tflite)
    y_pred, p_crow = predict_all(interp, inp, out, Xh)

    cm = confusion(yh, y_pred)
    # cm[t][p]: 行=実クラス, 列=予測クラス
    tn, fp = cm[0, 0], cm[0, 1]   # 実not_crow → 予測not_crow / crow
    fn, tp = cm[1, 0], cm[1, 1]   # 実crow     → 予測not_crow / crow

    acc = (tp + tn) / cm.sum() if cm.sum() else 0.0

    def safe(n, d):
        return n / d if d else 0.0

    # crow を陽性として
    prec_crow = safe(tp, tp + fp)
    rec_crow = safe(tp, tp + fn)
    f1_crow = safe(2 * prec_crow * rec_crow, prec_crow + rec_crow)
    # not_crow を陽性として
    prec_nc = safe(tn, tn + fn)
    rec_nc = safe(tn, tn + fp)
    f1_nc = safe(2 * prec_nc * rec_nc, prec_nc + rec_nc)

    print("=== 混同行列（行=実際 / 列=予測）===")
    print(f"                 予測:not_crow   予測:crow")
    print(f"  実:not_crow        {tn:6d}      {fp:6d}")
    print(f"  実:crow            {fn:6d}      {tp:6d}")
    print()
    print("=== クラス別指標 ===")
    print(f"  not_crow : precision={prec_nc:.3f}  recall={rec_nc:.3f}  f1={f1_nc:.3f}")
    print(f"  crow     : precision={prec_crow:.3f}  recall={rec_crow:.3f}  f1={f1_crow:.3f}")
    print(f"  マクロ平均 f1 = {(f1_crow + f1_nc) / 2:.3f}")
    print()
    print("=== 全体 ===")
    print(f"  accuracy = {acc:.3f}  ({tp + tn}/{cm.sum()} 正解)")
    print(f"  偽陰性 FN（カラスを見逃し）= {fn}  ← 最重要（検出漏れ）")
    print(f"  偽陽性 FP（カラスでないのに検出）= {fp}")
    print()

    # 誤分類サンプルの内訳
    wrong = np.where(y_pred != yh)[0]
    if len(wrong) == 0:
        print("=== 誤分類: なし（全問正解）===")
    else:
        print(f"=== 誤分類サンプル（{len(wrong)}件）===")
        for i in wrong:
            print(f"  idx={i:3d}  実={CLASS_NAMES[int(yh[i])]:8s} "
                  f"予測={CLASS_NAMES[int(y_pred[i])]:8s}  p_crow={p_crow[i]:.3f}")


if __name__ == "__main__":
    main()
