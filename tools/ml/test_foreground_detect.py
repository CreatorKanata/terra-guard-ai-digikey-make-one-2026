#!/usr/bin/env python3
"""
TerraGuard AI — 距離foregroundベースの陸上カラス検出 テスト/検証スクリプト

実機に焼く前に、保存済み検証データ(dataset/validation/*.json)を使って
「距離前景 + サーマル小局所スポット」の総合判定で カラスあり/なし を
分離できるかを検証する。

背景:
  地面に立つカラスは背景(地面)との距離差が 15-20cm 程度しか出ず、単一ゾーンの
  絶対値(最大80mm)では地面ノイズ床(σ p90 41mm, max 126mm)に埋もれて分離不能。
  しかしカラスは「隣接ゾーンがまとまって反応(連結塊)」「前景総和が大きい」
  「サーマルに小さい局所30-35℃スポット」という複合的特徴を持つため、これらを
  組み合わせると背景の散発ノイズから分離できる。これがNPUが4chテンソルから
  学ぶべきパターンそのもの。

判定特徴量(いずれも単一閾値では不可、複合で効く):
  - dist_fg_cluster_size : 距離前景(bg-cur, mm)の最大連結塊サイズ[px]
  - dist_fg_sum          : 距離前景の総和[mm]
  - dist_fg_top3_sum     : 距離前景 上位3ゾーン和[mm]
  - thermal_center_spot  : 中央寄りの小局所(<=6px)30-35℃スポット数

使い方:
  python tools/ml/test_foreground_detect.py \
      --present dataset/validation/2026-06-22_crow_present.json \
      --absent  dataset/validation/2026-06-22_crow_absent.json

SPDX-License-Identifier: BSD-3-Clause
"""
import argparse
import json

# 距離前景の点灯閾値[mm]。地面ノイズσ(p90 41mm)より少し上、かつカラスの
# 弱い反応(15-20cm差の周辺=数十mm)を拾える水準。単独では使わず複合判定に渡す。
DIST_FG_ON_MM = 30

# サーマルのカラス温度帯[℃]。生体カラスの羽毛表面30-35℃(外気が高い日は下限寄り)。
THERMAL_LO_C, THERMAL_HI_C = 30, 35
# 「小さい局所」とみなす最大連結サイズ[px]。これより大きい高温域は背景(壁/日射)。
THERMAL_MAX_SPOT_PX = 6


def dist_foreground(cur, bg):
    """ fg[z] = max(0, bg - cur)（背景より手前に出た量[mm]）。無効/負は0。 """
    out = []
    for z in range(64):
        if cur[z] == -1 or bg[z] <= 0 or bg[z] == -1:
            out.append(0)
        else:
            v = bg[z] - cur[z]
            out.append(v if v > 0 else 0)
    return out


def largest_cluster(fg, on_thr):
    """ 前景が on_thr 以上のゾーンの最大連結塊(4近傍)の (サイズ[px], 塊内fg合計[mm])。 """
    on = set(z for z in range(64) if fg[z] >= on_thr)
    seen = set()
    best_size, best_sum = 0, 0
    for s in on:
        if s in seen:
            continue
        stack, comp = [s], []
        while stack:
            z = stack.pop()
            if z in seen or z not in on:
                continue
            seen.add(z)
            comp.append(z)
            r, c = z // 8, z % 8
            for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nr, nc = r + dr, c + dc
                if 0 <= nr < 8 and 0 <= nc < 8:
                    stack.append(nr * 8 + nc)
        if len(comp) > best_size:
            best_size = len(comp)
            best_sum = sum(fg[z] for z in comp)
    return best_size, best_sum


