#!/usr/bin/env python3
"""
TerraGuard AI — サーマル(MLX90640) + 距離(VL53L5CX) 同時リアルタイムビューア

FRDM-MCXN947 が同一シリアルへ出力する、バイナリ＋テキスト混在ストリームを
1本からパースし、1つのウィンドウに 2×2 で表示する。
  - 1行目: 左=サーマル(24×24)、右=距離(8×8)
  - 2行目: 左=サーマル差分(現フレーム−前フレーム)、右=距離差分
フレーム差分（符号付き、発散カラー）は eIQ Neutron NPU への入力を想定した
動き検知用の特徴量。0 を中心に温度/距離の上昇=暖色・下降=寒色で描画する。

  - サーマル（バイナリ・高速）:
      magic 0xAA 0x55 + Ta(int16 LE, 1/100℃) + 768×int16 LE(1/100℃) = 1540B
  - "DIST,<z0>,...,<z63>"   距離（mm 整数、全ゾーン。テキスト）
  - "STAT,<s0>,...,<s63>"   距離の target_status（5/6/9/10=有効, 255=対象なし。テキスト）

旧テキスト形式 "FRAME,<Ta>,..." も後方互換でパースできる（通常ファームは
バイナリで出力する）。

サーマルと距離はフレームレートが異なる（MLX≈2Hz, VL53≈10Hz）が、それぞれ
最後に受信したフレームを保持して独立に再描画する。

キー操作:
    s … 現在の両パネルを1枚の PNG に保存（dual_YYYYmmdd_HHMMSS.png）
    q … 終了

使い方:
    tools/.venv/bin/python tools/dual_viewer.py [--port /dev/cu.usbmodemXXXX]

オプション:
    --port /dev/cu.usbmodemXXXX  シリアルポート
    --baud 115200
    --t-vmin 15 --t-vmax 45      サーマル温度スケール[℃]
    --d-vmin 0 --d-vmax 4000     距離スケール[mm]
    --t-cmap jet                 サーマルのカラーマップ
    --d-cmap jet_r               距離のカラーマップ（_r で近距離を暖色に）
    --diff-cmap coolwarm         フレーム差分のカラーマップ（0中心の発散カラー）
    --t-diff-range 3             サーマル差分の表示レンジ ±[℃]
    --d-diff-range 300           距離差分の表示レンジ ±[mm]
    --flip-h / --flip-v          両パネル共通の左右/上下反転（センサ向き合わせ）

依存: pyserial, numpy, matplotlib
    python -m venv tools/.venv && tools/.venv/bin/pip install pyserial numpy matplotlib
"""

import argparse
import datetime as dt
import sys

import serial

# サーマル MLX90640（生: 横32 × 縦24）
T_SRC_COLS = 32
T_ROWS = 24
T_PIXELS = T_SRC_COLS * T_ROWS  # 768

# 中央24列を切り出して 24×24 にする（左右4列ずつ捨てる）。
T_CROP_COLS = 24
T_CROP_X0 = (T_SRC_COLS - T_CROP_COLS) // 2  # = 4
# 切り出し後の表示上の列数。
T_COLS = T_CROP_COLS

# 距離 VL53L5CX（8×8）
D_GRID = 8
D_ZONES = D_GRID * D_GRID  # 64

# VL53L5CX target_status の意味（UM2884）。5/9 が高信頼、6/10 は実用可、255=対象なし。
STATUS_VALID = {5, 6, 9, 10}


def parse_args():
    p = argparse.ArgumentParser(description="MLX90640 + VL53L5CX 同時ビューア")
    p.add_argument("--port", default="/dev/cu.usbmodemFQI2HWQMUXQ2J3",
                   help="シリアルポート（ls /dev/cu.usbmodem* で確認）")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--t-vmin", type=float, default=15.0, help="サーマル温度下限[℃]")
    p.add_argument("--t-vmax", type=float, default=45.0, help="サーマル温度上限[℃]")
    p.add_argument("--d-vmin", type=float, default=0.0, help="距離スケール下限[mm]")
    p.add_argument("--d-vmax", type=float, default=4000.0, help="距離スケール上限[mm]")
    p.add_argument("--t-cmap", default="jet", help="サーマルのカラーマップ")
    p.add_argument("--d-cmap", default="jet_r", help="距離のカラーマップ（近=暖色は jet_r）")
    p.add_argument("--diff-cmap", default="coolwarm",
                   help="フレーム差分のカラーマップ（0が中心の発散カラー）")
    p.add_argument("--t-diff-range", type=float, default=3.0,
                   help="サーマル差分の表示レンジ ±[℃]（左右対称）")
    p.add_argument("--d-diff-range", type=float, default=300.0,
                   help="距離差分の表示レンジ ±[mm]（左右対称）")
    p.add_argument("--save-dir", default=".")
    p.add_argument("--flip-h", action="store_true", help="左右反転（両パネル共通）")
    p.add_argument("--flip-v", action="store_true", help="上下反転（両パネル共通）")
    return p.parse_args()


