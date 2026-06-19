#!/usr/bin/env python3
"""
TerraGuard AI — サーマル(MLX90640) + 距離(VL53L5CX) ブラウザ表示ビューア（Dash/Plotly）

dual_viewer.py（matplotlib版）が「描画が重く遅延がどんどん蓄積する」問題を
解決するためのブラウザ描画版。描画はブラウザ（Plotly.js）が担当し、Python側は
シリアル受信＋差分計算に専念する。

遅延を蓄積させない設計のキモ:
  - シリアル受信を専用スレッドに分離し、常に「最新フレームだけ」を保持する
    （描画が間に合わなくても古いフレームは捨てるので、受信＞描画の差が開かない）。
  - ブラウザは dcc.Interval で定期ポーリングし、毎回その時点の最新フレームを取得する。
    描画がコマ落ちしても、表示は常に最新に追従する（遅延が積み上がらない）。

表示（2×2）:
  - 1行目: 左=サーマル(24×24)、右=距離(8×8)
  - 2行目: 左=サーマル差分(現−前)、右=距離差分    ← eIQ Neutron NPU 入力想定の特徴量

パース仕様は dual_viewer.py と共通（同モジュールから import）。
  - サーマル: バイナリ magic 0xAA55 + Ta(int16) + 768×int16（1/100℃）
  - "DIST,..."/"STAT,..." : 距離(mm)と target_status（テキスト, 各64要素）

使い方:
    tools/.venv/bin/python tools/dual_viewer_web.py [--port /dev/cu.usbmodemXXXX]
    → 表示された http://127.0.0.1:8050 をブラウザで開く

依存: pyserial, numpy, dash, plotly
    tools/.venv/bin/pip install pyserial numpy dash plotly
"""

import argparse
import sys
import threading

import numpy as np
import serial

# パーサ・定数は matplotlib 版と共通のものを再利用する。
from dual_viewer import (
    T_SRC_COLS, T_ROWS, T_PIXELS, T_CROP_COLS, T_CROP_X0, T_COLS,
    D_GRID, D_ZONES, STATUS_VALID,
    parse_frame_line, parse_csv64, extract_messages, flip_grid, open_serial,
)


def parse_args():
    p = argparse.ArgumentParser(description="MLX90640 + VL53L5CX ブラウザ表示ビューア")
    p.add_argument("--port", default="/dev/cu.usbmodemFQI2HWQMUXQ2J3",
                   help="シリアルポート（ls /dev/cu.usbmodem* で確認）")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--t-vmin", type=float, default=15.0, help="サーマル温度下限[℃]")
    p.add_argument("--t-vmax", type=float, default=45.0, help="サーマル温度上限[℃]")
    p.add_argument("--d-vmin", type=float, default=0.0, help="距離スケール下限[mm]")
    p.add_argument("--d-vmax", type=float, default=4000.0, help="距離スケール上限[mm]")
    p.add_argument("--t-diff-range", type=float, default=3.0,
                   help="サーマル差分の表示レンジ ±[℃]（左右対称）")
    p.add_argument("--d-diff-range", type=float, default=300.0,
                   help="距離差分の表示レンジ ±[mm]（左右対称）")
    p.add_argument("--flip-h", action="store_true", help="左右反転（両パネル共通）")
    p.add_argument("--flip-v", action="store_true", help="上下反転（両パネル共通）")
    p.add_argument("--interval", type=int, default=66,
                   help="ブラウザの再描画間隔[ms]（既定66ms≒15fps）")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port-web", type=int, default=8050, help="Dashサーバのポート")
    return p.parse_args()


# ---------------------------------------------------------------------------
# 共有状態: 受信スレッドが書き、Dashコールバックが読む。最新フレームのみ保持。
# ---------------------------------------------------------------------------
class SharedState:
    def __init__(self):
        self._lock = threading.Lock()
        self.t_arr = None      # 最新サーマル(24×24, ℃)
        self.t_diff = None     # サーマル差分(24×24)
        self.t_ta = None       # 周囲温度
        self.d_grid = None     # 最新距離(8×8, mm)
        self.d_diff = None     # 距離差分(8×8)
        self.d_stat = None     # 距離 target_status(8×8)
        self.t_seq = 0         # フレーム連番（新フレーム判定・遅延監視用）
        self.d_seq = 0

    def update_thermal(self, arr, diff, ta):
        with self._lock:
            self.t_arr, self.t_diff, self.t_ta = arr, diff, ta
            self.t_seq += 1

    def update_distance(self, grid, diff, stat):
        with self._lock:
            self.d_grid, self.d_diff, self.d_stat = grid, diff, stat
            self.d_seq += 1

    def snapshot(self):
        with self._lock:
            return {
                "t_arr": self.t_arr, "t_diff": self.t_diff, "t_ta": self.t_ta,
                "d_grid": self.d_grid, "d_diff": self.d_diff, "d_stat": self.d_stat,
                "t_seq": self.t_seq, "d_seq": self.d_seq,
            }


