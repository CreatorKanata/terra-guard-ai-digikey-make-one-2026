# 申し送り — 自作NPUモデル差し替え ✅ 疎通成功（converter版不一致 解決済み）

最終更新: 2026-06-20 / branch `feat/eiq-npu-model` / GitHub Issue #2

## 🎉 結論（2026-06-20）

**自作モデルが FRDM-MCXN947 の eIQ Neutron NPU で推論ループを回すことを実機確認。疎通レベルのゴール達成。**

converter 版不一致は、**SDK同梱 3.0.0（`sdk/eiq-neutron-sdk-linux-3.0.0`, microcode `0X6d41ac8b`）で再変換**し、
さらに **op resolver を 3op 化（Softmax + Slice + NEUTRON_GRAPH）** することで完全に解消した
（3.0.0版の生.hは 3.1.3版と違い **Slice が追加される**点が落とし穴）。

実機ログ（115200, 受信先行→flash）:
```
Model: terra_guard_npu
Model Size: 0x1060 (4192 B)
TensorArena ... Used 0x23f4 (9204 B)
     Inference time: 221 us        ← NPU推論が回った（Invoke成功・Microcode mismatch消滅）
     Detected: No label detected (0%)   ← 未学習ランダム重みなので想定通り
```

本番の学習（none/crow/human の実データ学習）は後続フェーズ。

## ゴール

検証済みの tflm_cifar10(NPU版) パイプラインに、自作3クラスモデル（none/crow/human）の
`model_data.h` を差し替えて、自作モデルが FRDM-MCXN947 の eIQ Neutron NPU で
**推論ループを回す（疎通レベル）** ことを実機確認する。
※現状の自作モデルは「未学習のランダム重み tiny CNN」。形だけ本番相当（入力 [1,24,32,1]、出力 [1,3]）。
本番の学習は後続フェーズ。

## ✅ 完了していること

1. **前提**: tflm_cifar10(NPU版) は実機正常動作確認済み（`ship 99%` / 推論7.1ms）。
   「無音」の真因は受信タイミング取りこぼし。**正しい受信手順 = 受信を先に開始(115200) → その後
   `LinkServer flash load` でクリーン起動**（詳細 `docs/nxp/HANDOFF-npu-cifar10-debug.md`）。

2. **自作モデル生成〜変換〜差し替え機構を構築済み**:
   - `tools/ml/make_test_model.py` → int8 TFLite（`arch -arm64 tools/ml/.venv/bin/python` で実行。**arch -arm64 必須**）。
     出力: `tools/ml/build/terra_guard_int8.tflite`（5552B, 入出力int8, [1,24,32,1]→[1,3]）。
   - `tools/ml/neutron_convert.sh` で NPU 変換（変換率9/10）。
   - **新規ツール `tools/ml/make_model_data_h.py`**: neutron-converter の生 `.h` を SDK 枠
     （`__PLACEMENT`=`.model`(flash配置) / `kTensorArenaSize` / `MODEL_INPUT_MEAN/STD` / `MODEL_NAME`）
     に流し込んで、SDK にそのまま置ける `model_data.h` を合成する。
   - SDK の cifar10 pcq_npu を差し替え済み（**バックアップあり**: `pcq_npu.orig`）:
     - `.../tflm_cifar10/pcq_npu/model_data.h` ← 自作モデル（MODEL_NAME="terra_guard_npu", arena=64KB, 4192B/v300）
     - `.../tflm_cifar10/pcq_npu/model_cifarnet_ops_npu.cpp` ← op resolver **3op**(Softmax + Slice + NEUTRON_GRAPH)
       （op構成は model_data.h 冒頭スニペットに合わせる。3.0.0版では Slice が入る）

3. **ビルド・実機・推論まで完了**:
   west build（`--config flash_debug`）成功 → 自作モデル(4192B)が flash1(`.model`)に配置。
   実機で `Model: terra_guard_npu` ロード → `MODEL_Init`/`AllocateTensors` 成功 → **Invoke成功(221us)**。

## ✅ 解決した問題（converter版不一致）

かつて自作モデルの **Invoke(推論実行)** が失敗していた:
```
Microcode version mismatch! ... model converted with 3.5371...-0X59ef
Internal Neutron NPU driver error 109 in model run!  →  Node NeutronGraph failed status 1  →  Invoke failed!
```
**真因**: neutron-converter のバージョン不一致。
- SDK内蔵 mcxn ドライバ = **driver 3.0.0**（元cifar10は **converter 3.0.0+0X6d41ac8b** で変換）。
- 当初使った converter = **3.1.3+0X1e788118**（`sdk/eiq-neutron-sdk-linux-3.1.3`）→ 不一致。

**解決手順（実施済み）**:

1. 3.0.0 で再変換（`sdk/eiq-neutron-sdk-linux-3.0.0`, version `3.0.0+0X6d41ac8b` 確認済み）:
   ```bash
   NEUTRON_SDK_DIR=$PWD/sdk/eiq-neutron-sdk-linux-3.0.0 \
     ./tools/ml/neutron_convert.sh \
       tools/ml/build/terra_guard_int8.tflite \
       tools/ml/build/terra_guard_int8_mcxn94x_v300.tflite mcxn94x
   # 生成 .h の microcode が 0X6d41ac8b（=SDKドライバ3.0.0）になる
   ```