# ---------------------------------------------------------------------------
# パーサ（既存 thermal_viewer.py / distance_viewer.py と同一仕様）
# ---------------------------------------------------------------------------
def parse_frame_line(line: str):
    """ "FRAME,Ta,t0,...,t767" を (Ta[℃], 768要素の温度[℃] list) に変換。不正なら None。 """
    if not line.startswith("FRAME,"):
        return None
    parts = line.replace("\x00", "").strip().split(",")
    if len(parts) != T_PIXELS + 2:  # FRAME + Ta + 768
        return None
    try:
        vals = [int(x) for x in parts[1:]]
    except ValueError:
        return None
    ta = vals[0] / 100.0
    pix = [v / 100.0 for v in vals[1:]]
    return ta, pix


def parse_csv64(line: str, head: str):
    """ "<head>,v0,...,v63" を 64要素の int list に変換。不正なら None。 """
    if not line.startswith(head + ","):
        return None
    parts = line.replace("\x00", "").strip().split(",")
    if len(parts) != D_ZONES + 1:
        return None
    try:
        return [int(x) for x in parts[1:]]
    except ValueError:
        return None


# バイナリサーマルフレーム: magic(0xAA 0x55) + Ta(int16 LE) + 768×int16 LE = 1540B
BIN_MAGIC = b"\xAA\x55"
BIN_FRAME_LEN = 2 + 2 + T_PIXELS * 2  # 1540


def extract_messages(buf: bytearray):
    """ テキスト行(\\n終端)とバイナリフレーム(0xAA55...)が混在する bytearray から
        メッセージを順に取り出す。戻り値は (残りbuf, [msg,...])。
        msg は ("text", str) か ("thermal_bin", (ta, pix[768])) のタプル。
        - 不完全な末尾は buf に残す（次回読み足し）。 """
    import struct
    out = []
    i = 0
    n = len(buf)
    while i < n:
        b = buf[i]
        if b == 0xAA and i + 1 < n and buf[i + 1] == 0x55:
            # バイナリフレーム候補。全長そろっていなければ中断（末尾に残す）。
            if i + BIN_FRAME_LEN > n:
                break
            payload = bytes(buf[i + 2:i + BIN_FRAME_LEN])  # Ta+768画素 = 1538B
            vals = struct.unpack("<" + "h" * (1 + T_PIXELS), payload)
            ta = vals[0] / 100.0
            pix = [v / 100.0 for v in vals[1:]]
            out.append(("thermal_bin", (ta, pix)))
            i += BIN_FRAME_LEN
            continue
        # テキスト行: 次の \n まで。無ければ中断（末尾に残す）。
        nl = buf.find(b"\n", i)
        if nl == -1:
            break
        line = bytes(buf[i:nl]).decode("utf-8", "replace")
        out.append(("text", line))
        i = nl + 1
    return buf[i:], out


def flip_grid(flat, rows, cols, flip_h, flip_v):
    """ 1次元 list を rows×cols とみなして行/列反転し、再び1次元 list に戻す。 """
    if not (flip_h or flip_v):
        return flat
    grid = [flat[r * cols:(r + 1) * cols] for r in range(rows)]
    if flip_h:
        grid = [row[::-1] for row in grid]
    if flip_v:
        grid = grid[::-1]
    return [v for row in grid for v in row]


def open_serial(args, timeout=0.02):
    try:
        return serial.Serial(args.port, args.baud, timeout=timeout)
    except serial.SerialException as e:
        print(f"シリアルを開けません: {e}", file=sys.stderr)
        print("  ポートを確認: ls /dev/cu.usbmodem*", file=sys.stderr)
        sys.exit(1)