def reader_loop(ser, args, state, stop_evt):
    """ シリアルを常時読み、最新フレームだけを state に上書きし続ける。
        古いフレームは捨てるので、受信＞描画でも遅延が蓄積しない。 """
    buf = bytearray()
    dist = None       # 直近の DIST（STAT とペアになるまで保持）
    prev_t = None     # 直近サーマル配列(24×24) — 差分用
    prev_d = None     # 直近距離配列(8×8) — 差分用
    while not stop_evt.is_set():
        try:
            n = ser.in_waiting
            data = ser.read(n) if n else ser.read(1)  # 最低1B待ちでCPU空回り防止
        except serial.SerialException as e:
            print(f"シリアル読み出しエラー: {e}", file=sys.stderr)
            break
        if not data:
            continue
        buf.extend(data)
        latest_t = None   # (ta, pix[768])
        latest_d = None   # (dist[64], stat[64])
        buf, msgs = extract_messages(buf)
        for kind, payload in msgs:
            if kind == "thermal_bin":
                latest_t = payload
                continue
            line = payload
            ft = parse_frame_line(line)  # 旧テキストFRAME互換
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

        # --- サーマル: 最新フレームを整形し、前フレームとの差分を計算 ---
        if latest_t is not None:
            ta, pix = latest_t
            pix = flip_grid(pix, T_ROWS, T_SRC_COLS, args.flip_h, args.flip_v)
            arr = np.asarray(pix, dtype=float).reshape(T_ROWS, T_SRC_COLS)
            arr = arr[:, T_CROP_X0:T_CROP_X0 + T_CROP_COLS]  # 中央24列
            arr = arr[::-1, :]                                # 取り付け向き補正
            diff = (arr - prev_t) if prev_t is not None else np.zeros_like(arr)
            state.update_thermal(arr, diff, ta)
            prev_t = arr

        # --- 距離: 最新フレームを整形し、前フレームとの差分を計算 ---
        if latest_d is not None:
            dd, ss = latest_d
            dd = flip_grid(dd, D_GRID, D_GRID, args.flip_h, args.flip_v)
            ss = flip_grid(ss, D_GRID, D_GRID, args.flip_h, args.flip_v)
            grid = np.asarray(dd, dtype=float).reshape(D_GRID, D_GRID)
            stat = np.asarray(ss, dtype=int).reshape(D_GRID, D_GRID)
            ddiff = (grid - prev_d) if prev_d is not None else np.zeros_like(grid)
            state.update_distance(grid, ddiff, stat)
            prev_d = grid


