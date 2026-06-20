# 申し送り — NPU 疎通確認(tflm_cifar10) ✅ 解決済み

最終更新: 2026-06-20 / branch `feat/eiq-npu-model` / GitHub Issue #2

## 🎉 結論（2026-06-20 解決）

**tflm_cifar10(NPU版) は実機で完全正常動作。NPU 疎通確認は成功。**
受信ログ: `Model: cifarnet_quant_int8_npu` / `Core/NPU Frequency: 150 MHz` / `Inference time: 7130 us` / `Detected: ship (99%)`。

**「シリアル完全無音」の真因 = 受信タイミングの取りこぼし**（ハング/HardFault ではなかった）。
cifar10 は起動ログ＋推論結果を**起動直後に1回だけ**出力するため、「書き込み後にシリアルを繋ぐ」順だと出力済みで無音に見えていた。
**正しい手順: 受信を先に開始(115200, FC4/LPUART4) → その後 `LinkServer flash load` でクリーン起動**（LinkServer 単体 reset コマンドは無い）。

gdb 切り分けで確定: main→BOARD_Init→TIMER_Init→DEMO_PrintInfo→MODEL_Init(GetModel/interpreter/**AllocateTensors完了**/return success)→推論ループ、すべて到達。HardFault は一度も発火せず。

**次フェーズ**: この検証済みパイプラインの `model_data.h` を自作3クラス(なし/カラス/人)に差し替える。

---

## ゴール（このセッションの作業）

FRDM-MCXN947 で **NPU 推論が実機で回ること**を、SDK サンプル `tflm_cifar10`(frdmmcxn947, NPU版) で疎通確認する。これが通れば、自作モデルの `model_data.h` を差し替えて本番（分類: なし/カラス/人）へ進む。

## ✅ ここまで完了していること

1. **Mac での Keras→int8→neutron-converter(mcxn94x) は完全成功**（Issue #2 主目的の半分達成・コミット済み `5df95a4`）。
   - `tools/ml/`: arm64 venv(`arch -arm64 /usr/bin/python3`)、`make_test_model.py`(tiny CNN→int8)、`neutron_convert.sh`(Docker amd64 で neutron-converter)、`requirements.txt`。
   - 検証成果物（.gitignore の `build/`）: `build/terra_guard_int8.tflite`、`build/terra_guard_int8_mcxn94x.tflite` + `.h`（変換率9/10）。
   - 詳細は `docs/ml-model.md` §6（実機検証済み手順・ハマりどころ）。
2. **tflm_cifar10(NPU版) のビルド成功**。
   - コマンド: `~/mcuxpresso/mcuxsdk/mcuxsdk` で
     - `export ARMGCC_DIR=~/.mcuxpressotools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi`
     - `export PATH="$HOME/.mcuxpressotools/.mcux-venv-3.12/bin:$PATH"`
     - `west build -b frdmmcxn947 examples/eiq_examples/tflm_cifar10 --toolchain armgcc -Dcore_id=cm33_core0 --config flash_debug -d /tmp/build_cifar10`
   - 成果物: `/tmp/build_cifar10/tflm_cifar10_cm33_core0.elf`（neutron.cpp / pcq_npu モデル含む。ELF内にログ文字列 "CIFAR-10" "Model: cifarnet_quant_int8_npu" 等あり = 正しくビルド済み）。
   - ⚠️ ターゲットは `--config flash_debug`（NPU版は flash 配置前提。`debug` ではなく `flash_debug`）。

## ❌ 詰まっていること（次に解くべき問題）

**tflm_cifar10 を書き込むと シリアルが完全無音**（115200, `DEMO_PrintInfo()` の起動ログすら出ない）。

