#!/usr/bin/env python3
"""
fps_meter — 2センサの実FPSをPC側で計測して表示する（ファーム不変）。

FRDM-MCXN947 が出力する2系統のフレーム到着レートを測る:
  - 距離(VL53L5CX): テキスト "DIST,..." 行（1フレームに DIST/STAT の2行が出るが
                    フレーム数は DIST 行でカウント）
  - サーマル(MLX90640): バイナリフレーム（magic 0xAA55 + Ta + 768×int16）

既存の dual_viewer.extract_messages を使い、テキスト/バイナリ混在ストリームを
正しく分離してから、直近 N 秒ウィンドウの平均FPSを毎秒表示する。

使い方:
  tools/.venv/bin/python tools/fps_meter.py
  tools/.venv/bin/python tools/fps_meter.py --port /dev/cu.usbmodemXXXX --window 2.0
"""
import argparse
import sys
import time
from collections import deque

import serial

from dual_viewer import extract_messages, open_serial


def parse_args():
    p = argparse.ArgumentParser(description="2センサの実FPSを計測表示")
    p.add_argument("--port", default=None, help="シリアルポート（未指定なら自動検出）")
    p.add_argument("--baud", type=int, default=921600,
                   help="ボーレート（既定 921600。dual_viewer と同じ）")
    p.add_argument("--window", type=float, default=2.0,
                   help="FPS算出の移動ウィンドウ秒（既定 2.0）")
    return p.parse_args()


def autodetect_port():
    import glob
    cands = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not cands:
        print("ポートが見つかりません: /dev/cu.usbmodem*", file=sys.stderr)
        sys.exit(1)
    return cands[0]


def fps_from(times, now, window):
    """ deque(到着時刻) から直近 window 秒の FPS を算出。 """
    while times and now - times[0] > window:
        times.popleft()
    if len(times) < 2:
        return 0.0
    span = times[-1] - times[0]
    return (len(times) - 1) / span if span > 0 else 0.0


def main():
    args = parse_args()
    if args.port is None:
        args.port = autodetect_port()

    ser = open_serial(args, timeout=0.02)
    print(f"FPS計測開始: {args.port} @ {args.baud}  (window={args.window}s)  Ctrl-C で終了")

    buf = bytearray()
    dist_times = deque()     # DIST 行（距離フレーム）の到着時刻
    therm_times = deque()    # 0xAA55 バイナリ（サーマルフレーム）の到着時刻
    last_print = time.time()

    try:
        while True:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)
                buf, msgs = extract_messages(buf)
                now = time.time()
                for kind, payload in msgs:
                    if kind == "thermal_bin":
                        therm_times.append(now)
                    elif kind == "text" and payload.startswith("DIST,"):
                        dist_times.append(now)

            now = time.time()
            if now - last_print >= 1.0:
                d = fps_from(dist_times, now, args.window)
                t = fps_from(therm_times, now, args.window)
                print(f"距離(VL53L5CX) = {d:5.2f} fps   "
                      f"サーマル(MLX90640) = {t:5.2f} fps")
                last_print = now
    except KeyboardInterrupt:
        print("\n終了")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
