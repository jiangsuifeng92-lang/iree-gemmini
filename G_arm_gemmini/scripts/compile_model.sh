#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../env.sh"
model=${1:-matmul}
out="$G_HOME/build/model/$model"
case "$model" in
  matmul) input="$G_HOME/models/matmul.mlir" ;;
  mnist) input="$F_HOME/apps/mnist/gen/riscv_int8/cnn.mlir" ;;
  *) echo "usage: $0 [matmul|mnist]" >&2; exit 2 ;;
esac
mkdir -p "$out"
compiler="${IREE_COMPILE:-$F_HOME/toolchain/build/third_party/iree/tools/iree-compile}"
source_map=${G_MODEL_SOURCE_MAP:-0}
case "$source_map" in 0|1) ;; *) echo "G_MODEL_SOURCE_MAP must be 0 or 1" >&2; exit 2 ;; esac
strip_source_map=true
[ "$source_map" = 1 ] && strip_source_map=false
export H_GEMMINI_GRAPH_REPORT="$out/gemmini_graph.jsonl"
: > "$H_GEMMINI_GRAPH_REPORT"
export H_GEMMINI_EPILOGUE_REPORT="$out/gemmini_epilogues.jsonl"
: > "$H_GEMMINI_EPILOGUE_REPORT"
export H_GEMMINI_PLAN_REPORT="$out/gemmini_plans.jsonl"
: > "$H_GEMMINI_PLAN_REPORT"
"$compiler" "$input" --iree-hal-target-device=local \
  --iree-hal-local-target-device-backends=gemmini \
  --iree-vm-bytecode-module-strip-source-map="$strip_source_map" \
  --gemmini-static-library-output-path="$out/model_lib.o" \
  -o "$out/model.vmfb"
python3 "$G_HOME/scripts/prepare_model.py" "$out"
if [ "$model" = mnist ]; then
  cp "$F_HOME/apps/mnist/gen/riscv_int8/cnn_io.h" "$out/cnn_io.h"
fi
echo "VMFB for Arm and native RISC-V dispatch library: $out"
