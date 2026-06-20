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
    # --- データ収集（ビューア内蔵。collect_dataset.py と同形式の .npz を保存） ---
    p.add_argument("--collect-out", default="dataset/raw",
                   help="収集サンプル(.npz)の保存先")
    p.add_argument("--collect-fps", type=float, default=8.0,
                   help="収集レートの上限[fps]（近接フレームの重複を間引く）")
    return p.parse_args()


# ---------------------------------------------------------------------------
# 共有状態: 受信スレッドが書き、Dashコールバックが読む。最新フレームのみ保持。
# ---------------------------------------------------------------------------
class SharedState:
    def __init__(self):
        self._lock = threading.Lock()
        self.t_arr = None      # 最新サーマル(24×24, ℃) 表示用クロップ済み
        self.t_diff = None     # サーマル差分(24×24)
        self.t_ta = None       # 周囲温度
        self.d_grid = None     # 最新距離(8×8, mm)
        self.d_diff = None     # 距離差分(8×8)
        self.d_stat = None     # 距離 target_status(8×8)
        self.t_seq = 0         # フレーム連番（新フレーム判定・遅延監視用）
        self.d_seq = 0
        # 背景差分の候補判定(DET行)。(candidate, t_max_c, t_area, d_max, d_area)。
        self.det = None
        # NPU推論結果(INFER行)。(crow 0/1, p_crow 0..1, confidence 0..1)。
        self.infer = None
        # 実FPS計測用: 各センサのフレーム到着時刻を移動ウィンドウで保持。
        self._fps_window = 2.0           # FPS算出の移動ウィンドウ[秒]
        self.t_times = deque()           # サーマルフレーム到着時刻
        self.d_times = deque()           # 距離フレーム到着時刻
        # --- データ収集用: 生(クロップ前 32×24/8×8)を保持。collect_dataset.py と同形式。 ---
        self.t_raw = None      # 生サーマル[24,32] ℃（表示用クロップ前）
        self.t_fg_raw = None   # サーマル前景[24,32] ℃
        self.d_fg_raw = None   # 距離前景[8,8] mm
        # 収集制御（Dashコールバックが書き、受信スレッドが読む）。
        self.rec_on = False              # 収集中か
        self.rec_label = "crow"          # 現在のラベル
        self.rec_saved = {"crow": 0, "not_crow": 0}  # 保存枚数
        self._collect_idx = 0            # 保存連番（main で既存の続きに初期化）

    @staticmethod
    def _fps(times, now, window):
        """ 到着時刻 deque から直近 window 秒の FPS を算出（古い時刻は捨てる）。 """
        while times and now - times[0] > window:
            times.popleft()
        if len(times) < 2:
            return 0.0
        span = times[-1] - times[0]
        return (len(times) - 1) / span if span > 0 else 0.0

    def update_thermal(self, arr, diff, ta, raw=None, fg_raw=None):
        with self._lock:
            self.t_arr, self.t_diff, self.t_ta = arr, diff, ta
            if raw is not None:
                self.t_raw = raw
            if fg_raw is not None:
                self.t_fg_raw = fg_raw
            self.t_seq += 1
            self.t_times.append(time.monotonic())

    def update_distance(self, grid, diff, stat, fg_raw=None):
        with self._lock:
            self.d_grid, self.d_diff, self.d_stat = grid, diff, stat
            if fg_raw is not None:
                self.d_fg_raw = fg_raw
            self.d_seq += 1
            self.d_times.append(time.monotonic())

    def update_det(self, det):
        with self._lock:
            self.det = det

    def update_infer(self, infer):
        with self._lock:
            self.infer = infer

    # --- 収集制御 ---
    def set_recording(self, on, label=None):
        with self._lock:
            self.rec_on = on
            if label is not None:
                self.rec_label = label

    def rec_status(self):
        with self._lock:
            return self.rec_on, self.rec_label, dict(self.rec_saved)

    def collect_snapshot(self):
        """ 収集用: 生データ一式と収集設定を返す。背景未確立等で欠けたら None を含む。 """
        with self._lock:
            return {
                "on": self.rec_on, "label": self.rec_label,
                "t_raw": self.t_raw, "t_fg_raw": self.t_fg_raw,
                "d_grid": self.d_grid, "d_fg_raw": self.d_fg_raw,
                "det": self.det,
            }

    def inc_saved(self, label):
        with self._lock:
            self.rec_saved[label] = self.rec_saved.get(label, 0) + 1

    def snapshot(self):
        with self._lock:
            now = time.monotonic()
            return {
                "t_arr": self.t_arr, "t_diff": self.t_diff, "t_ta": self.t_ta,
                "d_grid": self.d_grid, "d_diff": self.d_diff, "d_stat": self.d_stat,
                "t_seq": self.t_seq, "d_seq": self.d_seq, "det": self.det,
                "infer": self.infer,
                "t_fps": self._fps(self.t_times, now, self._fps_window),
                "d_fps": self._fps(self.d_times, now, self._fps_window),
                "rec_on": self.rec_on, "rec_label": self.rec_label,
                "rec_saved": dict(self.rec_saved),
            }


