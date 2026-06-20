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
import time
from collections import deque

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
        # 背景差分の候補判定(DET行)。(candidate, t_max_c, t_area, d_max, d_area)。
        self.det = None
        # 実FPS計測用: 各センサのフレーム到着時刻を移動ウィンドウで保持。
        self._fps_window = 2.0           # FPS算出の移動ウィンドウ[秒]
        self.t_times = deque()           # サーマルフレーム到着時刻
        self.d_times = deque()           # 距離フレーム到着時刻

    @staticmethod
    def _fps(times, now, window):
        """ 到着時刻 deque から直近 window 秒の FPS を算出（古い時刻は捨てる）。 """
        while times and now - times[0] > window:
            times.popleft()
        if len(times) < 2:
            return 0.0
        span = times[-1] - times[0]
        return (len(times) - 1) / span if span > 0 else 0.0

    def update_thermal(self, arr, diff, ta):
        with self._lock:
            self.t_arr, self.t_diff, self.t_ta = arr, diff, ta
            self.t_seq += 1
            self.t_times.append(time.monotonic())

    def update_distance(self, grid, diff, stat):
        with self._lock:
            self.d_grid, self.d_diff, self.d_stat = grid, diff, stat
            self.d_seq += 1
            self.d_times.append(time.monotonic())

    def update_det(self, det):
        with self._lock:
            self.det = det

    def snapshot(self):
        with self._lock:
            now = time.monotonic()
            return {
                "t_arr": self.t_arr, "t_diff": self.t_diff, "t_ta": self.t_ta,
                "d_grid": self.d_grid, "d_diff": self.d_diff, "d_stat": self.d_stat,
                "t_seq": self.t_seq, "d_seq": self.d_seq, "det": self.det,
                "t_fps": self._fps(self.t_times, now, self._fps_window),
                "d_fps": self._fps(self.d_times, now, self._fps_window),
            }


def reader_loop(ser, args, state, stop_evt):
    """ シリアルを常時読み、最新フレームだけを state に上書きし続ける。
        古いフレームは捨てるので、受信＞描画でも遅延が蓄積しない。 """
    buf = bytearray()
    dist = None       # 直近の DIST（STAT とペアになるまで保持）
    t_fg = None       # 直近サーマル前景(24×24, ℃) — ファーム背景差分の結果
    d_fg = None       # 直近距離前景(8×8, mm) — DFG行で更新（DIST受信では消さない）
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
        latest_t = None    # (ta, pix[768])  生サーマル
        latest_d = None    # (dist[64], stat[64])
        buf, msgs = extract_messages(buf)
        for kind, payload in msgs:
            if kind == "thermal_bin":
                latest_t = payload
                continue
            if kind == "thermal_fg_bin":
                # サーマル前景(768画素, ℃)を生フレームと同じ向きに整形して保持。
                pix = flip_grid(payload, T_ROWS, T_SRC_COLS,
                                args.flip_h, args.flip_v)
                fg = np.asarray(pix, dtype=float).reshape(T_ROWS, T_SRC_COLS)
                fg = fg[:, T_CROP_X0:T_CROP_X0 + T_CROP_COLS]
                t_fg = fg[::-1, :]
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
                continue
            df = parse_csv64(line, "DFG")
            if df is not None:
                # 距離前景を即時整形して永続保持（DIST受信で消さない）。
                dff = flip_grid(df, D_GRID, D_GRID, args.flip_h, args.flip_v)
                d_fg = np.asarray(dff, dtype=float).reshape(D_GRID, D_GRID)
                continue
            if line.startswith("DET,"):
                parts = line.strip().split(",")
                if len(parts) == 6:
                    try:
                        state.update_det(tuple(int(x) for x in parts[1:]))
                    except ValueError:
                        pass

        # --- サーマル: 生フレームを整形。diffパネルにはファーム背景差分(前景)を表示 ---
        if latest_t is not None:
            ta, pix = latest_t
            pix = flip_grid(pix, T_ROWS, T_SRC_COLS, args.flip_h, args.flip_v)
            arr = np.asarray(pix, dtype=float).reshape(T_ROWS, T_SRC_COLS)
            arr = arr[:, T_CROP_X0:T_CROP_X0 + T_CROP_COLS]  # 中央24列
            arr = arr[::-1, :]                                # 取り付け向き補正
            # 前景未受信(背景確立前)はゼロマップ。
            fg = t_fg if t_fg is not None else np.zeros_like(arr)
            state.update_thermal(arr, fg, ta)

        # --- 距離: 生フレームを整形。diffパネルにはファーム背景差分(前景)を表示 ---
        if latest_d is not None:
            dd, ss = latest_d
            dd = flip_grid(dd, D_GRID, D_GRID, args.flip_h, args.flip_v)
            ss = flip_grid(ss, D_GRID, D_GRID, args.flip_h, args.flip_v)
            grid = np.asarray(dd, dtype=float).reshape(D_GRID, D_GRID)
            stat = np.asarray(ss, dtype=int).reshape(D_GRID, D_GRID)
            # ファームが無効ゾーンに出す -1 を NaN にしてグレー表示にする。
            grid[grid < 0] = np.nan
            # 距離前景(背景差分): 永続保持の d_fg を使う。未受信時のみゼロマップ。
            # （DFG が DIST と別の読み取りバッチに割れても前景を消さない＝全消えバグ修正）
            dfg_arr = d_fg if d_fg is not None else np.zeros_like(grid)
            state.update_distance(grid, dfg_arr, stat)


