---
name: frdm-mcxn947-dev
description: NXP FRDM-MCXN947 + terra-guard-ai のビルド・書き込み・シリアル確認・切り分けを行うときに使う。MLX90640/VL53L5CX センサ、eIQ Neutron NPU 推論、ステータスLED の開発手順とハマりどころ、ドキュメントの場所を集約。「ビルドして」「書き込んで」「シリアルで確認」「NPUモデルを変換」等で起動。
---

# FRDM-MCXN947 開発スキル (terra-guard-ai)

NXP FRDM-MCXN947 単体で MLX90640(サーマル) / VL53L5CX(距離) を I2C 接続し、
eIQ Neutron NPU でカラス検出を行うプロジェクトの開発手順。
**ビルド・書き込み・シリアル確認は必ず Claude が CLI で実行する**（CLAUDE.md のルール）。

## 確定パス（実機検証済み・推測しない）

- プロジェクトルート: `src/FRDM-MCXN947/terra-guard-ai`
- ELF: `src/FRDM-MCXN947/terra-guard-ai/debug/terra-guard-ai_cm33_core0.elf`
- LinkServer: `/Applications/LinkServer_25.6.131/LinkServer`（バージョンは `ls -d /Applications/LinkServer_*` で都度確認）
- pyserial 入り venv: `/Users/hide/.mcuxpressotools/.mcux-venv-3.12/bin/python`
- シリアルポート: `/dev/cu.usbmodem*`（実機は `FQI2HWQMUXQ2J3` 系。`ls /dev/cu.usbmodem*` で確認）
- **デバッグUART ボーレート: 921600**（115200 ではない。`board.h` の `BOARD_DEBUG_UART_BAUDRATE`）。8N1
- MCUXpresso SDK(west): `/Users/hide/mcuxpresso/mcuxsdk`
- eIQ middleware repo: `/Users/hide/src/github.com/nxp-mcuxpresso/mcuxsdk-middleware-eiq`

## ビルド

```bash
cd src/FRDM-MCXN947/terra-guard-ai
cmake --preset debug          # 初回 or CMakeLists変更時のみ
cmake --build debug
```

- preset は `debug` / `release`（`CMakePresets.json`）。
- `-Werror` 有効。未使用関数(`unused-function`)も即エラーになる。テスト専用関数は
  `#if 〜 #endif` で囲んでビルドから除外すること。
- clang の診断（`fsl_*.h not found` 等）は IDE が SDK include パスを知らないだけの
  ノイズ。実ビルドは armgcc + CMake で通る。**ビルド可否は `cmake --build` の結果で判断する。**

## 書き込み

```bash
/Applications/LinkServer_25.6.131/LinkServer flash "MCXN947:FRDM-MCXN947" \
  load debug/terra-guard-ai_cm33_core0.elf
```

成功時の正常ログ末尾（これで成功）:
```
Boot ROM stalled accessing address 0x50000040 ...
Ns: restart on reset
```

- `LinkServer probes` で接続中プローブを一覧（書き込み前の生存確認に便利）。
- `LinkServer reset` というサブコマンドは**無い**。`run`/`flash`/`gdbserver` 等のみ。
- **flash 直後に VCOM が一時切断**され `/dev/cu.usbmodem*` が一瞬消えることがある。
  `sleep 1` してポートを再確認してからシリアルを開く。

## シリアル確認（921600）

ポートを開きっぱなしにしてから受信するのが基本。テキスト行とバイナリフレームが
混在するので、用途で読み方を変える。

```bash
PY=/Users/hide/.mcuxpressotools/.mcux-venv-3.12/bin/python
"$PY" - <<'EOF'
import serial, time
ser = serial.Serial('/dev/cu.usbmodemFQI2HWQMUXQ2J3', 921600, timeout=0.1)
ser.reset_input_buffer()
buf = bytearray(); t0=time.time()
while time.time()-t0 < 5.0:
    d = ser.read(8192)
    if d: buf += d
ser.close()
print("bytes:", len(buf))
EOF
```

- **テキスト行**（INFER/統計/起動ログ）を読むときはバイナリに紛れるので、`buf` を
  `latin-1` decode して `re.finditer(r'INFER,[-\d,]+', text)` 等で抽出する
  （`readline()` だとバイナリ中の改行で化ける）。