def next_sample_index(out_dir):
    """ 既存 .npz の最大連番+1 を返す（追記収集できるように）。 """
    import os
    if not os.path.isdir(out_dir):
        return 0
    idx = 0
    for f in os.listdir(out_dir):
        if f.endswith(".npz"):
            try:
                idx = max(idx, int(f.split("_")[-1].split(".")[0]) + 1)
            except (ValueError, IndexError):
                pass
    return idx


def save_collect_sample(args, state):
    """ 現在の最新フレーム一式を 1 サンプル .npz に保存する。
        形式は tools/ml/collect_dataset.py と同一（build_trainset.py がそのまま読める）。
        背景未確立で前景が None の場合はゼロ配列で埋める（生サーマルは必須）。 """
    import os
    snap = state.collect_snapshot()
    t_raw = snap["t_raw"]
    if t_raw is None:
        return  # 生サーマル未受信なら保存しない
    label = snap["label"]
    t_fg = snap["t_fg_raw"] if snap["t_fg_raw"] is not None else np.zeros_like(t_raw)
    d_grid = (snap["d_grid"] if snap["d_grid"] is not None
              else np.full((D_GRID, D_GRID), np.nan, np.float32))
    d_fg = (snap["d_fg_raw"] if snap["d_fg_raw"] is not None
            else np.zeros((D_GRID, D_GRID), np.float32))
    det = (np.asarray(snap["det"], np.int32) if snap["det"] is not None
           else np.full(5, -1, np.int32))

    os.makedirs(args.collect_out, exist_ok=True)
    idx = state._collect_idx
    path = os.path.join(args.collect_out, f"sample_{idx:06d}.npz")
    np.savez_compressed(
        path,
        thermal=t_raw.astype(np.float32),
        thermal_fg=t_fg.astype(np.float32),
        distance=d_grid.astype(np.float32),
        distance_fg=d_fg.astype(np.float32),
        det=det,
        label=label,
        ts=time.time(),
    )
    state._collect_idx += 1
    state.inc_saved(label)