### 切り分け済み（重要）
- **シリアル経路・LinkServer・flash後の自動起動は正常**。証拠: terra-guard(動作実績) を書き戻したら 921600 で 112,980 バイト受信、DIST/STAT/DET/サーマルマジック(0xAA55/0xAA56)すべて確認。→ **ハード/UART経路/ボードは正常。cifar10 ファーム固有の問題**。
- pin_mux 一致: cifar10 も terra-guard も UART=P1_8/P1_9=FC4(Alt2)。同一。
- clock_config.c は cifar10 ビルドに含まれている（クロック欠落ではない）。
- USB抜き差し物理リセット & RESETボタン押下でも cifar10 は無音（タイミングの問題ではない）。
- → 仮説: **`BOARD_Init()` か起動直後でハング/HardFault**（PrintInfo前で死んでいる）。NPU初期化(`MODEL_Init`)はPrintInfoの後なので、それ以前で止まっている。

### 次の一手（やりかけ）
- **gdb で止め位置(PC)を特定**する。
  - gdb: `~/.mcuxpressotools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gdb`
  - 手順案: LinkServer gdbserver を起動
    `/Applications/LinkServer_25.6.131/LinkServer gdbserver "MCXN947:FRDM-MCXN947"`（port 3333）
    → gdb で `target remote localhost:3333` → `monitor reset` → `load` →
    `break main` / `break MODEL_Init` / `tbreak DEMO_PrintInfo` を置いて `continue` で
    どこまで到達するか確認。`main` に来ないなら起動コード(reset handler/clock)で死亡。
    `main`到達後 PrintInfo 前で死ぬなら BOARD_Init 内。
  - 注意: バッチの `continue &`+`interrupt` は不安定だった。`-batch -x script` で break を使う方式を推奨。

### 有力な代替方針（gdbで原因が割れない/時間かかる場合）
cifar10 サンプル自体の起動問題に深入りせず、**terra-guard(動作実績ある起動基盤=クロック/pin_mux/コンソール) に NPU 推論コードを移植**する。
- `examples/eiq_examples/common/tflm/model.cpp`(MODEL_Init/RunInference) + `pcq_npu/model_data.h` + op resolver を terra-guard プロジェクトに取り込み、`prj.conf` に
  `CONFIG_MCUX_COMPONENT_middleware.eiq.tensorflow_lite_micro.binary=y` と `...neutron=y` を追加。
- terra-guard は 921600 / debug_console_lite。cifar10 は 115200 / SDK_DEBUGCONSOLE_UART。コンソール差異に注意。
- これなら「起動が確実に動く土台」の上で NPU 推論だけ検証でき、最短で本番(センサ入力→NPU分類)に繋がる。

### もう一つの確認候補
- converter=3.1.3 と ファーム側 eIQ middleware の Neutron Software バージョン整合（前から要注意としている点）。ただし今回は PrintInfo 前で死んでいるので NPU 以前の問題の可能性が高い。優先度は gdb での PC 特定が先。

## 🧹 掃除（セッションリセット前に / 次セッション開始時に確認）

- **LinkServer gdbserver が残っている可能性**: `pgrep -fl "LinkServer gdbserver"` で確認し、いれば `kill <PID>`（このセッションでは PID 7433、port 3333/4444 を握る。残すと次の flash/gdb が衝突）。
- 実機には今 **terra-guard が書かれている**（デバッグ中に書き戻した）。
- 一時物: `/tmp/build_cifar10/`(cifar10ビルド)、`/tmp/gdbserver.log`、`/tmp/gdb*.txt`。
- git: branch `feat/eiq-npu-model`、ワーキングツリーはクリーン（この申し送り追加分を除く）。

## 参照
- `docs/ml-model.md`（全体フロー・§6実機検証済み手順）
- Issue #2: https://github.com/CreatorKanata/terra-guard-ai-digikey-make-one-2026/issues/2
- SDK サンプル: `~/mcuxpresso/mcuxsdk/mcuxsdk/examples/eiq_examples/tflm_cifar10/` と `_boards/frdmmcxn947/eiq_examples/tflm_cifar10/pcq_npu/`
- メモリ: `eiq-npu-workflow.md`
