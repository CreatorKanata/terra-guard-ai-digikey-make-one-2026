#!/usr/bin/env python3
"""
TerraGuard AI — 実機ライブ入力 vs int8 tflite 照合（誤検出の切り分け）

実機が「背景なのに crow」「カラスなのに not_crow」など想定外の判定を出すとき、
原因が (A)学習データ(分布)なのか (B)ファームNPU実行経路(テンソル詰め/変換/ドライバ)
なのかを切り分ける。

  - 実機ライブのサーマル/距離フレームを build_trainset と同じ前処理でテンソル化し、
    ホスト上の int8 tflite に通して p_crow を出す。
  - 同時にシリアルの INFER 行(=ファームNPUの結果)も拾う。
  - 同一入力に対する tflite と ファームNPU の p_crow を並べて表示する。

      tflite も実機NPUと同傾向 → 学習データ(分布)の問題。再収集→再学習で対処。
      tflite と実機NPU が乖離   → ファームNPU実行経路の問題(変換/詰め方/ドライバ)。

注意:
  - NPU版 tflite(NeutronGraph カスタムop入り)はホストでは実行不可。本スクリプトが
    回すのは「通常の int8 tflite」。NPU版の数値はファーム実機(INFER行)からしか得られない。
  - tflite 実行(arm64 tensorflow)と pyserial の両方が要る。tools/ml/.venv(arm64)に
    pyserial を入れて使う:  arch -arm64 tools/ml/.venv/bin/pip install pyserial

使い方:
    arch -arm64 tools/ml/.venv/bin/python tools/ml/diag_live_vs_tflite.py [seconds]

SPDX-License-Identifier: BSD-3-Clause
"""
import glob
import os
import re
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from dual_viewer import (  # noqa: E402
    T_ROWS, T_COLS, D_GRID, parse_csv64, extract_messages,
)
sys.path.insert(0, HERE)
import build_trainset as BT  # noqa: E402

import serial  # noqa: E402

BAUD = 921600
DEFAULT_TFLITE = os.path.join(HERE, "build", "terra_guard_int8.tflite")


def collect(port, seconds):
    """ サーマル(生/前景)・距離(grid/fg) の最新を束ね、サーマル更新ごとに1サンプル化。
        INFER 行(実機NPU結果)も拾う。戻り値: (samples, infers)。 """
    ser = serial.Serial(port, BAUD, timeout=0.02)
    ser.reset_input_buffer()
    buf = bytearray()
    t_abs = t_fg = d_grid = d_fg = None
    dist = None
    samples, text_acc = [], ""
    t0 = time.time()
    while time.time() - t0 < seconds:
        n = ser.in_waiting
        data = ser.read(n) if n else ser.read(1)
        if not data:
            continue
        text_acc += data.decode("latin-1", "replace")
        buf.extend(data)
        buf, msgs = extract_messages(buf)
        for kind, payload in msgs:
            if kind == "thermal_bin":
                _ta, pix = payload
                t_abs = np.asarray(pix, dtype=np.float32).reshape(T_ROWS, T_COLS)
                samples.append((
                    t_abs.copy(),
                    (t_fg.copy() if t_fg is not None else np.zeros((T_ROWS, T_COLS), np.float32)),
                    (d_grid.copy() if d_grid is not None else np.full((D_GRID, D_GRID), np.nan)),
                    (d_fg.copy() if d_fg is not None else np.zeros((D_GRID, D_GRID), np.float32)),
                ))
            elif kind == "thermal_fg_bin":
                t_fg = np.asarray(payload, dtype=np.float32).reshape(T_ROWS, T_COLS)
            else:
                line = payload
                d = parse_csv64(line, "DIST")
                if d is not None:
                    dist = d
                    continue
                s = parse_csv64(line, "STAT")
                if s is not None and dist is not None:
                    grid = np.asarray(dist, dtype=np.float32).reshape(D_GRID, D_GRID)
                    stat = np.asarray(s, dtype=np.float32).reshape(D_GRID, D_GRID)
                    grid[stat < 0] = np.nan
                    d_grid, dist = grid, None
                    continue
                df = parse_csv64(line, "DFG")
                if df is not None:
                    d_fg = np.asarray(df, dtype=np.float32).reshape(D_GRID, D_GRID)
    ser.close()
    infers = [(int(a), int(b), int(c)) for a, b, c in
              re.findall(r"INFER,(-?\d+),(-?\d+),-?\d+,(-?\d+),-?\d+", text_acc)]
    return samples, infers


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
    tflite_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_TFLITE
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        print("シリアルポートが見つかりません(/dev/cu.usbmodem*)。"); return
    port = ports[0]
    print(f"port={port}  tflite={tflite_path}  collecting {seconds}s ...")
    samples, infers = collect(port, seconds)
    print(f"samples={len(samples)}  infer_lines={len(infers)}")
    if infers:
        raw = [i[2] for i in infers]
        print(f"  [firmware NPU] raw crow=1: {sum(raw)}/{len(raw)}  "
              f"p_crow(x1000) mean={int(np.mean([i[1] for i in infers]))}")
    if not samples:
        print("サンプルが取れませんでした(ファームのサーマル送出を確認)。"); return

    try:
        import tensorflow as tf
        Interp = tf.lite.Interpreter
    except Exception:
        from tflite_runtime.interpreter import Interpreter as Interp
    it = Interp(model_path=tflite_path)
    it.allocate_tensors()
    ind, outd = it.get_input_details()[0], it.get_output_details()[0]
    in_scale, in_zp = ind["quantization"]
    out_scale, out_zp = outd["quantization"]

    use = samples[-min(20, len(samples)):]  # 末尾の安定フレーム
    pcs = []
    for (ta, tfg, dg, dfg) in use:
        x01 = BT.sample_to_tensor({"thermal": ta, "thermal_fg": tfg,
                                   "distance": dg, "distance_fg": dfg})
        q = np.clip(np.round(x01 / in_scale).astype(np.int32) + in_zp,
                    -128, 127).astype(np.int8)[None, ...]
        it.set_tensor(ind["index"], q)
        it.invoke()
        oq = it.get_tensor(outd["index"])[0].astype(np.int32)
        p = (oq - out_zp) * out_scale
        pcs.append((float(p[1]), p[1] >= p[0]))
    n_crow = sum(1 for _, c in pcs if c)
    pc = [p for p, _ in pcs]
    print(f"\n=== [host tflite] 同一ライブ入力 (末尾{len(use)}枚) ===")
    print(f"  crow=1: {n_crow}/{len(pcs)}  "
          f"p_crow mean={np.mean(pc):.3f} min={min(pc):.3f} max={max(pc):.3f}")
    x01 = BT.sample_to_tensor({"thermal": use[-1][0], "thermal_fg": use[-1][1],
                               "distance": use[-1][2], "distance_fg": use[-1][3]})
    for c, name in enumerate(["thermal_abs", "thermal_fg", "distance", "distance_fg"]):
        ch = x01[..., c]
        print(f"  ch{c} {name:12s}: min={ch.min():.3f} max={ch.max():.3f} mean={ch.mean():.3f}")
    print("\n判定: host tflite と firmware NPU が同傾向→学習データ(分布)の問題。"
          "\n      乖離→ファームNPU実行経路(変換/詰め方/ドライバ)の問題。")


if __name__ == "__main__":
    main()
