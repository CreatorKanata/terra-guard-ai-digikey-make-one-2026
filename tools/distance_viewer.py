#!/usr/bin/env python3
"""
TerraGuard AI — VL53L5CX 8×8 距離マップ テスト用ビューア

FRDM-MCXN947 の led_blinky.c が出力する "DIST,<z0>,...,<z63>" 形式
（各値は mm 整数、無効ゾーンは -1）をシリアルから受信し、8×8 の距離マップを表示する。

2つのモード:
  --mode term  … ターミナルに数値グリッドを連続表示（matplotlib 不要・すぐ確認できる）
  --mode gui   … matplotlib でヒートマップをリアルタイム表示（各ゾーンを拡大、近=暖色）

キー操作（gui）:
    s … 現在のフレームを PNG 保存（distance_YYYYmmdd_HHMMSS.png）
    q … 終了

使い方:
    # ターミナルで手軽に（依存は pyserial のみ）
    python tools/distance_viewer.py --mode term

    # ヒートマップ表示（要 numpy/matplotlib）
    tools/.venv/bin/python tools/distance_viewer.py --mode gui

オプション:
    --port /dev/cu.usbmodemXXXX  シリアルポート
    --baud 115200
    --vmin 0 --vmax 4000         距離スケール[mm]（gui）
    --scale 50                   1ゾーンを何pxに拡大するか（gui, 8*50=400px）
    --cmap jet_r                 カラーマップ（gui。_r で近距離を暖色に）
"""

import argparse
import datetime as dt
import sys

import serial

GRID = 8
ZONES = GRID * GRID  # 64


def parse_args():
    p = argparse.ArgumentParser(description="VL53L5CX 8x8 距離ビューア")
    p.add_argument("--mode", choices=["term", "gui"], default="term",
                   help="term=ターミナル数値表示 / gui=ヒートマップ")
    p.add_argument("--port", default="/dev/cu.usbmodemFQI2HWQMUXQ2J3")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--vmin", type=float, default=0.0, help="距離スケール下限[mm]")
    p.add_argument("--vmax", type=float, default=4000.0, help="距離スケール上限[mm]")
    p.add_argument("--scale", type=int, default=50, help="1ゾーンの拡大px（8×scale）")
    p.add_argument("--cmap", default="jet_r", help="カラーマップ（近=暖色は jet_r）")
    p.add_argument("--save-dir", default=".")
    p.add_argument("--flip-h", action="store_true")
    p.add_argument("--flip-v", action="store_true")
    return p.parse_args()


def parse_csv_line(line: str, head: str):
    """ "<head>,v0,...,v63" を 64要素の int リストに変換。不正なら None。 """
    if not line.startswith(head + ","):
        return None
    parts = line.replace("\x00", "").strip().split(",")
    if len(parts) != ZONES + 1:
        return None
    try:
        return [int(x) for x in parts[1:]]
    except ValueError:
        return None


def parse_dist_line(line: str):
    """ "DIST,z0,...,z63" を 64要素の距離[mm]リストに変換（全ゾーン、status不問）。 """
    return parse_csv_line(line, "DIST")


def parse_stat_line(line: str):
    """ "STAT,s0,...,s63" を 64要素の target_status リストに変換。 """
    return parse_csv_line(line, "STAT")


# VL53L5CX target_status の意味（UM2884）。5/9 が高信頼、6/10 は実用可、255=対象なし。
STATUS_VALID = {5, 6, 9, 10}


def open_serial(args):
    try:
        return serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"シリアルを開けません: {e}", file=sys.stderr)
        print("  ポートを確認: ls /dev/cu.usbmodem*", file=sys.stderr)
        sys.exit(1)


def apply_flip(nums, args):
    """ 8×8 として行/列反転を適用し、フラットな64要素に戻す。 """
    if not (args.flip_h or args.flip_v):
        return nums
    grid = [nums[r * GRID:(r + 1) * GRID] for r in range(GRID)]
    if args.flip_h:
        grid = [row[::-1] for row in grid]
    if args.flip_v:
        grid = grid[::-1]
    return [v for row in grid for v in row]


