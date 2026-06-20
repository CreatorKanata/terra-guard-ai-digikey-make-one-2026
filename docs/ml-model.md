# 学習モデルの作り方とNPUデプロイ（FRDM-MCXN947 / eIQ Neutron NPU）

FRDM-MCXN947 の **eIQ Neutron NPU** で動く学習モデルを、**Mac 上で作って書き込む**までの調査結果と方針をまとめる。

> **今回のスコープ（確定）: まずは「分類だけ」（なし / カラス / 人）を NPU で動かす。**
> カラス時の 8×8 ヒートマップ出力は後段（[後述の発展](#5-発展カラス時の-88-ヒートマップ)）。

---

## 0. 結論（先に要点）

- **NPU は `int8` 量子化済みの TensorFlow Lite モデルしか加速しない。** float32 や uint8 主体のモデルは CPU フォールバックになる。
- 公式の道は2通り。本プロジェクトは **(B) を主軸**にする。
  - **(A) eIQ Portal GUI**: ノーコードで学習〜量子化〜変換。ただし**画像分類デモ前提**で、カメラ画像(128×128×3)向け。我々の「サーマル+距離の多チャネル小画像」「マルチタスク」には窓が狭い。**eIQ Portal は Windows/Linux 配布で macOS 非対応**。
  - **(B) Keras/TF で自作 → INT8量子化 → `neutron-converter` で NPU 変換**: モデル形状・入力チャネル・出力ヘッドを自由に設計でき、Mac の Python で学習まで完結。**変換ツール `neutron-converter` だけ Linux x86 実行**が必要（本機 Mac は x86_64 なので Docker で動く見込み。[6章](#6-neutron-converter-を-mac-で動かす)）。
- **C言語側の組み込みは SDK サンプル `tflm_cifar10`（FRDM-MCXN947 NPU版 = `pcq_npu`）がそのまま雛形になる。** モデルを差し替えるだけ。32×32×3 の CNN 分類が **約1ms/推論**で動く実績がサンプル readme に記載。
- NPU の対応オペレータ表（int8）に、tiny CNN に必要な op（Conv2D / DepthwiseConv2D / FullyConnected / Pooling / Softmax / Add / Relu 等）は**すべて揃っている**（[4章](#4-npu-対応オペレータint8)）。

---

## 1. 参照ソース（ローカルに取得済み）

調査の一次情報。online doc を読むより手元のこれらが速くて正確。

| 場所 | 内容 |
| --- | --- |
| `docs/nxp/MCXN947：顧客MLモデルをトレーニングし、NPUに展開する方法.pdf` / `.md` | NXP公式記事（[原文](https://community.nxp.com/t5/MCX-Microcontrollers-Knowledge/MCXN947-How-to-Train-and-Deploy-Customer-ML-model-to-NPU/ta-p/1899497)）。eIQ Portal GUI での学習〜変換〜CIFAR10サンプル展開の手順 |
| `~/src/github.com/nxp-mcuxpresso/mcuxsdk-middleware-eiq` | **eIQ Middleware repo**（[GitHub](https://github.com/nxp-mcuxpresso/mcuxsdk-middleware-eiq)）。Neutron NPU の**オンデバイス側**＝`neutron/driver/include/NeutronDriver.h`（NPU実行API）と `libNeutronDriver.a` / `libNeutronFirmware.a`。SDK 25.06 / Neutron Software 3.0.0。<br>※この repo の `docs/index.rst` は online doc `.../eiq/tensorflow-lite/docs/ugindex.html` の**生成元**。ただし手元 clone は部分的で `tensorflow-lite/` サブツリーは含まれない（west で別取得） |
| `~/mcuxpresso/mcuxsdk/mcuxsdk/middleware/eiq/tensorflow-lite/docs/` | **TFLite-Micro 公式UGの実体**（= 上記 online doc と同一内容）。`topics/convert_model.md`（変換手順）、`topics/supported_operators.md`（**対応op表**）など |
| `~/mcuxpresso/mcuxsdk/mcuxsdk/examples/eiq_examples/tflm_cifar10/` | **C側の雛形となる SDK サンプル**。`readme.md` に「自分のモデルへの差し替え手順」「neutron-converter コマンド」「TensorArena サイズ調整」まで全部書いてある |
| `~/mcuxpresso/mcuxsdk/mcuxsdk/examples/_boards/frdmmcxn947/eiq_examples/tflm_cifar10/pcq_npu/` | **FRDM-MCXN947 専用の NPU 版モデル一式**（`cifarnet_quant_int8_npu.tflite` / `model_data.h` / `model_cifarnet_ops_npu.cpp`）。これを我々のモデルで置き換える |

他にも `examples/_boards/frdmmcxn947/eiq_examples/` に `tflm_label_image` / `tflm_kws`（キーワード検出）/ `tflm_modelrunner` / `mpp` がある。

---

## 2. 全体フロー（Mac 中心・(B) 方式）

```
[1] データ収集            実機(FRDM)からUART経由でサーマル/距離フレームを記録（tools/ のビューア基盤を流用）
        │                  ラベル付け：なし / カラス / 人
        ▼
[2] 学習 (Mac, Python)     Keras/TF で tiny CNN を学習（float32）。NHWC [batch,H,W,C]
        │
        ▼
[3] INT8量子化 (Mac)       TFLiteConverter + 代表データセットで full-integer量子化 → quant_int8.tflite
        │                  入出力も int8（NPU要件）
        ▼
[4] NPU変換 (Linux/Docker) neutron-converter --target mcxn94x --dump-header-file-output
        │                  → *_npu.tflite と model_data.h（C配列）+ op登録スニペットを生成
        ▼
[5] 組み込み (Mac, CLI)    tflm_cifar10 を雛形に model_data.h と op resolver を差し替え、
        │                  kTensorArenaSize を調整
        ▼
[6] ビルド・書き込み       cmake --preset debug && cmake --build debug → LinkServer flash
        │
        ▼
[7] 実機推論              入力テンソルにセンサフレームを詰めて MODEL_RunInference()
```

ステップ [1][2][3][5][6][7] は **Mac でそのまま**できる。**[4] だけ Linux x86 バイナリ**（[6章](#6-neutron-converter-を-mac-で動かす)）。

---

## 3. モデル設計（分類：なし / カラス / 人）

### 3.1 入力テンソルの作り方

センサは解像度が違う（サーマル 32×24、距離 8×8）。**サーマルの 32×24 を基準グリッド**にして、距離を最近傍 or バイリニアで 32×24 に拡大し、**マルチチャネル画像**として 1 本のテンソルに束ねるのがNPUに優しい（Conv2Dが1枝で済む）。

推奨入力（NHWC, `int8`）: **`[1, 24, 32, C]`**（H=24, W=32）

候補チャネル（既存の背景差分実装 [bg_subtract](../src/FRDM-MCXN947/terra-guard-ai/app/bg_subtract.h) を活かす）:

| ch | 内容 | 出所 |
| --- | --- | --- |
| 0 | サーマル生フレーム（℃を正規化） | MLX90640 |
| 1 | サーマル前景（背景差分。暖かい物体） | `bg_thermal_fg()` |
| 2 | 距離前景を24×32へ拡大（手前に出た量） | `bg_dist_fg()` を upsample |

- まずは **ch=1本（サーマル前景だけ）** で最小モデルを動かし、精度が足りなければ ch を足す、が安全な進め方。
- **正規化はC側で固定スケールに揃える**（学習時と同じ式で int8 に量子化）。サーマルは「背景比の差分℃」を 0〜数℃ → 0〜127 にクリップする等、レンジを固定すると安定。

> なぜ画像化するか: NPU が速いのは Conv 系。768+64 を素のベクトルで FullyConnected に入れるより、空間構造（カラス＝小さな塊／人＝大きな塊）を Conv で捉えた方が小さいモデルで効く。

### 3.2 アーキテクチャ候補

いずれも **INT8量子化前提・NPU対応opのみ**で構成（[4章](#4-npu-対応オペレータint8)の表で全てYes）。

**候補A（最小・まず動かす用）— Tiny CNN**
```
Input [24,32,C] int8
→ Conv2D 8ch 3x3 + ReLU → MaxPool 2x2     # 12x16x8
→ Conv2D 16ch 3x3 + ReLU → MaxPool 2x2    # 6x8x16
→ GlobalAveragePool (= AvgPool)           # 1x1x16
→ FullyConnected 3 → Softmax
```
- パラメータ数 ~数千、MACs ~数十万。`cifarnet`(91KB) よりはるかに軽い。推論 < 1ms 見込み。

**候補B（精度寄り）— DepthwiseSeparable**
```
Conv2D(8) → [DW3x3 + PW1x1]×2 を挟む → AvgPool → FC(3) → Softmax
```
- MobileNet 風。DepthwiseConv2D も int8 でNPU対応。パラメータ効率が良い。

**候補C（時系列）— Nフレームスタック**
- 動き（カラスは速い／人はゆっくり大きい）を使うため、直近 N=3 フレームを**チャネル方向に積む**だけ（`[24,32,C×N]`）。アーキは候補A/Bのまま。3D Conv は不要（NPU非対応）。

> 進め方の推奨: **候補A・1チャネル・単フレームから**。動いたらチャネル/フレームを足す。

### 3.3 出力

- 3クラス（none / crow / human）の Softmax。`tflm_cifar10` の `get_top_n` / `output_postproc` 流用で「最尤クラス＋信頼度」を取り出せる。

### 3.4 データ収集とラベル

- **実機ロギング**: 既存の UART ビューア基盤（`tools/dual_viewer.py` 等、サーマル=バイナリ `0xAA55`/前景=`0xAA56`、距離=テキスト `DIST,`）でフレームを保存 → クラス別フォルダに振り分け（eIQ_Toolkit のデータセット構造と同じ流儀）。
- **カラスが集めにくい問題**: 初期は「暖かい小物体を動かす」ダミー（[roadmap.md](./roadmap.md) のダミーカラス実験）で代用しつつ、人は自分で歩いて収集。
- **データ拡張（サーマル向け）**: 左右反転 / 小さな平行移動 / 温度ジッタ（全体 ±数℃）/ 背景差分後ならゲイン微調整。小モデルなので数百〜数千フレーム/クラスで一旦回せる。
- **「なし(none)」が大半になる不均衡**に注意。none をサブサンプリング、または crow/human を拡張で増やす。

---

## 4. NPU 対応オペレータ（int8）

`topics/supported_operators.md`「MCXN947 (eIQ Neutron NPU)」列より。**tiny CNN に必要な op は全部 int8 で Yes**。

| op | int8 NPU | 備考 |
| --- | --- | --- |
| CONV_2D | ✅ | 主力 |
| DEPTHWISE_CONV_2D | ✅ | MobileNet系に必須 |
| FULLY_CONNECTED | ✅ | 分類ヘッド |
| AVERAGE_POOL_2D / MAX_POOL_2D | ✅ | プーリング |
| RELU / RELU6 / LEAKY_RELU | ✅ | 活性化 |
| SOFTMAX / LOGISTIC / TANH | ✅ | 出力・活性化 |
| ADD / SUB / MUL | ✅ | 残差・スケール |
| CONCAT / PAD / TRANSPOSE | ✅ | 接続・整形 |
| RESIZE_NEAREST_NEIGHBOR | ✅ | （距離8×8の拡大をモデル内でやる場合） |
| TRANSPOSE_CONV | ✅ | （ヒートマップのupsampleに使える＝発展で有用） |

- 表に**無い op / int8列がNoの op は CPU フォールバック**（CMSIS-NN or リファレンス実装）。動くが遅く、TensorArena が増える。設計時は上表の op だけで組む。
- **量子化は per-channel int8（PCQ）が前提**（サンプルのフォルダ名 `pcq_npu` = per-channel-quant NPU）。Keras→TFLite の full-integer 量子化が標準でこれ。

---

## 5. C側の組み込み（`tflm_cifar10` 雛形）

差し替えは公式 readme どおり **2ステップ**。

1. **`model_data.h` を差し替え**: neutron-converter が吐く `model_data[]` / `model_data_len` に置換。`kTensorArenaSize` を converter 出力の "Total data" の **約105%** に設定（小さすぎると `AllocateTensors() failed`）。
2. **op resolver を差し替え**: converter が吐く `.h` 内の op 登録スニペット（例 `AddReshape/AddSlice/AddSoftmax/AddDequantize/AddCustom(NEUTRON_GRAPH)`）を `model_cifarnet_ops_npu.cpp` の `MODEL_GetOpsResolver()` に反映。**カスタムop `NEUTRON_GRAPH` の登録が NPU 実行の肝**。

推論API（`examples/eiq_examples/common/tflm/model.cpp` と `main.cpp`）:
```c
MODEL_Init();                                   // インタプリタ生成・AllocateTensors
in  = MODEL_GetInputTensorData(&inDims,&inType);// [batch,H,W,C], int8
out = MODEL_GetOutputTensorData(&outDims,&outType);
// ... in に正規化済みセンサフレームを int8 で詰める ...
MODEL_RunInference();                            // 内部で NeutronDriver が NPU 実行
MODEL_ProcessOutput(out,&outDims,outType,dt);    // argmax + 信頼度
```
- オンデバイス NPU 実行の低レベルAPIは `NeutronDriver.h`（`neutronInit / neutronModelPrepare / neutronRunBlocking`）。通常は TFLite-Micro のカスタムop経由で間接的に呼ばれるので直接触らない。
- Neutron-Software のバージョン更新は `NeutronDriver.h` / `NeutronErrors.h` / `libNeutronDriver.a` / `libNeutronFirmware.a` の4点を差し替えるだけ（readme記載）。

> **本プロジェクトへの統合方針**: まず `tflm_cifar10`(frdmmcxn947) 単体を west でビルド→書き込みし、**NPU で推論が回ること自体を疎通確認**（[firmware.md](./firmware.md) の west 手順と同じ流儀）。その後、推論コードを `terra-guard-ai` 本体へ移植し、入力をセンサフレームに差し替える。

---

## 6. Mac での環境構築と neutron-converter 実行（✅ 実機検証済み 2026-06）

Keras学習(Mac arm64) → int8量子化(Mac) → neutron-converter(Docker) → C ヘッダ生成、までを
**実際に通して確認済み**。手順とハマりどころを残す。

### 6.1 学習用 Python 環境（TF）— ⚠️ arm64 ネイティブ必須

このマシンは **Apple Silicon(M2 Max) だが、シェル環境が x86_64(Rosetta)** という構成。
TensorFlow の macOS wheel まわりで次の落とし穴がある（全部実機で踏んだ）。

- ❌ **x86_64/Rosetta の TF は import 時にクラッシュ**（`AVX instructions ... aren't available`, SIGABRT/exit134）。
  Rosetta は AVX を提供しないため、TF の x86_64 wheel は動かない。
- ❌ **pyenv で arm64 Python を自前ビルドも失敗しやすい**。x86 Homebrew(`/usr/local`)の
  `libintl`(gettext) を拾って `__locale_textdomain ... symbol(s) not found for architecture arm64`、
  `/usr/local` を排除すると今度は OpenSSL が無く `_ssl` 欠損になる。
- ✅ **解決: Apple 付属の `/usr/bin/python3`(universal2, _ssl同梱) を `arch -arm64` で起動して venv を作る。**

```bash
# venv 作成（arm64固定。Python3.9.6 / TF2.16.2 でOK）
arch -arm64 /usr/bin/python3 -m venv tools/ml/.venv
arch -arm64 tools/ml/.venv/bin/python -m pip install -r tools/ml/requirements.txt
# import 確認（machine: arm64 と出れば成功。x86だとここで落ちる）
arch -arm64 tools/ml/.venv/bin/python -c 'import tensorflow as tf,platform; print(platform.machine(), tf.__version__)'
```

> ⚠️ **以降、venv の python を叩くときは必ず `arch -arm64` を前置きする**（シェルが x86_64 のため）。

### 6.2 int8 TFLite の生成 — TF2.16 の変換クラッシュ回避

`tools/ml/make_test_model.py` が雛形（tiny CNN → full-int8量子化）。実装上の注意:

- ❌ `tf.lite.TFLiteConverter.from_keras_model(model)` は TF2.16(Keras3) で MLIR が落ちる
  （`ReadVariableOp: missing attribute 'value'` → `LLVM ERROR: Failed to infer result type`）。
- ✅ **`model.export(saved_dir)` で SavedModel 化 → `from_saved_model(saved_dir)` で変換**すると安定。
- full-int8 設定: `optimizations=[DEFAULT]` + `representative_dataset` +
  `supported_ops=[TFLITE_BUILTINS_INT8]` + `inference_input_type=int8` / `inference_output_type=int8`。

```bash
arch -arm64 tools/ml/.venv/bin/python tools/ml/make_test_model.py --out build/terra_guard_int8.tflite
# → 入力 int8 [1,24,32,1] / 出力 [1,3] の量子化tfliteが出る
```

### 6.3 neutron-converter（Docker amd64 エミュ）

- **`neutron-converter` は eIQ Neutron SDK 同梱の Linux **x86-64** ELF バイナリ**
  （本リポジトリでは `sdk/eiq-neutron-sdk-linux-3.1.3/bin/neutron-converter`、.gitignore済み・各自DL）。
  [eIQ Toolkit / Neutron SDK ダウンロード](https://www.nxp.com/design/design-center/software/eiq-ai-development-environment/eiq-toolkit-for-end-to-end-model-development-and-deployment:EIQ-TOOLKIT)。
- Mac の Docker は `linux/aarch64` で動くため、**`--platform linux/amd64`（QEMUエミュ）で実行**する。
  → `tools/ml/neutron_convert.sh` がこのラッパー（SDKをマウントして amd64 コンテナで実行）。✅ 動作確認済み。

```bash
tools/ml/neutron_convert.sh build/terra_guard_int8.tflite \
    build/terra_guard_int8_mcxn94x.tflite mcxn94x
# 内部で: docker run --platform linux/amd64 ... neutron-converter
#   --input ... --output ... --target mcxn94x --dump-header-file-output
```

- **target デフォルトは `mcxn94x`**（= FRDM-MCXN947。MCXN947 は Neutron-C ターゲット）。
  `neutron-converter --show-targets` で一覧確認可。RT700専用の `--use-sequencer` /
  `--fetch-constants-to-sram` は MCXN947 では不要。
- 出力: NPU版 `*_mcxn94x.tflite` と、C配列 `*_mcxn94x.h`（`model_data[]` / `model_data_len`
  + op登録スニペット `AddSoftmax()` / `AddCustom(NEUTRON_GRAPH)`）。
- 検証時の実例（tiny CNN, 1,299 params）: **オペレータ変換率 9/10 = 0.9**（1 NeutronGraphに集約、
  残りSoftmaxはCPU）、Total data 8,464B / weights 2,272B。`kTensorArenaSize` は Total data ×1.05 が目安。

### 6.4 SDK バージョン互換の注意

- 検証に使ったのは **eIQ Neutron SDK 3.1.3**（converter出力に `Neutron Converter Version: 3.1.3`）。
  一方、ファーム側 eIQ Middleware は **SDK 25.06 / Neutron Software 3.0.0**。
  **converter と オンデバイス NeutronFirmware/Driver のバージョン整合**は要確認
  （ズレると実機で動かない可能性。記事も「converterバージョンをSDKと互換確認」と明記）。
  実機書き込み時に不整合が出たら、`eiq/neutron/` の4ファイル
  （`NeutronDriver.h`/`NeutronErrors.h`/`libNeutronDriver.a`/`libNeutronFirmware.a`）を
  converter と同世代に合わせて差し替える。
- 代替: eIQ Portal が使える Windows/Linux 環境があれば (A) GUI 変換も可
  （ただし多チャネル入力・マルチタスクは GUI では難しい）。

---

## 7. 発展：カラス時の 8×8 ヒートマップ

分類が動いた後の拡張。設計だけ先に置く（実装は後段）。

- **タスク化**: 8×8 の**占有グリッド回帰**（各ゾーンに「カラスらしさ」0〜1）。`Conv → TRANSPOSE_CONV/RESIZE で 8×8 にアップサンプル → Sigmoid(Logistic)` の小ヘッドを分類と共有エンコーダにぶら下げる**マルチタスク**が省メモリ。
- **正解ラベル**: 背景差分の前景ブロブ重心に**ガウシアン**を置いて 8×8 の教師マップを自動生成（手動アノテーション最小化）。
- NPU op 的には TRANSPOSE_CONV / RESIZE_NEAREST_NEIGHBOR / LOGISTIC が int8 Yes なので実装可能。

---

関連: [firmware.md](./firmware.md)（ビルド・書き込み・west）/ [roadmap.md](./roadmap.md)（Step4 カラス検出）/ [sensor-processing.md](./sensor-processing.md)（特徴量）