def reader_loop(ser, args, state, stop_evt):
    """ シリアルを常時読み、最新フレームだけを state に上書きし続ける。
        古いフレームは捨てるので、受信＞描画でも遅延が蓄積しない。 """
    buf = bytearray()
    dist = None       # 直近の DIST（STAT とペアになるまで保持）
    t_fg = None       # 直近サーマル前景(24×24, ℃) — ファーム背景差分の結果（表示用クロップ済み）
    t_fg_raw = None   # 直近サーマル前景[24,32]（収集用・クロップ前）
    d_fg = None       # 直近距離前景(8×8, mm) — DFG行で更新（DIST受信では消さない）
    last_save = 0.0   # 収集の間引き用（直近保存時刻）
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
                fg_full = np.asarray(pix, dtype=float).reshape(T_ROWS, T_SRC_COLS)
                fg_full = fg_full[::-1, :]                  # 取り付け向き補正
                t_fg_raw = fg_full                          # 収集用[24,32]
                t_fg = fg_full[:, T_CROP_X0:T_CROP_X0 + T_CROP_COLS]  # 表示用クロップ
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
                continue
            if line.startswith("INFER,"):
                # INFER,<確定crow 0/1>,<p_crow x1000>,<conf x1000>[,<raw crow>,<streak>]
                # crow は時間方向デバウンス後の「確定」判定。raw/streak は任意（デバッグ用）。
                parts = line.strip().split(",")
                if len(parts) >= 4:
                    try:
                        crow = int(parts[1])            # 確定crow（デバウンス済み）
                        p_crow = int(parts[2]) / 1000.0
                        conf = int(parts[3]) / 1000.0
                        raw = int(parts[4]) if len(parts) >= 5 else crow
                        streak = int(parts[5]) if len(parts) >= 6 else 0
                        state.update_infer((crow, p_crow, conf, raw, streak))
                    except ValueError:
                        pass

        # --- サーマル: 生フレームを整形。diffパネルにはファーム背景差分(前景)を表示 ---
        if latest_t is not None:
            ta, pix = latest_t
            pix = flip_grid(pix, T_ROWS, T_SRC_COLS, args.flip_h, args.flip_v)
            arr_full = np.asarray(pix, dtype=float).reshape(T_ROWS, T_SRC_COLS)
            arr_full = arr_full[::-1, :]                      # 取り付け向き補正[24,32]
            arr = arr_full[:, T_CROP_X0:T_CROP_X0 + T_CROP_COLS]  # 表示用クロップ(24列)
            # 前景未受信(背景確立前)はゼロマップ。
            fg = t_fg if t_fg is not None else np.zeros_like(arr)
            state.update_thermal(arr, fg, ta, raw=arr_full, fg_raw=t_fg_raw)

            # --- データ収集: サーマル新フレーム到着＝1サンプル。収集ONなら保存 ---
            now = time.monotonic()
            if (state.collect_snapshot()["on"]
                    and now - last_save >= 1.0 / args.collect_fps):
                last_save = now
                save_collect_sample(args, state)

        # --- 距離: 生フレームを整形。diffパネルにはファーム背景差分(前景)を表示 ---
        if latest_d is not None:
            dd, ss = latest_d
            dd = flip_grid(dd, D_GRID, D_GRID, args.flip_h, args.flip_v)
            ss = flip_grid(ss, D_GRID, D_GRID, args.flip_h, args.flip_v)
            grid = np.asarray(dd, dtype=float).reshape(D_GRID, D_GRID)
            stat = np.asarray(ss, dtype=int).reshape(D_GRID, D_GRID)
            # 距離前景の生(NaN化前)を収集用に保持。
            d_fg_raw_save = d_fg.copy() if d_fg is not None else np.zeros((D_GRID, D_GRID))
            # ファームが無効ゾーンに出す -1 を NaN にしてグレー表示にする。
            grid[grid < 0] = np.nan
            # 距離前景(背景差分): 永続保持の d_fg を使う。未受信時のみゼロマップ。
            # （DFG が DIST と別の読み取りバッチに割れても前景を消さない＝全消えバグ修正）
            dfg_arr = d_fg if d_fg is not None else np.zeros_like(grid)
            state.update_distance(grid, dfg_arr, stat, fg_raw=d_fg_raw_save)


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
                            # --- データ収集: ラベル選択 + Start/Stop。collect_dataset.py
                            #     と同形式の .npz を保存し build_trainset.py で学習に回す。 ---
                            html.Div(
                                style={"border": "1px solid #ddd",
                                       "borderRadius": "4px", "padding": "10px",
                                       "display": "flex", "flexDirection": "column",
                                       "gap": "8px"},
                                children=[
                                    html.Div("Data collection",
                                             style={"fontWeight": "bold"}),
                                    dcc.RadioItems(
                                        id="collect-label",
                                        options=[{"label": " crow", "value": "crow"},
                                                 {"label": " not_crow",
                                                  "value": "not_crow"}],
                                        value="crow",
                                        inline=True,
                                        labelStyle={"marginRight": "12px"}),
                                    html.Div(
                                        style={"display": "flex",
                                               "flexDirection": "row", "gap": "8px"},
                                        children=[
                                            html.Button(
                                                "Start", id="btn-collect-start",
                                                n_clicks=0,
                                                style={"fontSize": "14px",
                                                       "padding": "6px 16px",
                                                       "background": "#27ae60",
                                                       "color": "#fff",
                                                       "border": "none",
                                                       "borderRadius": "4px",
                                                       "cursor": "pointer"}),
                                            html.Button(
                                                "Stop", id="btn-collect-stop",
                                                n_clicks=0,
                                                style={"fontSize": "14px",
                                                       "padding": "6px 16px",
                                                       "background": "#7f8c8d",
                                                       "color": "#fff",
                                                       "border": "none",
                                                       "borderRadius": "4px",
                                                       "cursor": "pointer"}),
                                        ]),
                                    html.Div(id="collect-status",
                                             style={"fontSize": "12px",
                                                    "color": "#444"}),
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
        Output("collect-status", "children"),
        Input("tick", "n_intervals"),
    )
    def refresh(_n):
        from dash import no_update
        s = state.snapshot()
        # 収集状態（Start/Stop と保存枚数）を毎 tick 表示。
        rec = ("● recording" if s["rec_on"] else "○ stopped")
        collect_status = (
            f"{rec}  label={s['rec_label']}  "
            f"crow={s['rec_saved'].get('crow', 0)}  "
            f"not_crow={s['rec_saved'].get('not_crow', 0)}  "
            f"→ {args.collect_out}")

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

        # NPU inference banner (INFER line). confirmed crow=red(purple), else grey.
        # crow は時間方向デバウンス後の確定判定。raw/streak はサブ情報として小さく出す。
        inf = s["infer"]
        if inf is not None:
            crow, p_crow, conf = inf[0], inf[1], inf[2]
            raw = inf[3] if len(inf) > 3 else crow
            streak = inf[4] if len(inf) > 4 else 0
            sub = (f"p(crow)={p_crow*100:.0f}%  conf={conf*100:.0f}%  "
                   f"raw={raw} streak={streak}")
            if crow:
                infer_banner = html.Div(
                    [html.Div("🐦 CROW DETECTED", style={"fontSize": "16px"}),
                     html.Div(sub, style={"fontSize": "11px"})],
                    style={"fontWeight": "bold", "color": "#fff",
                           "background": "#8e44ad", "padding": "8px 12px",
                           "borderRadius": "4px", "textAlign": "center"})
            else:
                infer_banner = html.Div(
                    [html.Div("NPU: not crow", style={"fontSize": "15px"}),
                     html.Div(sub, style={"fontSize": "11px"})],
                    style={"color": "#555", "background": "#e8e8e8",
                           "padding": "8px 12px", "borderRadius": "4px",
                           "textAlign": "center"})
        else:
            infer_banner = html.Div("NPU: waiting...",
                                    style={"color": "#999", "background": "#f5f5f5",
                                           "padding": "8px 12px",
                                           "borderRadius": "4px",
                                           "textAlign": "center"})

        # Status: サイドパネルに縦積み。NPU判定を最上段、次にFPS。
        # （背景差分の candidate バナーは紛らわしいので非表示にした。）
        status = html.Div(
            style={"display": "flex", "flexDirection": "column", "gap": "12px"},
            children=[
            infer_banner,
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
        return fig_t, fig_d, fig_td, fig_dd, status, collect_status

    # 収集 Start/Stop: ラベルを反映しつつ収集フラグを切り替える。
    @app.callback(
        Output("btn-collect-start", "n_clicks"),
        Input("btn-collect-start", "n_clicks"),
        Input("btn-collect-stop", "n_clicks"),
        State("collect-label", "value"),
        prevent_initial_call=True,
    )
    def on_collect_toggle(_start, _stop, label):
        from dash import ctx, no_update
        trig = ctx.triggered_id
        if trig == "btn-collect-start":
            state.set_recording(True, label=label)
        elif trig == "btn-collect-stop":
            state.set_recording(False)
        return no_update

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
    # 収集サンプルの連番を既存ファイルの続きから採番（追記収集できるように）。
    state._collect_idx = next_sample_index(args.collect_out)
    if state._collect_idx:
        print(f"収集: {args.collect_out} に既存 → 連番 {state._collect_idx:06d} から")
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
