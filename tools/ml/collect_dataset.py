#!/usr/bin/env python3
"""
TerraGuard AI — カラス検出データ収集ツール（ライブ・キー打鍵ラベリング）

FRDM-MCXN947 がシリアルに流す
  - 生サーマル    (0xAA55 + Ta + 768×int16, 32×24, ℃)
  - サーマル前景  (0xAA56 + 768×int16, 32×24, 背景差分の正側)
  - 距離          ("DIST,..." 64要素, mm)
  - 距離 status   ("STAT,..." 64要素)
  - 距離前景      ("DFG,..."  64要素, mm)
  - 候補判定      ("DET,cand,t_max_c,t_area,d_max,d_area")
を受信し、サーマル1フレーム到着を基準に「直近の各系統」を1サンプルへ束ねて
連番 .npz で保存する。受信＞保存でも遅延が積まないよう、最新フレームのみ束ねる。

ラベルはターミナルでのキー打鍵でライブに付ける（押した時点の current_label が
以降のサンプルに付与され続ける）:
    c = crow      （カラスがいる）
    n = not_crow  （カラスがいない=背景/人/その他）
    s = skip      （以降のフレームを保存しない=曖昧な区間）
    q / Ctrl-C    （終了）

保存先（既定 dataset/raw/）には次を 1 サンプル=1ファイルで書く:
    thermal     : float32 [24,24]  サーマル(℃)。ファーム側で90度右回転＋中央24行crop済み。
    thermal_fg  : float32 [24,24]  サーマル前景(℃, >=0)。未確立なら全0。
    distance    : float32 [8,8]    距離(mm)。無効ゾーンは NaN。
    distance_fg : float32 [8,8]    距離前景(mm closer, >=0)。
    det         : int32   [5]      (cand,t_max_c,t_area,d_max,d_area)。無ければ -1。
    label       : 'crow' | 'not_crow'
    ts          : float            収集時刻(time.time())

入力テンソル化（32×32×4 等）は build_trainset.py に集約する。ここでは
「向きを直した生データ」を素直に貯める（後段で前処理を自由に変えられる）。

使い方:
    tools/.venv/bin/python tools/ml/collect_dataset.py \
        --port /dev/cu.usbmodemXXXX --out dataset/raw

依存: pyserial, numpy（tools/.venv にあり）

SPDX-License-Identifier: BSD-3-Clause
"""
import argparse
import os
import sys
import termios
import threading
import time
import tty
from collections import deque

import numpy as np

# パーサ・定数はビューア群と共通のものを再利用する（フォーマットの二重管理を避ける）。
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from dual_viewer import (  # noqa: E402
    T_ROWS, T_COLS, D_GRID,
    parse_csv64, extract_messages, open_serial,
)

# ファーム側で「90度右回転＋中央24行crop」済みの 24×24 が送られてくる。
# 受信側は reshape(24,24) するだけ（回転・crop・上下反転は一切しない）。

LABELS = {"c": "crow", "n": "not_crow"}


def parse_args():
    p = argparse.ArgumentParser(description="カラス検出データ収集（ライブラベリング）")
    p.add_argument("--port", default="/dev/cu.usbmodemFQI2HWQMUXQ2J3",
                   help="シリアルポート（ls /dev/cu.usbmodem* で確認）")
    p.add_argument("--baud", type=int, default=921600,
                   help="ボーレート（ファーム既定=921600）")
    p.add_argument("--out", default="dataset/raw",
                   help="保存先ディレクトリ（無ければ作成）")
    p.add_argument("--max-fps", type=float, default=8.0,
                   help="保存レートの上限[fps]（近接フレームの重複を間引く）")
    return p.parse_args()


