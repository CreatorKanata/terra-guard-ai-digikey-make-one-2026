# src/FRDM-MCXN947

NXP FRDM-MCXN947 の MCUXpresso SDK プロジェクト。MCUXpresso for VS Code 拡張で作成した、リポジトリ管理（repo-managed）プロジェクト。

- 開発ボード: NXP FRDM-MCXN947（Cortex-M33 ×2 + DSP + eIQ Neutron NPU）
- 現状: **LED点滅（led_blinky）でビルド・書き込み環境を確認する段階**
- 今回のスコープ: カラスの検出（センサ取得は次ステップ）

## ディレクトリ構成

- `terra-guard-ai/` — アプリ本体（VS Code 拡張が生成したプロジェクト）
  - `led_blinky.c` — メイン（SysTick割り込みでオンボードLEDをトグル）
  - `CMakePresets.json` / `mcux_include.json` — ビルド設定（toolchain / SDK パス / board / core_id）
  - `frdmmcxn947_cm33_core0/` — ボード初期化・pin_mux・clock 等
  - `.vscode/` — VS Code 用の build / debug / flash 設定
- `sdks/`（gitignore） — プロジェクト専用の MCUXpresso SDK クローン。**コミットしない**（容量大。west / VS Code 拡張で再取得）。

## ビルド・書き込み

### VS Code（推奨）

1. `terra-guard-ai/` を MCUXpresso for VS Code 拡張で開く
2. プロジェクトをビルド（preset: `debug`）
3. FRDM-MCXN947 を USB-C で MCU-Link ポート（J17）に接続
4. Flash / Debug ボタンで書き込み・実行 → オンボードLEDが点滅

### CLI（CMake Presets）

必要な環境変数は `terra-guard-ai/mcux_include.json` の preset 内に定義済み（ARMGCC_DIR / SdkRootDirPath / MCUX_VENV_PATH）。

```bash
cd terra-guard-ai
cmake --preset debug      # configure
cmake --build debug       # build → debug/terra-guard-ai_cm33_core0.elf
```

> 注: `mcux_include.json` の SDK パス等はローカル環境の絶対パスを含む。別環境では VS Code 拡張で再生成するか、パスを調整する。

詳細な開発方針は [../../docs/firmware.md](../../docs/firmware.md) を参照。
