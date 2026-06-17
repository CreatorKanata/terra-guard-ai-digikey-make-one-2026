#!/usr/bin/env python3
"""
TerraGuard AI — MLX90640 サーマルフレーム ヒートマップビューア

FRDM-MCXN947 の led_blinky.c が出力する "FRAME,<Ta>,<t0>,...,<t767>" 形式
（各値は 1/100℃ 単位の整数）をシリアルから受信し、32×24 のサーマル画像を
リアルタイムにヒートマップ表示する。各画素は 10倍に拡大して 320×240 で描画する。

- カラーマップ: jet
- 温度スケール: 固定 15〜35℃（フレーム間の絶対温度を比較しやすい）
- キー操作:
    s … 現在のフレームを PNG 保存（thermal_YYYYmmdd_HHMMSS.png）
    q … 終了

使い方:
    python tools/thermal_viewer.py [--port /dev/cu.usbmodemXXXX] [--baud 115200]
                                   [--vmin 15] [--vmax 35] [--scale 10]
                                   [--save-dir .]

依存: pyserial, numpy, matplotlib
    python -m venv tools/.venv && tools/.venv/bin/pip install pyserial numpy matplotlib
"""

import argparse
import datetime as dt
import sys

import numpy as np
import serial
import matplotlib.pyplot as plt

# MLX90640 の解像度（横32 × 縦24）
COLS = 32
ROWS = 24
PIXELS = COLS * ROWS  # 768


def parse_args():
    p = argparse.ArgumentParser(description="MLX90640 サーマルヒートマップビューア")
    p.add_argument("--port", default="/dev/cu.usbmodemFQI2HWQMUXQ2J3",
                   help="シリアルポート（ls /dev/cu.usbmodem* で確認）")
    p.add_argument("--baud", type=int, default=921600, help="ボーレート")
    p.add_argument("--vmin", type=float, default=15.0, help="温度スケール下限[℃]")
    p.add_argument("--vmax", type=float, default=35.0, help="温度スケール上限[℃]")
    p.add_argument("--scale", type=int, default=10, help="1画素を何pxに拡大するか")
    p.add_argument("--cmap", default="jet", help="matplotlib カラーマップ名")
    p.add_argument("--save-dir", default=".", help="PNG 保存先ディレクトリ")
    p.add_argument("--flip-h", action="store_true", help="左右反転（センサ向きに合わせる）")
    p.add_argument("--flip-v", action="store_true", help="上下反転")
    return p.parse_args()


def parse_frame_line(line: str):
    """ "FRAME,Ta,t0,...,t767" を (Ta[℃], 24x32 の温度[℃] ndarray) に変換。
        不正な行は None を返す。 """
    if not line.startswith("FRAME,"):
        return None
    parts = line.strip().split(",")
    # FRAME + Ta + 768画素 = 770 フィールド
    if len(parts) != PIXELS + 2:
        return None
    try:
        vals = [int(x) for x in parts[1:]]  # Ta と 768画素（1/100℃）
    except ValueError:
        return None
    ta = vals[0] / 100.0
    pix = np.asarray(vals[1:], dtype=np.float32) / 100.0  # ℃
    # MLX90640 の画素並びは行優先（row0 col0..31, row1 ...）。32列×24行に整形。
    img = pix.reshape(ROWS, COLS)
    return ta, img


def upscale(img: np.ndarray, scale: int) -> np.ndarray:
    """ 最近傍で各画素を scale 倍に拡大（32×24 → 320×240）。 """
    return np.kron(img, np.ones((scale, scale), dtype=img.dtype))


def main():
    args = parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"シリアルを開けません: {e}", file=sys.stderr)
        print("  ポートを確認: ls /dev/cu.usbmodem*", file=sys.stderr)
        sys.exit(1)

    print(f"接続: {args.port} @ {args.baud}  （s=保存 / q=終了）")

    # 表示の初期化（最初は空画像）
    plt.ion()
    fig, ax = plt.subplots(figsize=(COLS * args.scale / 100, ROWS * args.scale / 100), dpi=100)
    fig.canvas.manager.set_window_title("TerraGuard AI — MLX90640 Thermal")
    init = np.full((ROWS * args.scale, COLS * args.scale), args.vmin, dtype=np.float32)
    im = ax.imshow(init, cmap=args.cmap, vmin=args.vmin, vmax=args.vmax,
                   interpolation="nearest", aspect="equal")
    ax.set_xticks([])
    ax.set_yticks([])
    cbar = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    cbar.set_label("Temperature [°C]")
    title = ax.set_title("waiting for frame...")

    state = {"latest": None, "ta": None, "save": False, "quit": False}

    def on_key(event):
        if event.key == "s":
            state["save"] = True
        elif event.key == "q":
            state["quit"] = True

    fig.canvas.mpl_connect("key_press_event", on_key)

    buf = ""
    try:
        while not state["quit"]:
            # シリアルから読めるだけ読み、行に分割
            data = ser.read(4096)
            if data:
                buf += data.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    parsed = parse_frame_line(line)
                    if parsed is not None:
                        ta, img = parsed
                        if args.flip_h:
                            img = img[:, ::-1]
                        if args.flip_v:
                            img = img[::-1, :]
                        state["latest"] = img
                        state["ta"] = ta

            # 最新フレームがあれば描画更新
            if state["latest"] is not None:
                big = upscale(state["latest"], args.scale)
                im.set_data(big)
                vmin_obs = float(state["latest"].min())
                vmax_obs = float(state["latest"].max())
                title.set_text(
                    f"Ta={state['ta']:.2f}°C  min={vmin_obs:.2f}  max={vmax_obs:.2f}  "
                    f"({COLS*args.scale}x{ROWS*args.scale}px)"
                )

                if state["save"]:
                    state["save"] = False
                    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
                    path = f"{args.save_dir.rstrip('/')}/thermal_{stamp}.png"
                    fig.savefig(path, dpi=100, bbox_inches="tight")
                    print(f"保存: {path}")

            plt.pause(0.05)

            # ウィンドウが閉じられたら終了
            if not plt.fignum_exists(fig.number):
                break
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        plt.ioff()
        print("終了しました。")


if __name__ == "__main__":
    main()