# ---------------------------------------------------------------------------
# キーボード: raw モードで1文字ずつ非ブロッキングに読む（ターミナル前提）。
# ---------------------------------------------------------------------------
class KeyReader:
    """ stdin を raw モードにして1文字ずつ取り出す。Enter 不要。 """

    def __init__(self):
        self._fd = sys.stdin.fileno()
        self._old = None
        self._lock = threading.Lock()
        self._buf = deque()
        self._stop = threading.Event()
        self._thread = None

    def __enter__(self):
        if not sys.stdin.isatty():
            raise RuntimeError("標準入力が tty ではありません（ターミナルで実行してください）")
        self._old = termios.tcgetattr(self._fd)
        tty.setcbreak(self._fd)
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        if self._old is not None:
            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._old)

    def _loop(self):
        while not self._stop.is_set():
            ch = sys.stdin.read(1)
            if ch:
                with self._lock:
                    self._buf.append(ch)

    def get(self):
        """ 溜まっているキーを1つ返す。無ければ None。 """
        with self._lock:
            return self._buf.popleft() if self._buf else None


# ---------------------------------------------------------------------------
# 受信スレッド: 最新の各系統を保持し、サーマル更新で1サンプルにまとめてキューへ。
# ---------------------------------------------------------------------------
class Collector:
    def __init__(self, args):
        self.args = args
        self._lock = threading.Lock()
        # 直近の各系統（向き補正済み）。
        self.d_grid = None     # [8,8] mm（NaN=無効）
        self.d_fg = None       # [8,8] mm closer
        self.t_fg = None       # [24,32] ℃ 前景
        self.det = None        # (cand,t_max_c,t_area,d_max,d_area)
        # 1サンプルにまとまった生データのキュー（保存側が取り出す）。
        self.queue = deque()
        self.stop_evt = threading.Event()
        self._last_emit = 0.0

    def _shape_thermal(self, ta_pix):
        """ サーマル list(576) を [24,24] にする。
            サーマル・距離とも向きはファーム側で確定済み（thermal_mlx90640.c の
            rotate_crop で「90度右回転＋中央24行crop」、tof_vl53l5cx.c で左右反転）。
            収集側は無加工で reshape するだけ（実機NPU入力と同一の向きを保つ）。 """
        _ta, pix = ta_pix
        return np.asarray(pix, dtype=np.float32).reshape(T_ROWS, T_COLS)

    def _shape_thermal_fg(self, pix):
        return np.asarray(pix, dtype=np.float32).reshape(T_ROWS, T_COLS)

    def _shape_dist(self, flat):
        return np.asarray(flat, dtype=np.float32).reshape(D_GRID, D_GRID)

    def reader_loop(self, ser):
        buf = bytearray()
        dist = None  # DIST を STAT とペアになるまで保持（NaN化に status を使う）
        while not self.stop_evt.is_set():
            try:
                n = ser.in_waiting
                data = ser.read(n) if n else ser.read(1)
            except Exception as e:  # serial.SerialException 等
                print(f"\nシリアル読み出しエラー: {e}", file=sys.stderr)
                break
            if not data:
                continue
            buf.extend(data)
            latest_t = None
            buf, msgs = extract_messages(buf)
            for kind, payload in msgs:
                if kind == "thermal_bin":
                    latest_t = payload
                    continue
                if kind == "thermal_fg_bin":
                    with self._lock:
                        self.t_fg = self._shape_thermal_fg(payload)
                    continue
                line = payload
                d = parse_csv64(line, "DIST")
                if d is not None:
                    dist = d
                    continue
                s = parse_csv64(line, "STAT")
                if s is not None and dist is not None:
                    grid = self._shape_dist(dist)
                    stat = self._shape_dist(s)
                    grid[stat < 0] = np.nan        # ファームの -1 → NaN
                    with self._lock:
                        self.d_grid = grid
                    dist = None
                    continue
                df = parse_csv64(line, "DFG")
                if df is not None:
                    with self._lock:
                        self.d_fg = self._shape_dist(df)
                    continue
                if line.startswith("DET,"):
                    parts = line.strip().split(",")
                    if len(parts) == 6:
                        try:
                            with self._lock:
                                self.det = tuple(int(x) for x in parts[1:])
                        except ValueError:
                            pass

            # サーマル新フレーム → 1サンプルに束ねてキューへ（max-fps で間引き）。
            if latest_t is not None:
                now = time.monotonic()
                if now - self._last_emit < 1.0 / self.args.max_fps:
                    continue
                self._last_emit = now
                t_arr = self._shape_thermal(latest_t)
                with self._lock:
                    sample = {
                        "thermal": t_arr,
                        "thermal_fg": (self.t_fg if self.t_fg is not None
                                       else np.zeros_like(t_arr)),
                        "distance": (self.d_grid if self.d_grid is not None
                                     else np.full((D_GRID, D_GRID), np.nan, np.float32)),
                        "distance_fg": (self.d_fg if self.d_fg is not None
                                        else np.zeros((D_GRID, D_GRID), np.float32)),
                        "det": (np.asarray(self.det, np.int32) if self.det is not None
                                else np.full(5, -1, np.int32)),
                        "ts": time.time(),
                    }
                    self.queue.append(sample)


