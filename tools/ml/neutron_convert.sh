#!/usr/bin/env bash
# neutron-converter を Docker(amd64エミュ)で実行するラッパー。
#
# 本機 Mac の neutron-converter は Linux x86 バイナリのため、Docker の
# linux/amd64 エミュレーションで実行する（M2 Max でも QEMU 経由で動作確認済み）。
#
# 使い方:
#   tools/ml/neutron_convert.sh <input.tflite> [output.tflite] [target]
# 例:
#   tools/ml/neutron_convert.sh build/terra_guard_int8.tflite \
#       build/terra_guard_int8_mcxn94x.tflite mcxn94x
#
# 出力: 変換済み .tflite と、--dump-header-file-output による C ヘッダ .h
# SPDX-License-Identifier: BSD-3-Clause
set -euo pipefail

# eIQ Neutron SDK の場所（リポジトリ相対。各自 NXP からダウンロード→ sdk/ 配下に展開）
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDK_DIR="${NEUTRON_SDK_DIR:-$REPO_ROOT/sdk/eiq-neutron-sdk-linux-3.1.3}"
IMAGE="${NEUTRON_DOCKER_IMAGE:-debian:bookworm-slim}"

INPUT="${1:?usage: neutron_convert.sh <input.tflite> [output.tflite] [target]}"
OUTPUT="${2:-${INPUT%.tflite}_converted.tflite}"
TARGET="${3:-mcxn94x}"

if [[ ! -x "$SDK_DIR/bin/neutron-converter" ]]; then
  echo "ERROR: neutron-converter が見つからない: $SDK_DIR/bin/neutron-converter" >&2
  echo "       NEUTRON_SDK_DIR を設定するか、sdk/ に eIQ Neutron SDK を展開してください。" >&2
  exit 1
fi

# 入出力は同一ディレクトリ（input の親）をマウントして相対パスで渡す
IN_DIR="$(cd "$(dirname "$INPUT")" && pwd)"
IN_BASE="$(basename "$INPUT")"
OUT_BASE="$(basename "$OUTPUT")"

echo "[neutron-convert] target=$TARGET  input=$IN_BASE  output=$OUT_BASE  (via docker amd64)"
docker run --rm --platform linux/amd64 \
  -v "$SDK_DIR":/sdk:ro \
  -v "$IN_DIR":/work \
  -w /work \
  "$IMAGE" \
  bash -lc "export LD_LIBRARY_PATH=/sdk/lib:\$LD_LIBRARY_PATH; \
    /sdk/bin/neutron-converter \
      --input '/work/$IN_BASE' \
      --output '/work/$OUT_BASE' \
      --target '$TARGET' \
      --dump-header-file-output"

echo "[neutron-convert] done -> $IN_DIR/$OUT_BASE (+ .h)"
