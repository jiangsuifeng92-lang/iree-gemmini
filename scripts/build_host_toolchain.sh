#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/host-toolchain"
mkdir -p "$root/build/logs"
cmake -G Ninja -S "$root/third_party/llvm-project/llvm" -B "$out" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="${BOOTSTRAP_CC:-gcc}" \
  -DCMAKE_CXX_COMPILER="${BOOTSTRAP_CXX:-g++}" \
  '-DLLVM_ENABLE_PROJECTS=clang;lld' '-DLLVM_TARGETS_TO_BUILD=ARM;RISCV;X86' \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF -DCLANG_INCLUDE_TESTS=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_PARALLEL_LINK_JOBS=1 \
  2>&1 | tee "$root/build/logs/host-toolchain-configure.log"
cmake --build "$out" --parallel "${BUILD_JOBS:-4}" \
  --target clang lld llvm-ar llvm-ranlib llvm-objcopy \
  2>&1 | tee "$root/build/logs/host-toolchain-build.log"
echo "Offline LLVM tools built. Root env.sh selects these tools automatically."