- 文字だけ化けてタイミングは合う → **ボーレート不一致**（921600 を疑う）。

### 出力フォーマット（同一シリアル上で混在、行頭マーカ/magicで識別）

- `0xAA55` + Ta(int16) + 576×int16 … サーマル（**rotate_crop 後 24×24**, 1/100℃, 1156B）
- `0xAA56` + 576×int16 … サーマル前景（背景差分の正側, 24×24）
- `DIST,<z0..z63>` / `STAT,<s0..s63>` / `DFG,...` … 距離8×8（mm / target_status / 前景）
- `INFER,<確定crow 0/1>,<p_crow×1000>,<conf×1000>,<raw crow>,<streak>` … NPU推論
- `FRAME,<Ta_centi>,...` … サーマルのテキスト版（低速）

### ビューア（ホスト側可視化）

```bash
tools/.venv/bin/python tools/dual_viewer_web.py --port /dev/cu.usbmodemXXXX
# → http://127.0.0.1:8050 をブラウザで開く
```
※ `tools/.venv` は x86_64 で arm64 環境だと numpy が動かない場合がある。
   その場合は計算検証を純Python（整数インデックス）で行うか別venvを使う。

## 物理リセット（USB抜き差し）をユーザーに依頼してよい場面

CLAUDE.md 公認。以下のときは遠慮なく声をかける:
- **センサ(I2C: MLX90640/VL53L5CX)が無反応**（バスロック）→ USB抜き差しで回復
- 書き込み直後にクリーンな起動ログを取りたい
- シリアルが文字化け／無音で切り分けが必要
- `LinkServer probes` で **No probes detected** → 抜き差しでプローブ復帰

## ハマりどころ（実機で解決済み）

### GPIO/LED が光らない → GPIO クロックゲート
MCXN947 は GPIO にクロックゲートがある。`CLOCK_EnableClock(kCLOCK_Gpio0)`
（GPIO1 なら `kCLOCK_Gpio1`）を呼ばないと `GPIO_PinWrite`/`PortToggle` が
レジスタに効かず**LEDが光らない**。ピンmuxが正しくても無意味。
症状: ループ自体は回る（SysTick/PRINTFは別系統）のに GPIO 出力だけ死ぬ。

### SysTick で無音ハング → SysTick_Handler 必須
`SysTick_Config()` は SysTick 割り込みを有効化する。対応する
`void SysTick_Handler(void)` を定義しないと startup の weak default
（無限ループ）に落ちて**起動直後にシリアル完全無音**。SDK の
`examples/demo_apps/led_blinky` の `g_systickCounter` パターンに従う。

### 外部I2C(FC2/LPI2C2) 1MHz化
J8/J2 = FC2(LPI2C2, P4_0=SDA/P4_1=SCL)。FRO12M では 1MHz SCL を作れず
センサが ACK しない。`hardware_init.c` で FRO_HF を FRO_HF_DIV ÷2=24MHz に
して FC2 へアタッチ（BootClocks 後）。バスロックは USB 抜き差しで回復。

### NPU converter とドライバのバージョン一致
neutron-converter のバージョンが SDK 内蔵ドライバと不一致だと microcode が
合わず推論が回らない。**ファーム内蔵ドライバは 3.0.0** なので変換も 3.0.0 で行う。
`neutron_convert.sh` のデフォルトSDKは 3.1.3 を指すため、必ず
`NEUTRON_SDK_DIR=$PWD/sdk/eiq-neutron-sdk-linux-3.0.0` を前置きすること。
詳細は `docs/ml-model.md`。

### サーマルは 90度右回転＋中央24行crop済み（24×24）
**⚠️ 重要: センサ生は 32×24=768 だが、取得直後に 24×24 化し、以降のすべての処理は 24×24 で行う。**
「32×24」と書いてよいのは MLX90640 の**生センサ仕様**を説明するときだけ。
`thermal_mlx90640.c` の `rotate_crop()` が生フレーム(24行×32列)を時計回り90度
回転し、中央24行を crop して **24×24=576画素** にする。以降の全消費者
（bin送出 0xAA55 / 前景 0xAA56 / 統計 / 背景差分 / NPU入力 / データ収集）は 24×24 を見る。
**NPU入力もそのまま 24×24（pad は廃止）**。距離(VL53)は回転しないが左右反転
（`vl53_flip_h()`）済み。学習側（`build_trainset.py` / `collect_dataset.py`）も
同じ前処理に一致（回転・crop・反転はファームで確定、収集・ビューアは無加工）。

