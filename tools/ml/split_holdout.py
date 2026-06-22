#!/usr/bin/env python3
"""
TerraGuard AI — 学習テンソルを「学習用」と「ホールドアウト検証用」に分割する。

build_trainset.py が作った dataset/built/{X,y}.npy（[N,24,24,4] / ラベル0/1）から、
各クラス N_HOLDOUT 枚ずつをランダム（固定シードで再現可能）に抜き出して検証用とし、
残りを学習用とする。学習(train_model.py)は X.npy/y.npy 固定名を読むので、

  - X.npy / y.npy          ← 学習用（ホールドアウトを除いた残り）に上書き
  - X_holdout.npy / y_holdout.npy ← 検証用（各クラス N_HOLDOUT 枚）
  - X_all.npy / y_all.npy  ← 元の全データのバックアップ（再分割や検証に使える）

を書き出す。これにより train_model.py は無改修で「学習用のみ」を使い、
evaluate_model.py が X_holdout を未学習データとして評価できる。

使い方:
    tools/.venv/bin/python tools/ml/split_holdout.py \
        --data dataset/built --n-holdout 20 --seed 0

SPDX-License-Identifier: BSD-3-Clause
"""
import argparse
import os

import numpy as np

CLASSES = {0: "not_crow", 1: "crow"}


def main():
    ap = argparse.ArgumentParser(description="学習/ホールドアウト検証 分割")
    ap.add_argument("--data", default="dataset/built", help="X.npy/y.npy のディレクトリ")
    ap.add_argument("--n-holdout", type=int, default=20,
                    help="各クラスから検証用に抜く枚数")
    ap.add_argument("--seed", type=int, default=0, help="再現用シード")
    args = ap.parse_args()

    x_path = os.path.join(args.data, "X.npy")
    y_path = os.path.join(args.data, "y.npy")
    X = np.load(x_path)
    y = np.load(y_path)
    print(f"全データ: X={X.shape} y={y.shape}  "
          f"crow={int((y == 1).sum())} not_crow={int((y == 0).sum())}")

    # 元データを X_all/y_all として必ずバックアップ（X.npy は学習用に上書きするため）。
    np.save(os.path.join(args.data, "X_all.npy"), X)
    np.save(os.path.join(args.data, "y_all.npy"), y)

    rng = np.random.default_rng(args.seed)
    hold_idx = []
    for cls in CLASSES:
        cls_idx = np.where(y == cls)[0]
        if len(cls_idx) < args.n_holdout:
            raise SystemExit(
                f"クラス{cls}({CLASSES[cls]}) は {len(cls_idx)} 枚しかなく "
                f"検証用 {args.n_holdout} 枚を確保できません。")
        picked = rng.choice(cls_idx, size=args.n_holdout, replace=False)
        hold_idx.append(picked)
    hold_idx = np.concatenate(hold_idx)
    hold_mask = np.zeros(len(y), dtype=bool)
    hold_mask[hold_idx] = True

    X_hold, y_hold = X[hold_mask], y[hold_mask]
    X_train, y_train = X[~hold_mask], y[~hold_mask]

    np.save(os.path.join(args.data, "X_holdout.npy"), X_hold)
    np.save(os.path.join(args.data, "y_holdout.npy"), y_hold)
    # train_model.py が読む X.npy/y.npy を「学習用のみ」に上書き。
    np.save(x_path, X_train)
    np.save(y_path, y_train)

    print(f"検証用(holdout): X={X_hold.shape}  "
          f"crow={int((y_hold == 1).sum())} not_crow={int((y_hold == 0).sum())}")
    print(f"学習用(train):   X={X_train.shape}  "
          f"crow={int((y_train == 1).sum())} not_crow={int((y_train == 0).sum())}")
    print(f"→ {args.data}/X_holdout.npy, y_holdout.npy（検証用・未学習）")
    print(f"→ {args.data}/X.npy, y.npy（学習用に上書き）/ X_all.npy, y_all.npy（全データ控え）")
    print(f"  seed={args.seed} n_holdout={args.n_holdout}（再現可能）")


if __name__ == "__main__":
    main()
