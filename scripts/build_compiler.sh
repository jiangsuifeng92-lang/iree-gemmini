#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../env.sh"
mkdir -p "$MNIST_ROOT/build/logs"
python3 "$MNIST_ROOT/scripts/generate_config.py"
cmake -G Ninja -S "$F_HOME" -B "$IREE_BUILD_ROOT" \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_BUILD_TYPE=Release \
  -DIREE_SOURCE_DIR="$IREE_SOURCE_DIR" -DIREE_BUNDLED_LLVM_DIR="$IREE_BUNDLED_LLVM_DIR" \
  -DLLVM_PARALLEL_LINK_JOBS=1 2>&1 | tee "$MNIST_ROOT/build/logs/compiler-configure.log"
# Cross Runtime schema generation needs a host flatcc executable.
cmake --build "$IREE_BUILD_ROOT" --parallel "$BUILD_JOBS" --target iree-flatcc-cli \
  2>&1 | tee "$MNIST_ROOT/build/logs/host-tools-build.log"
touch "$IREE_BUILD_ROOT/host-tools-ready"
cmake --build "$IREE_BUILD_ROOT" --parallel "$BUILD_JOBS" --target iree-compile iree-dump-module \
  2>&1 | tee "$MNIST_ROOT/build/logs/compiler-build.log"
"$IREE_COMPILE" --version