def thermal_center_spots(grid, lo=THERMAL_LO_C, hi=THERMAL_HI_C,
                         maxsize=THERMAL_MAX_SPOT_PX):
    """ 中央寄りの「小さい局所」30-35℃スポット数。視野端の固定熱源は除外。 """
    H, W = len(grid), len(grid[0])
    hot = set((r, c) for r in range(H) for c in range(W)
              if lo <= grid[r][c] <= hi)
    seen, spots = set(), []
    for cell in hot:
        if cell in seen:
            continue
        stack, comp = [cell], []
        while stack:
            x = stack.pop()
            if x in seen or x not in hot:
                continue
            seen.add(x)
            comp.append(x)
            r, c = x
            for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                stack.append((r + dr, c + dc))
        if len(comp) > maxsize:
            continue  # 大きい高温域=背景。除外
        cr = sum(r for r, c in comp) / len(comp)
        cc = sum(c for r, c in comp) / len(comp)
        # 視野端(行<5/>18, 列<3/>20)の固定熱源を除外し、中央寄りのみ数える。
        if 5 <= cr <= 18 and 3 <= cc <= 20:
            spots.append((len(comp), round(cr), round(cc)))
    return spots


def features(present_grid, bg_grid, thermal_grid):
    """ 距離+サーマルから判定特徴量を算出。 """
    fg = dist_foreground(present_grid, bg_grid)
    cl_size, cl_sum = largest_cluster(fg, DIST_FG_ON_MM)
    spots = thermal_center_spots(thermal_grid)
    return {
        "dist_fg_cluster_size": cl_size,
        "dist_fg_cluster_sum": cl_sum,
        "dist_fg_sum": sum(fg),
        "dist_fg_top3_sum": sum(sorted(fg, reverse=True)[:3]),
        "thermal_center_spots": len(spots),
        "thermal_spot_detail": spots,
    }


def decide(feat):
    """ 複合判定: 距離前景の塊 or 総和 が出ていて、かつサーマル中央スポットがあれば
        カラスあり。距離・サーマルどちらかが弱くても、もう一方が強ければ拾う(OR寄り)。
        実機/NPU では学習に置き換わるが、ここはヒューリスティック検証用。 """
    dist_strong = (feat["dist_fg_cluster_size"] >= 2 and feat["dist_fg_cluster_sum"] >= 100) \
        or feat["dist_fg_top3_sum"] >= 120
    thermal_strong = feat["thermal_center_spots"] >= 1
    # 距離とサーマルの両方が弱いときのみ「なし」。片方でも強ければ候補。
    return dist_strong or thermal_strong, dist_strong, thermal_strong


def main():
    ap = argparse.ArgumentParser(description="距離foregroundベース カラス検出テスト")
    ap.add_argument("--present", required=True, help="カラスあり 検証JSON")
    ap.add_argument("--absent", required=True, help="カラスなし(背景) 検証JSON")
    args = ap.parse_args()

    A = json.load(open(args.present))
    B = json.load(open(args.absent))
    bg = B["dist_8x8_mm"]["grid"]  # カラス不在=背景モデル相当

    print("=" * 60)
    print("距離foregroundベース カラス検出テスト")
    print("=" * 60)

    cases = [
        ("カラスあり", A["dist_8x8_mm"]["grid"], A["thermal_24x24_C"]["grid"], True),
        ("カラスなし", bg, B["thermal_24x24_C"]["grid"], False),
    ]
    ok = True
    for name, dist, thermal, expect in cases:
        feat = features(dist, bg, thermal)
        verdict, ds, ts = decide(feat)
        mark = "✅" if verdict == expect else "❌"
        if verdict != expect:
            ok = False
        print(f"\n--- {name} (期待={'crow' if expect else 'none'}) {mark} ---")
        print(f"  距離前景: 最大塊={feat['dist_fg_cluster_size']}px "
              f"塊和={feat['dist_fg_cluster_sum']}mm "
              f"総和={feat['dist_fg_sum']}mm 上位3和={feat['dist_fg_top3_sum']}mm "
              f"→ {'強' if ds else '弱'}")
        print(f"  サーマル中央スポット: {feat['thermal_center_spots']}個 "
              f"{feat['thermal_spot_detail']} → {'強' if ts else '弱'}")
        print(f"  判定: {'crow' if verdict else 'none'}")

    print("\n" + "=" * 60)
    print("結果:", "全ケース正しく分離 ✅" if ok else "分離失敗 ❌（特徴量/閾値の見直しが必要）")
    print("=" * 60)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