def run_term(args):
    """ ターミナルに数値グリッドを連続表示。DIST(距離) と STAT(status) を対にして描画。
        status==255（対象なし）のゾーンは距離値の代わりに '·' を表示する。 """
    ser = open_serial(args)
    print(f"接続: {args.port} @ {args.baud}  （Ctrl-C で終了）")
    buf = ""
    dist = None
    try:
        while True:
            data = ser.read(2048)
            if not data:
                continue
            buf += data.decode("utf-8", errors="replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                d = parse_dist_line(line)
                if d is not None:
                    dist = d
                    continue
                stat = parse_stat_line(line)
                if stat is None or dist is None:
                    continue
                # DIST に続く STAT が来たタイミングで1フレーム描画
                dd = apply_flip(dist, args)
                ss = apply_flip(stat, args)
                valid = [dd[i] for i in range(ZONES) if ss[i] in STATUS_VALID]
                print("\033[2J\033[H", end="")  # clear + home
                print(f"VL53L5CX 8x8 距離[mm]  有効ゾーン={len(valid)}/64  "
                      f"(·=対象なし status255)", end="")
                if valid:
                    print(f"\n  min={min(valid)}  max={max(valid)}  avg={sum(valid)//len(valid)} mm")
                else:
                    print()
                for r in range(GRID):
                    cells = []
                    for c in range(GRID):
                        i = r * GRID + c
                        if ss[i] == 255:
                            cells.append("    ·")
                        else:
                            cells.append(f"{dd[i]:5d}")
                    print(" ".join(cells))
                print("\n（Ctrl-C で終了）")
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print("\n終了しました。")


def run_gui(args):
    """ matplotlib でヒートマップをリアルタイム表示。 """
    try:
        import numpy as np
        import matplotlib.pyplot as plt
    except ImportError:
        print("gui モードには numpy/matplotlib が必要です:", file=sys.stderr)
        print("  tools/.venv/bin/python tools/distance_viewer.py --mode gui", file=sys.stderr)
        sys.exit(1)

    ser = open_serial(args)
    print(f"接続: {args.port} @ {args.baud}  （s=保存 / q=終了）")

    plt.ion()
    fig, ax = plt.subplots(figsize=(GRID * args.scale / 100 + 1, GRID * args.scale / 100), dpi=100)
    fig.canvas.manager.set_window_title("TerraGuard AI — VL53L5CX Distance")
    init = np.full((GRID, GRID), np.nan)
    im = ax.imshow(init, cmap=args.cmap, vmin=args.vmin, vmax=args.vmax, interpolation="nearest")
    ax.set_xticks([]); ax.set_yticks([])
    cbar = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    cbar.set_label("Distance [mm]")
    title = ax.set_title("waiting for frame...")
    # 各ゾーンに数値を重ねる
    texts = [[ax.text(c, r, "", ha="center", va="center", fontsize=7, color="black")
              for c in range(GRID)] for r in range(GRID)]

    state = {"save": False}

    def on_key(event):
        if event.key == "s":
            state["save"] = True
        elif event.key == "q":
            plt.close(fig)

    fig.canvas.mpl_connect("key_press_event", on_key)

    buf = ""
    dist = None
    try:
        while plt.fignum_exists(fig.number):
            data = ser.read(2048)
            if data:
                buf += data.decode("utf-8", errors="replace")
                latest_d = None
                latest_s = None
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    d = parse_dist_line(line)
                    if d is not None:
                        dist = d
                        continue
                    s = parse_stat_line(line)
                    if s is not None and dist is not None:
                        latest_d = apply_flip(dist, args)
                        latest_s = apply_flip(s, args)
                if latest_d is not None and latest_s is not None:
                    grid = np.array(latest_d, dtype=float).reshape(GRID, GRID)
                    smask = np.array(latest_s).reshape(GRID, GRID)
                    grid[smask == 255] = np.nan  # 対象なしゾーンは描画しない
                    im.set_data(grid)
                    valid = [latest_d[i] for i in range(ZONES) if latest_s[i] in STATUS_VALID]
                    if valid:
                        title.set_text(f"有効={len(valid)}/64  min={min(valid)} max={max(valid)} "
                                       f"avg={sum(valid)//len(valid)} mm")
                    for r in range(GRID):
                        for c in range(GRID):
                            i = r * GRID + c
                            texts[r][c].set_text("" if latest_s[i] == 255 else f"{latest_d[i]}")

                    if state["save"]:
                        state["save"] = False
                        stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
                        path = f"{args.save_dir.rstrip('/')}/distance_{stamp}.png"
                        fig.savefig(path, dpi=100, bbox_inches="tight")
                        print(f"保存: {path}")
            plt.pause(0.03)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        plt.ioff()
        print("終了しました。")


def main():
    args = parse_args()
    if args.mode == "term":
        run_term(args)
    else:
        run_gui(args)


if __name__ == "__main__":
    main()