def build_app(args, state):
    from dash import Dash, dcc, html, Output, Input
    import plotly.graph_objects as go

    app = Dash(__name__)
    app.title = "TerraGuard AI — Thermal + Distance (Web)"

    def empty_fig(title):
        fig = go.Figure()
        fig.update_layout(
            title=title, margin=dict(l=10, r=10, t=40, b=10),
            xaxis=dict(visible=False), yaxis=dict(visible=False, scaleanchor="x"),
        )
        return fig

    app.layout = html.Div(
        style={"fontFamily": "sans-serif"},
        children=[
            html.H3("TerraGuard AI — Thermal + Distance（ブラウザ描画）"),
            html.Div(id="status", style={"color": "#666", "marginBottom": "6px"}),
            html.Div(
                style={"display": "grid",
                       "gridTemplateColumns": "1fr 1fr",
                       "gap": "8px", "maxWidth": "1000px"},
                children=[
                    dcc.Graph(id="g-thermal", figure=empty_fig("MLX90640")),
                    dcc.Graph(id="g-distance", figure=empty_fig("VL53L5CX")),
                    dcc.Graph(id="g-thermal-diff", figure=empty_fig("MLX90640 diff")),
                    dcc.Graph(id="g-distance-diff", figure=empty_fig("VL53L5CX diff")),
                ],
            ),
            dcc.Interval(id="tick", interval=args.interval, n_intervals=0),
        ],
    )

    def heatmap(z, zmin, zmax, colorscale, title, text=None):
        fig = go.Figure(
            go.Heatmap(z=z, zmin=zmin, zmax=zmax, colorscale=colorscale,
                       text=text, texttemplate="%{text}" if text is not None else None,
                       textfont=dict(size=9))
        )
        fig.update_layout(
            title=title, margin=dict(l=10, r=10, t=40, b=10),
            uirevision="keep",  # ズーム等のUI状態を更新間で保持
        )
        fig.update_xaxes(visible=False)
        # y を反転して画像の上下を一般的な向きにそろえる。
        fig.update_yaxes(visible=False, scaleanchor="x", autorange="reversed")
        return fig

    @app.callback(
        Output("g-thermal", "figure"),
        Output("g-distance", "figure"),
        Output("g-thermal-diff", "figure"),
        Output("g-distance-diff", "figure"),
        Output("status", "children"),
        Input("tick", "n_intervals"),
    )
    def refresh(_n):
        from dash import no_update
        s = state.snapshot()

        # サーマル
        if s["t_arr"] is not None:
            fig_t = heatmap(s["t_arr"], args.t_vmin, args.t_vmax, "Jet", "MLX90640 [°C]")
            fig_td = heatmap(s["t_diff"], -args.t_diff_range, args.t_diff_range,
                             "RdBu_r", "MLX90640 diff [Δ°C]")
            t_info = (f"Ta={s['t_ta']:.2f}°C  "
                      f"min={s['t_arr'].min():.2f} max={s['t_arr'].max():.2f}  "
                      f"|Δ|max={np.abs(s['t_diff']).max():.2f}°C")
        else:
            fig_t = fig_td = no_update
            t_info = "thermal: waiting..."

        # 距離（数値オーバーレイ付き）
        if s["d_grid"] is not None:
            dtxt = np.round(s["d_grid"]).astype(int).astype(str)
            fig_d = heatmap(s["d_grid"], args.d_vmin, args.d_vmax, "Jet_r",
                            "VL53L5CX [mm]", text=dtxt)
            difftxt = np.array([[f"{v:+.0f}" for v in row] for row in s["d_diff"]])
            fig_dd = heatmap(s["d_diff"], -args.d_diff_range, args.d_diff_range,
                             "RdBu_r", "VL53L5CX diff [Δmm]", text=difftxt)
            valid = int(np.isin(s["d_stat"], list(STATUS_VALID)).sum())
            d_info = (f"valid={valid}/64  "
                      f"min={s['d_grid'].min():.0f} max={s['d_grid'].max():.0f} "
                      f"avg={s['d_grid'].mean():.0f}mm")
        else:
            fig_d = fig_dd = no_update
            d_info = "distance: waiting..."

        status = f"thermal#{s['t_seq']}  {t_info}   |   distance#{s['d_seq']}  {d_info}"
        return fig_t, fig_d, fig_td, fig_dd, status

    return app


def main():
    args = parse_args()

    try:
        import dash  # noqa: F401
        import plotly  # noqa: F401
    except ImportError:
        print("dual_viewer_web には dash/plotly が必要です:", file=sys.stderr)
        print("  tools/.venv/bin/pip install dash plotly", file=sys.stderr)
        sys.exit(1)

    ser = open_serial(args, timeout=0.05)
    print(f"接続: {args.port} @ {args.baud}")

    state = SharedState()
    stop_evt = threading.Event()
    reader = threading.Thread(target=reader_loop, args=(ser, args, state, stop_evt),
                              daemon=True)
    reader.start()

    app = build_app(args, state)
    url = f"http://{args.host}:{args.port_web}"
    print(f"ブラウザで開いてください: {url}  （Ctrl+C で終了）")
    try:
        app.run(host=args.host, port=args.port_web, debug=False)
    except KeyboardInterrupt:
        pass
    finally:
        stop_evt.set()
        ser.close()
        print("終了しました。")


if __name__ == "__main__":
    main()
