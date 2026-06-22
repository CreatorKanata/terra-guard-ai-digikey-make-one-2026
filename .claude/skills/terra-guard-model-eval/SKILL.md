---
name: terra-guard-model-eval
description: TerraGuard カラス検出NPUモデルの性能検証・誤検出の切り分けを行うときに使う。ホールドアウト評価(混同行列/FN/FP)、実機ライブでの誤爆確認、tflite vs ファームNPU の照合による原因切り分け、再学習〜再デプロイの判定基準を集約。「モデルの性能を確認」「誤検出を切り分け」「ホールドアウト評価」「再学習すべきか」等で起動。
---

# TerraGuard モデル性能検証スキル

カラス検出 **2クラス（not_crow / crow, 入力 24×24×4）** の int8/NPU モデルの
性能を測り、誤検出（背景なのに crow / カラスなのに not_crow）の原因を切り分ける。
**ビルド・書き込み・シリアル・学習は必ず Claude が CLI で実行する**（CLAUDE.md）。
実機操作を伴うので、起動前に `frdm-mcxn947-dev` スキルも併せて参照すること。

## 鉄則（今回の失敗から確定）

- **ホールドアウト評価「だけ」を合格基準にしない。** ランダム分割のホールドアウトは
  学習データと同質なので**過大評価**になる（実例: 40/40 全問正解でも実機背景で
  confirmed crow=42/90 と誤爆した）。**最終合格は「実機ライブ背景で confirmed
  crow≈0、p_crow の中央値が十分低い（目安 <0.2）」まで確認して初めて判定する。**
- 「カラス見逃し(FN)ゼロ」が最優先。背景誤爆(FP)はデバウンスで一部救えるが、
  恒常的に raw crow が多い（例 >70%）状態はデバウンス頼みにせず再学習で潰す。

## 検証フロー（確定手順）

### 1. ホールドアウト評価（ホスト・arm64）

`split_holdout.py` で分離 → `train_model.py` で学習 → `evaluate_model.py` で評価。

```bash
# 各クラス N 枚をホールドアウト分離（X.npy を学習用に上書き、X_holdout を生成）
tools/.venv/bin/python tools/ml/split_holdout.py \
    --data dataset/built --n-holdout 40 --seed 0

# 学習＋int8量子化（⚠️ arm64 必須。出力 tools/ml/build/terra_guard_int8.tflite）
arch -arm64 tools/ml/.venv/bin/python tools/ml/train_model.py

# ホールドアウトで混同行列/precision/recall/F1/FN/FP
arch -arm64 tools/ml/.venv/bin/python tools/ml/evaluate_model.py \
    --tflite tools/ml/build/terra_guard_int8.tflite --data dataset/built
```

- `split_holdout.py` は `X_all.npy/y_all.npy`（全データ控え）も書く。再分割や
  全データ再学習に使える。`X.npy/y.npy` は**学習用のみに上書きされる**点に注意。
- FN（カラス見逃し）が最重要指標。`evaluate_model.py` が誤分類サンプルの
  idx と p_crow を出すので、境界事例の傾向を見る。

### 2. 実機ライブでの誤爆確認（合格判定の本丸）

書き込み後、**背景のまま**（カラス被写体なし）でシリアルの INFER 行を観測する。
INFER フォーマット: `INFER,<確定crow 0/1>,<p_crow×1000>,<conf×1000>,<raw crow>,<streak>`。

```bash
PY=/Users/hide/.mcuxpressotools/.mcux-venv-3.12/bin/python
"$PY" - <<'EOF'
import serial, time, re, glob, statistics
port = glob.glob('/dev/cu.usbmodem*')[0]
ser = serial.Serial(port, 921600, timeout=0.1); ser.reset_input_buffer()
buf=bytearray(); t0=time.time()
while time.time()-t0 < 15.0:
    d=ser.read(16384)
    if d: buf+=d
ser.close()
inf=re.findall(r'INFER,(-?\d+),(-?\d+),-?\d+,(-?\d+),(-?\d+)', buf.decode('latin-1','replace'))
pc=[int(x[1]) for x in inf]; conf=[x[0] for x in inf]; raw=[x[2] for x in inf]
print(f"frames={len(inf)} p_crow med={statistics.median(pc)} mean={int(statistics.mean(pc))}")
print(f"confirmed crow=1: {conf.count('1')}/{len(conf)}   raw crow=1: {raw.count('1')}/{len(raw)}")
EOF
```