2. **⚠️ op resolver を 3op に修正（重要な落とし穴）**: 3.0.0版の生.h冒頭スニペットは
   `Softmax + Slice + NEUTRON_GRAPH` の **3op**（3.1.3版は2op）。`model_cifarnet_ops_npu.cpp` を
   `MicroMutableOpResolver<3>` + `AddSlice()` に修正。
3. SDK枠に流し込み → 配置:
   ```bash
   arch -arm64 tools/ml/.venv/bin/python tools/ml/make_model_data_h.py \
     --src-h tools/ml/build/terra_guard_int8_mcxn94x_v300.h \
     --out tools/ml/build/model_data.h --name terra_guard_npu --arena 65536
   cp tools/ml/build/model_data.h \
     ~/mcuxpresso/mcuxsdk/mcuxsdk/examples/_boards/frdmmcxn947/eiq_examples/tflm_cifar10/pcq_npu/model_data.h
   ```
4. ビルド → 受信先行 → flash → ログ確認:
   ```bash
   export ARMGCC_DIR=~/.mcuxpressotools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi
   export PATH="$HOME/.mcuxpressotools/.mcux-venv-3.12/bin:$PATH"   # ← mcux venv 必須(system python3.14だと失敗)
   (cd ~/mcuxpresso/mcuxsdk/mcuxsdk && west build -b frdmmcxn947 examples/eiq_examples/tflm_cifar10 \
     --toolchain armgcc -Dcore_id=cm33_core0 --config flash_debug -d /tmp/build_cifar10 --pristine always)
   # 受信(115200)を先に開始 → LinkServer flash load .../tflm_cifar10_cm33_core0.elf
   # 結果: Inference time: 221 us / Detected 表示（Microcode mismatch 消滅）= 疎通成功
   ```

## 🔜 次フェーズ（本番化）

疎通は完了。次は **本番モデルの学習・配備**:
- not_crow/crow の2クラス実データで学習し int8 量子化。**入力は 24×24×4**（thermal_abs / thermal_fg / distance / distance_fg）。MLX90640 のサーマルは生 32×24 を取得直後に `rotate_crop` で 24×24 化したものを使い、距離 8×8 は 3倍 kron で 24×24 に拡大する（処理は一貫して 24×24。確定仕様は [../ml-model.md](../ml-model.md) §3.5）。※「none/crow/human の3クラス・32×24」は旧設計。
- 量子化後は同じ手順（3.0.0再変換 → op resolver合わせ → make_model_data_h → 配置 → ビルド/flash）で差し替え。
- 推論結果（`Detected: ...`）を背景差分の検出パイプラインに接続（[[terra-guard-bg-subtraction]]）。
- 最終的には tflm_cifar10 サンプルの間借りをやめ、terra-guard-ai プロジェクト本体へ model.cpp / pcq_npu / op resolver を取り込む。

## ビルド/書き込みコマンド（CLI完結。ユーザーはビルドしない）

- ビルド: 上記 west build（`--config flash_debug` 必須＝NPU版はflash配置前提）
- 書き込み: `/Applications/LinkServer_25.6.131/LinkServer flash "MCXN947:FRDM-MCXN947" load /tmp/build_cifar10/tflm_cifar10_cm33_core0.elf`
- シリアル: 115200 8N1, `/dev/cu.usbmodem*`, pyserial=`/Users/hide/.mcuxpressotools/.mcux-venv-3.12/bin/python`
- gdbserver: `/Applications/LinkServer_25.6.131/LinkServer gdbserver "MCXN947:FRDM-MCXN947"`（port3333）、
  gdb=`~/.mcuxpressotools/arm-gnu-toolchain-*/bin/arm-none-eabi-gdb`。
  ⚠️ gdb終了時にtargetがrunningだと次接続が詰まる→その時は `pkill -f "LinkServer gdbserver"` して再起動。

## 🧹 掃除・状態

- 実機には今「自作モデル(v300・3.0.0版・**動作する**)」が書かれている。
- SDK pcq_npu バックアップ: `.../tflm_cifar10/pcq_npu.orig`。
  元cifar10に戻すなら `pcq_npu.orig/*` を `pcq_npu/` に戻す。
- 一時物: `/tmp/build_cifar10/`、`/tmp/terra_npu_rx.txt`。
- LinkServer/gdbserver の残プロセス無し。
- git: branch `feat/eiq-npu-model`。新規ファイル:
  `docs/nxp/HANDOFF-npu-cifar10-debug.md`, `docs/nxp/HANDOFF-npu-custom-model.md`(本ファイル),
  `tools/ml/make_model_data_h.py`。SDK ツリー(`~/mcuxpresso/...`)の変更はリポジトリ外。

## 関連メモリ

- `npu-cifar10-verified` — cifar10実機検証・無音の真因
- `npu-converter-version-mismatch` — 本件のバージョン不一致詳細
- `eiq-npu-workflow` / `docs/ml-model.md` — 学習/変換/デプロイ手順