## ML / NPU ワークフロー（再学習の流れ・✅ 2026-06-22 実機検証済み）

カラス検出 **2クラス（not_crow / crow）**。入力は **24×24×4**
（ch0 thermal_abs / ch1 thermal_fg / ch2 distance(8×8を3倍kron) / ch3 distance_fg）。

1. データ収集: `tools/ml/collect_dataset.py`（CLI）or `tools/dual_viewer_web.py`
   の Start/Stop/Delete UI
2. トレインセット構築: `tools/.venv/bin/python tools/ml/build_trainset.py`
   （`dataset/raw/*.npz` → `dataset/built/X.npy,y.npy` [N,24,24,4]）
3. 学習+int8量子化: `arch -arm64 tools/ml/.venv/bin/python tools/ml/train_model.py`
   （⚠️ arm64必須。出力 int8 [1,24,24,4]→[1,2]）
4. NPU変換: `NEUTRON_SDK_DIR=$PWD/sdk/eiq-neutron-sdk-linux-3.0.0 \
   tools/ml/neutron_convert.sh tools/ml/build/terra_guard_int8.tflite \
   tools/ml/build/terra_guard_int8_mcxn94x.tflite mcxn94x`
5. モデルヘッダ生成: `tools/.venv/bin/python tools/ml/make_model_data_h.py \
   --src-h tools/ml/build/terra_guard_int8_mcxn94x.h --out tools/ml/build/model_data.h \
   --name terra_guard_crow --arena 65536` → `tflm/pcq_npu/model_data.h` に差し替え
6. ビルド・書き込み（op resolver は Softmax+Slice+NEUTRON_GRAPH の3op固定で変更不要）

前処理（回転/crop/正規化レンジ/距離3倍kron）は**ファーム `npu_infer.c` と学習
`build_trainset.py` で厳密一致**させること（量子化 scale=1/255・zp=-128 も一致）。

### 距離前景の複合判定（陸上カラス対応・✅ 2026-06-22）
地面に立つカラスは背景=地面との距離差が体高ぶん 15〜20cm（最大80mm）しか出ず、
地面ノイズ床（σ p90 41mm / max 126mm）と重なり**単一閾値では分離不能**。
`bg_subtract.c` は距離前景 ON を 30mm に下げ、**連結塊（≥2px かつ 塊和≥100mm）
または前景上位3和≥120mm** の「まとまり」で誤検出を抑える複合判定にした。
保存データでの検証は `tools/ml/test_foreground_detect.py`、実測は
`dataset/validation/2026-06-22_crow_{present,absent}.json`。DET 行は後方互換で
末尾に d_cluster_size/d_cluster_sum/d_top3_sum を追加。

## ドキュメントの場所

- `docs/overview.md` — プロジェクト全体像
- `docs/hardware.md` — 部品・配線・システム構成
- `docs/firmware.md` — MCUXpresso SDK 開発手順
- `docs/sensor-processing.md` — 前処理・特徴量・検出ロジック
- `docs/ml-model.md` — 学習〜NPUデプロイ手順
- `docs/HANDOFF.md` — セッション間の作業状態（引き継ぎ）
- `docs/datasheets/FRDM-MCXN947/` — User Manual(UM12018)/QSG/基板図(FAB)/pin-layout.png
- `docs/datasheets/MLX90640.md` / `VL53L5CX.md` — センサ要約 + PDF

## 関連メモ（~/.claude/.../memory/）

`frdm-mcxn947-build-env` / `frdm-mcxn947-gpio-led` / `frdm-mcxn947-i2c-buses` /
`frdm-mcxn947-vl53l5cx` / `frdm-mcxn947-onboard-sensor` / `mcu-link-firmware-recovery` /
`eiq-npu-workflow` / `npu-cifar10-verified` / `npu-converter-version-mismatch` /
`terra-guard-bg-subtraction` / `terra-guard-data-collection` / `crow-thermal-signature`