**合格**: `confirmed crow=1` が 0（または極小）かつ `p_crow median < 200`(×1000=0.2)。
**不合格**: confirmed が出続ける / raw crow が多数（>70%）→ 背景を追加収集して再学習。

### 3. 誤検出の原因切り分け（tflite vs ファームNPU）

「学習データ(分布)の問題」か「ファームNPU実行経路(変換/詰め方/ドライバ)の問題」かを
**同一ライブ入力**で切り分ける。

```bash
# 実機ライブ入力を build_trainset と同じ前処理でテンソル化→host tflite 推論し、
# 同時に INFER 行(ファームNPU結果)も拾って並べる
arch -arm64 tools/ml/.venv/bin/python tools/ml/diag_live_vs_tflite.py 12
```

- **host tflite と firmware NPU が同傾向**（ともに高い/低い）→ **学習データ(分布)の問題**。
  背景や被写体を追加収集して再学習する（下記「再学習〜再デプロイ」）。
- **両者が乖離**（tflite は妥当なのに NPU だけ偏る）→ **ファームNPU実行経路の問題**。
  量子化 scale/zp 不一致・テンソル詰め順・変換バージョン(3.0.0)・microcode を疑う。

実例(2026-06-22): 背景で host tflite p_crow≈0.42・firmware NPU≈0.65。前処理/量子化
(scale=1/255・zp=-128, 出力 1/256・-128)はファームと学習で一致を確認済み→「学習データ
の背景不足」と判断。背景 +505枚で再学習し、tflite 0.28 / NPU 0.09 に低下、confirmed
crow=0/90 で誤爆解消。

## 環境のハマりどころ（確定）

- **venv が2系統に分断**:
  - `tools/.venv` = x86_64。pyserial/numpy はあるが **tensorflow は動かない**。
    収集系・ビルドツール(build_trainset/split_holdout)用。
  - `tools/ml/.venv` = **arm64**。tensorflow/tflite が動く。train/evaluate/diag 用。
    pyserial は標準では入っていないので、シリアルを使う diag には
    `arch -arm64 tools/ml/.venv/bin/pip install pyserial` を一度実行（pure-python で安全）。
- **量子化パラメータはファームのハードコードと一致必須**。再学習で変わったら
  `app/npu_infer.c` の `IN_SCALE/IN_ZP`(入力) と `outScale/outZp`(出力) を見直す。
  現行モデルは IN=1/255・-128、OUT=1/256・-128 で一致。tflite の実値は次で確認:
  ```bash
  arch -arm64 tools/ml/.venv/bin/python -c \
   "import tensorflow as tf;i=tf.lite.Interpreter('tools/ml/build/terra_guard_int8.tflite');i.allocate_tensors();\
    print('IN',i.get_input_details()[0]['quantization']);print('OUT',i.get_output_details()[0]['quantization'])"
  ```
- **NPU版 tflite(NeutronGraph)はホストで実行不可**。`diag_live_vs_tflite.py` が回すのは
  通常 int8 tflite。NPU版の数値は実機 INFER 行からしか得られない。

## 再学習〜再デプロイ（合格しなかったとき）

詳細手順は `frdm-mcxn947-dev` スキルの「ML / NPU ワークフロー」に集約。要点のみ:

1. 不足データを収集（背景なら not_crow ラベル）。収集は**ユーザーがビューア
   `tools/dual_viewer_web.py` で実施**（私=Claude はキー打鍵不可）。
2. `build_trainset.py` → `split_holdout.py` → `train_model.py`(arm64) → `evaluate_model.py`
3. NPU変換は **3.0.0 厳守**:
   `NEUTRON_SDK_DIR=$PWD/sdk/eiq-neutron-sdk-linux-3.0.0 tools/ml/neutron_convert.sh ...`
   （microcode が `0x6d41ac8b` で一致することを確認）
4. `make_model_data_h.py --name terra_guard_crow --arena 65536` →
   `src/FRDM-MCXN947/terra-guard-ai/tflm/pcq_npu/model_data.h` に差替 → ビルド → 書込
5. **必ず手順2の「実機ライブ誤爆確認」まで戻って合格判定する**

## 関連

- ツール: `tools/ml/{split_holdout,evaluate_model,diag_live_vs_tflite}.py`
- スキル: `frdm-mcxn947-dev`（ビルド/書込/シリアル/NPU変換の確定手順）
- メモ: `terra-guard-data-collection` / `npu-converter-version-mismatch` /
  `npu-cifar10-verified` / `crow-thermal-signature` / `terra-guard-thermal-24x24`
- ドキュメント: `docs/ml-model.md`
