#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../env.sh"
spike="$MNIST_ROOT/build/dpi/spike"
mkdir -p "$spike" "$MNIST_ROOT/build/logs"
if [ ! -f "$spike/Makefile" ]; then
  (cd "$spike"
   "$MNIST_ROOT/third_party/riscv-isa-sim/configure" \
     --prefix="$MNIST_ROOT/build/dpi/install" --without-boost --without-boost-asio --without-boost-regex \
     CC="${DPI_CC:-gcc}" CXX="${DPI_CXX:-g++}" \
     CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC -std=c++17") \
     > "$MNIST_ROOT/build/logs/fesvr-configure.log" 2>&1 || {
       tail -60 "$MNIST_ROOT/build/logs/fesvr-configure.log"; exit 1; }
fi
make -C "$spike" -j"$BUILD_JOBS" libfesvr.a > "$MNIST_ROOT/build/logs/fesvr-build.log" 2>&1 || {
  tail -60 "$MNIST_ROOT/build/logs/fesvr-build.log"; exit 1; }
cmake -G Ninja -S "$MNIST_ROOT/sim" -B "$MNIST_ROOT/build/dpi/dramsim" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="${DPI_CXX:-g++}" \
  > "$MNIST_ROOT/build/logs/dramsim-configure.log" 2>&1
cmake --build "$MNIST_ROOT/build/dpi/dramsim" --parallel "$BUILD_JOBS" \
  > "$MNIST_ROOT/build/logs/dramsim-build.log" 2>&1 || { tail -60 "$MNIST_ROOT/build/logs/dramsim-build.log"; exit 1; }
echo "Built complete FESVR (including tsi.cc) and DRAMSim2 on this host."