def main():
    args = parse_args()

    try:
        import numpy as np
        import matplotlib.pyplot as plt
    except ImportError:
        print("dual_viewer には numpy/matplotlib が必要です:", file=sys.stderr)
        print("  tools/.venv/bin/python tools/dual_viewer.py", file=sys.stderr)
        sys.exit(1)

    # GUI を固めないよう短い timeout でノンブロッキング読み出しする。
    ser = open_serial(args)
    print(f"接続: {args.port} @ {args.baud}  （s=保存 / q=終了）")

    plt.ion()
    # 2×2: 1行目=現フレーム（サーマル/距離）、2行目=フレーム差分（NPU入力用）
    fig, ((ax_t, ax_d), (ax_td, ax_dd)) = plt.subplots(2, 2, figsize=(11, 9), dpi=100)
    fig.canvas.manager.set_window_title("TerraGuard AI — Thermal + Distance")

    # 1行目左: サーマル（24×24）
    t_init = np.full((T_ROWS, T_COLS), args.t_vmin, dtype=float)
    im_t = ax_t.imshow(t_init, cmap=args.t_cmap, vmin=args.t_vmin, vmax=args.t_vmax,
                       interpolation="nearest", aspect="equal")
    ax_t.set_xticks([]); ax_t.set_yticks([])
    cbar_t = fig.colorbar(im_t, ax=ax_t, fraction=0.046, pad=0.04)
    cbar_t.set_label("Temperature [°C]")
    title_t = ax_t.set_title("MLX90640: waiting...")

    # 1行目右: 距離（8×8）。各ゾーンに数値を重ねる。
    d_init = np.full((D_GRID, D_GRID), np.nan)
    im_d = ax_d.imshow(d_init, cmap=args.d_cmap, vmin=args.d_vmin, vmax=args.d_vmax,
                       interpolation="nearest", aspect="equal")
    ax_d.set_xticks([]); ax_d.set_yticks([])
    cbar_d = fig.colorbar(im_d, ax=ax_d, fraction=0.046, pad=0.04)
    cbar_d.set_label("Distance [mm]")
    title_d = ax_d.set_title("VL53L5CX: waiting...")
    d_texts = [[ax_d.text(c, r, "", ha="center", va="center", fontsize=7, color="black")
                for c in range(D_GRID)] for r in range(D_GRID)]

    # 2行目左: サーマル差分（現−前、符号付き）。0を中心に ±t_diff_range。
    td_init = np.zeros((T_ROWS, T_COLS), dtype=float)
    im_td = ax_td.imshow(td_init, cmap=args.diff_cmap,
                         vmin=-args.t_diff_range, vmax=args.t_diff_range,
                         interpolation="nearest", aspect="equal")
    ax_td.set_xticks([]); ax_td.set_yticks([])
    cbar_td = fig.colorbar(im_td, ax=ax_td, fraction=0.046, pad=0.04)
    cbar_td.set_label("ΔTemperature [°C]")
    title_td = ax_td.set_title("MLX90640 diff: waiting...")

    # 2行目右: 距離差分（現−前、符号付き）。0を中心に ±d_diff_range。
    dd_init = np.zeros((D_GRID, D_GRID), dtype=float)
    im_dd = ax_dd.imshow(dd_init, cmap=args.diff_cmap,
                         vmin=-args.d_diff_range, vmax=args.d_diff_range,
                         interpolation="nearest", aspect="equal")
    ax_dd.set_xticks([]); ax_dd.set_yticks([])
    cbar_dd = fig.colorbar(im_dd, ax=ax_dd, fraction=0.046, pad=0.04)
    cbar_dd.set_label("ΔDistance [mm]")
    title_dd = ax_dd.set_title("VL53L5CX diff: waiting...")
    dd_texts = [[ax_dd.text(c, r, "", ha="center", va="center", fontsize=7, color="black")
                 for c in range(D_GRID)] for r in range(D_GRID)]

    fig.tight_layout()

    state = {"save": False}

    def on_key(event):
        if event.key == "s":
            state["save"] = True
        elif event.key == "q":
            plt.close(fig)

    fig.canvas.mpl_connect("key_press_event", on_key)

    buf = bytearray()
    dist = None  # 直近の DIST（STAT とペアになるまで保持）
    prev_t = None  # 直近に描画したサーマル配列(24×24) — 差分計算用
    prev_d = None  # 直近に描画した距離配列(8×8) — 差分計算用
    try:
        while plt.fignum_exists(fig.number):
            n = ser.in_waiting
            data = ser.read(n) if n else b""
            if data:
                buf.extend(data)
                latest_t = None   # (ta, pix[768])
                latest_d = None   # (dist[64], stat[64])
                # 溜まった分を全部パース（テキスト行＋バイナリフレーム混在）。
                # 各センサの最新フレームのみ採用（古いフレームはスキップ）。
                buf, msgs = extract_messages(buf)
                for kind, payload in msgs:
                    if kind == "thermal_bin":
                        latest_t = payload  # (ta, pix[768])
                        continue
                    # kind == "text"
                    line = payload

                    ft = parse_frame_line(line)  # 旧テキストFRAME互換（通常は来ない）
                    if ft is not None:
                        latest_t = ft
                        continue

                    d = parse_csv64(line, "DIST")
                    if d is not None:
                        dist = d
                        continue

                    s = parse_csv64(line, "STAT")
                    if s is not None and dist is not None:
                        latest_d = (dist, s)

                # --- サーマル更新 ---
                if latest_t is not None:
                    ta, pix = latest_t
                    pix = flip_grid(pix, T_ROWS, T_SRC_COLS, args.flip_h, args.flip_v)
                    arr = np.asarray(pix, dtype=float).reshape(T_ROWS, T_SRC_COLS)
                    # 中央24列だけを切り出して 24×24 にする（左右4列ずつ捨てる）。
                    arr = arr[:, T_CROP_X0:T_CROP_X0 + T_CROP_COLS]
                    # センサの取り付け向きに合わせて常に上下反転する。
                    arr = arr[::-1, :]
                    im_t.set_data(arr)
                    title_t.set_text(
                        f"MLX90640  Ta={ta:.2f}°C  "
                        f"min={arr.min():.2f}  max={arr.max():.2f}"
                    )

                    # --- サーマル差分（現フレーム − 前フレーム, NPU入力用）---
                    if prev_t is not None:
                        diff = arr - prev_t
                        im_td.set_data(diff)
                        title_td.set_text(
                            f"MLX90640 diff  "
                            f"min={diff.min():+.2f}  max={diff.max():+.2f}  "
                            f"|max|={np.abs(diff).max():.2f}°C"
                        )
                    prev_t = arr

                # --- 距離更新 ---
                if latest_d is not None:
                    dd, ss = latest_d
                    dd = flip_grid(dd, D_GRID, D_GRID, args.flip_h, args.flip_v)
                    ss = flip_grid(ss, D_GRID, D_GRID, args.flip_h, args.flip_v)
                    grid = np.asarray(dd, dtype=float).reshape(D_GRID, D_GRID)
                    im_d.set_data(grid)
                    valid = [dd[i] for i in range(D_ZONES) if ss[i] in STATUS_VALID]
                    title_d.set_text(
                        f"VL53L5CX  valid={len(valid)}/64  "
                        f"min={min(dd)} max={max(dd)} avg={sum(dd)//D_ZONES} mm"
                    )
                    for r in range(D_GRID):
                        for c in range(D_GRID):
                            i = r * D_GRID + c
                            if ss[i] == 255:  # 低信頼は括弧付き・グレー
                                d_texts[r][c].set_text(f"({dd[i]})")
                                d_texts[r][c].set_color("dimgray")
                                d_texts[r][c].set_fontsize(6)
                            else:
                                d_texts[r][c].set_text(f"{dd[i]}")
                                d_texts[r][c].set_color("black")
                                d_texts[r][c].set_fontsize(7)

                    # --- 距離差分（現フレーム − 前フレーム, NPU入力用）---
                    if prev_d is not None:
                        ddiff = grid - prev_d
                        im_dd.set_data(ddiff)
                        title_dd.set_text(
                            f"VL53L5CX diff  "
                            f"min={ddiff.min():+.0f}  max={ddiff.max():+.0f}  "
                            f"|max|={np.abs(ddiff).max():.0f} mm"
                        )
                        for r in range(D_GRID):
                            for c in range(D_GRID):
                                v = ddiff[r, c]
                                dd_texts[r][c].set_text(f"{v:+.0f}")
                                # 変化が小さいゾーンは薄く（ノイズを目立たせない）
                                if abs(v) < args.d_diff_range * 0.1:
                                    dd_texts[r][c].set_color("dimgray")
                                else:
                                    dd_texts[r][c].set_color("black")
                    prev_d = grid

            if state["save"]:
                state["save"] = False
                stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
                path = f"{args.save_dir.rstrip('/')}/dual_{stamp}.png"
                fig.savefig(path, dpi=100, bbox_inches="tight")
                print(f"保存: {path}")

            plt.pause(0.03)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        plt.ioff()
        print("終了しました。")


if __name__ == "__main__":
    main()
