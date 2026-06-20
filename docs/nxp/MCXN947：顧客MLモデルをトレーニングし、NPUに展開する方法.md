# MCXN947: Customer ML ModelをNPUへ学習・変換・デプロイする手順

## 概要

NXPのMCX Nシリーズ、特に **MCXN947 / MCX N94x** では、eIQ Neutron NPUを使って機械学習推論を高速化できる。

MCXN947は、従来のMCU単体CPU実行と比べて、NPUによりML推論を高速化できる。記事では、NPUは最大でCPU単体比38倍の推論性能を示すと説明されている。

## ハードウェア構成

- 開発ボード: [FRDM-MCXN947](https://www.nxp.com/design/design-center/development-boards-and-designs/FRDM-MCXN947)
- LCD: [3.5" TFT LCD / PAR-LCD-S035](https://www.nxp.com/)
- カメラ: [OV7670](https://www.amazon.com/)
- 組み立て手順: [Instructions for putting together demo](https://community.nxp.com/)

## ソフトウェア構成

- [eIQ Neutron SDK](https://www.nxp.com/)
- [MCUXpresso IDE](https://www.nxp.com/)
- [Label CIFAR10 Image Demo on NXP App Code Hub](https://github.com/)

---

# 手順全体

大きな流れは以下の3ステップ。

1. モデルを学習する
2. TensorFlow LiteモデルをNeutron NPU向けに変換する
3. MCUXpressoプロジェクトへ組み込み、FRDM-MCXN947へ書き込む

---

# 1. Dataset Preparation

デモでは、リンゴとバナナを分類する2クラス分類モデルを例としている。

データセットは以下のように分割する。

- Training set: 80%
- Test set: 20%

[画像: Dataset構成](https://community.nxp.com/t5/image/serverpage/image-id/287100i7343891A8A61B97A/image-size/medium?px=400&v=v2)

---

# 2. モデル学習

画像データセットを使って分類モデルを作成する。

利用候補:

- [eIQ Model Creator](https://eiq.modelcat.ai/)
- [TensorFlow](https://ai.google.dev/)

最終的に、TensorFlow Lite形式の `.tflite` モデルを作成する。

例:

```bash
fruit_model.tflite