def build_app(args, state, ser):
    from dash import Dash, dcc, html, Output, Input, State
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

    # レイアウト: 横長。左=2×2チャートグリッド、右=操作&ステータスのサイドパネル。
    # 背景リセットボタンと候補(candidate)バナーは、チャートの上ではなく
    # チャート右側の空きスペース（サイドパネル）に置く。
    app.layout = html.Div(
        style={"fontFamily": "sans-serif"},
        children=[
            html.H3("TerraGuard AI Demo"),
            html.Div(
                # 全体を横並び: [チャートグリッド] [サイドパネル]
                style={"display": "flex", "flexDirection": "row",
                       "gap": "20px", "alignItems": "flex-start"},
                children=[
                    # 左: 2×2 チャートグリッド（左列=thermal, 右列=distance）
                    html.Div(
                        style={"display": "grid",
                               "gridTemplateColumns": "1fr 1fr",
                               "gap": "16px", "width": "1000px", "flex": "0 0 auto"},
                        children=[
                            html.Div(
                                style={"display": "flex", "flexDirection": "column",
                                       "gap": "8px"},
                                children=[
                                    dcc.Graph(id="g-thermal",
                                              figure=empty_fig("MLX90640")),
                                    dcc.Graph(id="g-thermal-diff",
                                              figure=empty_fig("MLX90640 foreground")),
                                ],
                            ),
                            html.Div(
                                style={"display": "flex", "flexDirection": "column",
                                       "gap": "8px"},
                                children=[
                                    dcc.Graph(id="g-distance",
                                              figure=empty_fig("VL53L5CX")),
                                    dcc.Graph(id="g-distance-diff",
                                              figure=empty_fig("VL53L5CX foreground")),
                                ],
                            ),
                        ],
                    ),
                    # 右: サイドパネル（操作ボタン + candidate バナー + ステータス）
                    html.Div(
                        style={"flex": "1 1 auto", "minWidth": "280px",
                               "display": "flex", "flexDirection": "column",
                               "gap": "12px"},
                        children=[
                            # 背景リセット: 押した瞬間を新しい背景として取り直すよう
                            # ファームへ 'R' を送る。UI文言は英語で統一する。
                            # ボタンは左寄せ・自動幅。メッセージはボタンの右隣に置く。
                            html.Div(
                                style={"display": "flex", "flexDirection": "row",
                                       "alignItems": "center", "gap": "10px"},
                                children=[
                                    html.Button(
                                        "Reset background (R)", id="btn-bg-reset",
                                        n_clicks=0,
                                        style={"fontSize": "14px",
                                               "padding": "8px 14px",
                                               "background": "#2471a3",
                                               "color": "#fff", "border": "none",
                                               "borderRadius": "4px",
                                               "cursor": "pointer",
                                               "flex": "0 0 auto"}),
                                    html.Span(id="reset-msg",
                                              style={"color": "#2471a3",
                                                     "fontSize": "12px"}),
                                ],
                            ),
                            # 候補バナー + ステータス（refresh コールバックが更新）。
                            html.Div(id="status", style={"color": "#666"}),
                        ],
                    ),
                ],
            ),
            dcc.Interval(id="tick", interval=args.interval, n_intervals=0),
        ],
    )

    def heatmap(z, zmin, zmax, colorscale, title, text=None):
        fig = go.Figure(
            go.Heatmap(z=z, zmin=zmin, zmax=zmax, colorscale=colorscale,
                       text=text, texttemplate="%{text}" if text is not None else None,
                       textfont=dict(size=9),
                       hoverongaps=False)  # NaN セルはホバーも無効
        )
        fig.update_layout(
            title=title, margin=dict(l=10, r=10, t=40, b=10),
            uirevision="keep",  # ズーム等のUI状態を更新間で保持
            plot_bgcolor="#bdbdbd",  # NaN(無効)ゾーンは背景のグレーが透ける
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

        # Thermal. Diff panel shows the firmware background-subtraction foreground.
        if s["t_arr"] is not None:
            fig_t = heatmap(s["t_arr"], args.t_vmin, args.t_vmax, "Jet", "MLX90640 [°C]")
            tfg = s["t_diff"]  # foreground (>=0)
            fig_td = heatmap(tfg, 0.0, args.t_diff_range,
                             "Hot", "MLX90640 foreground [Δ°C above bg]")
            t_info = (f"Ta={s['t_ta']:.2f}°C  "
                      f"min={s['t_arr'].min():.2f} max={s['t_arr'].max():.2f}  "
                      f"fg_max={np.nanmax(tfg):.2f}°C")
        else:
            fig_t = fig_td = no_update
            t_info = "thermal: waiting..."

        # Distance (with value overlay). Invalid zones are NaN -> blank cell, grey.
        if s["d_grid"] is not None:
            dg = s["d_grid"]
            dtxt = np.array([["" if np.isnan(v) else f"{v:.0f}" for v in row]
                             for row in dg])
            fig_d = heatmap(dg, args.d_vmin, args.d_vmax, "Jet_r",
                            "VL53L5CX [mm]", text=dtxt)
            # Diff panel = distance foreground (mm closer than background, >=0).
            dfg = s["d_diff"]
            dfgtxt = np.array([["" if (np.isnan(v) or v <= 0) else f"{v:.0f}"
                                for v in row] for row in dfg])
            fig_dd = heatmap(dfg, 0.0, args.d_diff_range,
                             "Hot", "VL53L5CX foreground [mm closer]", text=dfgtxt)
            valid = int(np.isin(s["d_stat"], list(STATUS_VALID)).sum())
            n_valid_grid = int(np.count_nonzero(~np.isnan(dg)))
            if n_valid_grid > 0:
                d_info = (f"valid={valid}/64  "
                          f"min={np.nanmin(dg):.0f} max={np.nanmax(dg):.0f} "
                          f"avg={np.nanmean(dg):.0f}mm  invalid={64 - n_valid_grid}")
            else:
                d_info = f"valid={valid}/64  all zones invalid"
        else:
            fig_d = fig_dd = no_update
            d_info = "distance: waiting..."

        # Detection banner from firmware background-subtraction candidate (DET).
        # サイドパネル向けに縦積み・幅いっぱいのブロック表示にする。
        det = s["det"]
        if det is not None:
            cand, t_max_c, t_area, d_max, d_area = det
            if cand:
                banner = html.Div(
                    [html.Div("CANDIDATE", style={"fontSize": "16px"}),
                     html.Div(f"thermal: {t_max_c/100:.1f}°C / {t_area}px",
                              style={"fontSize": "12px"}),
                     html.Div(f"distance: {d_max}mm / {d_area}z",
                              style={"fontSize": "12px"})],
                    style={"fontWeight": "bold", "color": "#fff",
                           "background": "#c0392b", "padding": "8px 12px",
                           "borderRadius": "4px", "textAlign": "center"})
            else:
                banner = html.Div(
                    [html.Div("no candidate", style={"fontSize": "15px"}),
                     html.Div(f"thermal: {t_max_c/100:.1f}°C / {t_area}px",
                              style={"fontSize": "12px"}),
                     html.Div(f"distance: {d_max}mm / {d_area}z",
                              style={"fontSize": "12px"})],
                    style={"color": "#666", "background": "#eee",
                           "padding": "8px 12px", "borderRadius": "4px",
                           "textAlign": "center"})
        else:
            banner = html.Div("background: initializing...",
                              style={"color": "#999", "background": "#f5f5f5",
                                     "padding": "8px 12px", "borderRadius": "4px",
                                     "textAlign": "center"})

        # Status: サイドパネルに縦積み（thermal の下に distance）、FPS付き。
        status = html.Div(
            style={"display": "flex", "flexDirection": "column", "gap": "12px"},
            children=[
            banner,
            html.Div(
                style={"display": "flex", "flexDirection": "column", "gap": "12px"},
                children=[
                    html.Div(children=[
                        html.Span("Thermal MLX90640",
                                  style={"fontWeight": "bold", "color": "#c0392b"}),
                        html.Span(f" {s['t_fps']:.1f} fps",
                                  style={"fontWeight": "bold", "color": "#c0392b"}),
                        html.Br(),
                        html.Span(f"#{s['t_seq']}  {t_info}",
                                  style={"fontSize": "12px"}),
                    ]),
                    html.Div(children=[
                        html.Span("Distance VL53L5CX",
                                  style={"fontWeight": "bold", "color": "#2471a3"}),
                        html.Span(f" {s['d_fps']:.1f} fps",
                                  style={"fontWeight": "bold", "color": "#2471a3"}),
                        html.Br(),
                        html.Span(f"#{s['d_seq']}  {d_info}",
                                  style={"fontSize": "12px"}),
                    ]),
                ],
            ),
        ])
        return fig_t, fig_d, fig_td, fig_dd, status

    @app.callback(
        Output("reset-msg", "children"),
        Input("btn-bg-reset", "n_clicks"),
        prevent_initial_call=True,
    )
    def on_bg_reset(n_clicks):
        """ ボタン押下で 'R' を送信し、ファームに背景を取り直させる。 """
        try:
            ser.write(b"R")
            ser.flush()
            return f"Background reset sent (#{n_clicks}). Re-establishing (~5-6 s)."
        except Exception as e:  # serial.SerialException 等
            return f"Send failed: {e}"

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

    app = build_app(args, state, ser)
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
