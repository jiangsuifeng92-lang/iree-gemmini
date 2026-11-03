#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../env.sh"
upstream="$IREE_SOURCE_DIR"
src="$G_HOME/build/iree-arm-source"
out="$G_HOME/build/iree-arm"
headers="${ARM_NEWLIB_INCLUDE:-$G_HOME/toolchain/sysroot/usr/include/newlib}"
verification=${G_VM_BYTECODE_VERIFY:-0}
case "$verification" in 0|1) ;; *) echo "G_VM_BYTECODE_VERIFY must be 0 or 1" >&2; exit 2 ;; esac
if [ ! -f "$headers/stdlib.h" ]; then
  echo "Run scripts/prepare_arm_toolchain.sh first (or set ARM_NEWLIB_INCLUDE)." >&2
  exit 1
fi
mkdir -p "$out" "$G_HOME/runtime/lib/arm"
python3 "$G_HOME/scripts/stage_iree.py" "$upstream" "$src"
arm_flags="-include $G_HOME/include/baremetal_time.h -march=armv4 -marm -mfloat-abi=soft -Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -isystem $headers -DIREE_PLATFORM_GENERIC=1 -DIREE_FILE_IO_ENABLE=0 -DIREE_TIME_NOW_FN=\"{ return 0; }\" -DIREE_HAL_TOPOLOGY_MAX_DEVICE_COUNT=1 -DIREE_VM_BYTECODE_VERIFICATION_ENABLE=$verification"
cmake -G Ninja -S "$src" -B "$out" \
  -DCMAKE_TOOLCHAIN_FILE="$G_HOME/cmake/armv4.cmake" \
  -DARM_NEWLIB_INCLUDE="$headers" \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_C_FLAGS="$arm_flags" -DCMAKE_CXX_FLAGS="$arm_flags" \
  -DIREE_HOST_BIN_DIR="$IREE_HOST_BIN_DIR" \
  -DIREE_BUILD_COMPILER=OFF -DIREE_BUILD_TESTS=OFF -DIREE_BUILD_SAMPLES=OFF \
  -DIREE_BUILD_BINDINGS_PYTHON=OFF -DIREE_ENABLE_LIBBACKTRACE=OFF \
  -DIREE_ENABLE_CPUINFO=OFF -DIREE_ERROR_ON_MISSING_SUBMODULES=OFF \
  -DIREE_HAL_DRIVER_DEFAULTS=OFF -DIREE_HAL_DRIVER_LOCAL_SYNC=ON \
  -DIREE_HAL_EXECUTABLE_LOADER_DEFAULTS=OFF \
  -DIREE_HAL_EXECUTABLE_PLUGIN_DEFAULTS=OFF \
  > "$out/configure.log" 2>&1 || { tail -50 "$out/configure.log"; exit 1; }
cmake --build "$out" --parallel "${G_BUILD_JOBS:-4}" --target \
  runtime/src/iree/runtime/libiree_runtime_impl.a \
  runtime/src/iree/hal/drivers/local_sync/libiree_hal_drivers_local_sync_sync_driver.a \
  runtime/src/iree/hal/local/loaders/libiree_hal_local_loaders_static_library_loader.a \
  > "$out/build.log" 2>&1 || { tail -60 "$out/build.log"; exit 1; }
python3 "$G_HOME/scripts/merge_runtime.py" "$out" \
  "$G_HOME/runtime/lib/arm/libiree_runtime.a"
echo "armv4-softfloat-topology1-verify$verification" > "$G_HOME/runtime/lib/arm/profile.txt"
echo "Built Armv4 IREE runtime (matching F sources)."