def main():
    args = parse_args()
    os.makedirs(args.out, exist_ok=True)

    # 既存ファイルの最大連番の続きから採番する（追記収集できるように）。
    existing = [f for f in os.listdir(args.out) if f.endswith(".npz")]
    next_idx = 0
    for f in existing:
        try:
            next_idx = max(next_idx, int(f.split("_")[-1].split(".")[0]) + 1)
        except (ValueError, IndexError):
            pass

    ser = open_serial(args, timeout=0.05)
    print(f"接続: {args.port} @ {args.baud}")
    print(f"保存先: {args.out}（連番 {next_idx:06d} から）")
    print("─" * 56)
    print("  c=crow   n=not_crow   s=skip(保存停止)   q=終了")
    print("  押した時点のラベルが以降のフレームに付与され続けます。")
    print("─" * 56)

    col = Collector(args)
    reader = threading.Thread(target=col.reader_loop, args=(ser,), daemon=True)
    reader.start()

    current_label = None  # None または 'crow'/'not_crow'。s で None に戻す。
    saved = {"crow": 0, "not_crow": 0}
    idx = next_idx
    last_status = 0.0

    try:
        with KeyReader() as keys:
            while True:
                ch = keys.get()
                if ch is not None:
                    if ch in ("q", "\x03"):  # q / Ctrl-C
                        break
                    if ch == "s":
                        current_label = None
                        print("\n[skip] 保存を停止（次のラベルキーで再開）")
                    elif ch in LABELS:
                        current_label = LABELS[ch]
                        print(f"\n[label] → {current_label}")

                # キューを掃き出して保存（current_label があるときだけ）。
                while col.queue:
                    sample = col.queue.popleft()
                    if current_label is None:
                        continue
                    path = os.path.join(args.out, f"sample_{idx:06d}.npz")
                    np.savez_compressed(
                        path,
                        thermal=sample["thermal"],
                        thermal_fg=sample["thermal_fg"],
                        distance=sample["distance"],
                        distance_fg=sample["distance_fg"],
                        det=sample["det"],
                        label=current_label,
                        ts=sample["ts"],
                    )
                    saved[current_label] += 1
                    idx += 1

                # 0.5秒ごとに進捗を1行表示（上書き）。
                now = time.monotonic()
                if now - last_status > 0.5:
                    last_status = now
                    lbl = current_label if current_label else "（停止中）"
                    sys.stdout.write(
                        f"\r収集中 label={lbl:8s}  "
                        f"crow={saved['crow']}  not_crow={saved['not_crow']}  "
                        f"total={saved['crow'] + saved['not_crow']}   ")
                    sys.stdout.flush()

                time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    finally:
        col.stop_evt.set()
        ser.close()
        print(f"\n終了。保存: crow={saved['crow']} not_crow={saved['not_crow']} "
              f"→ {args.out}")


if __name__ == "__main__":
    main()
