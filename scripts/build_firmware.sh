#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../env.sh"
"$MNIST_ROOT/scripts/build_compiler.sh"
"$MNIST_ROOT/scripts/build_riscv_runtime.sh"
"$G_HOME/scripts/build_iree_firmware.sh" mnist 2>&1 | tee "$MNIST_ROOT/build/logs/firmware-build.log"
"$IREE_HOST_BIN_DIR/iree-dump-module" "$G_HOME/build/model/mnist/model.vmfb" > "$G_HOME/build/model/mnist/model.vmfb.txt"
"$RISCV/bin/riscv64-unknown-elf-objdump" -dr "$G_HOME/build/model/mnist/model_lib.o" > "$G_HOME/build/model/mnist/model_lib.disasm.txt"
