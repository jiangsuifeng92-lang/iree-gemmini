#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../env.sh"
out="$G_HOME/build/iree-riscv"
mkdir -p "$out" "$F_HOME/runtime/lib/riscv64"
flags="-include $G_HOME/include/baremetal_time.h "
flags+='-march=rv64gc -mabi=lp64d -mcmodel=medany -Os -ffunction-sections -fdata-sections -DIREE_PLATFORM_GENERIC=1 -DIREE_FILE_IO_ENABLE=0 -DIREE_ALLOCATOR_SYSTEM_CTL=iree_allocator_libc_ctl -DIREE_SYNCHRONIZATION_DISABLE_UNSAFE=1 -DIREE_TIME_NOW_FN="{ return 0; }" -DIREE_DEVICE_SIZE_T=uint64_t -DPRIdsz=PRIu64'
cmake -G Ninja -S "$IREE_SOURCE_DIR" -B "$out" \
  -DCMAKE_TOOLCHAIN_FILE="$G_HOME/cmake/riscv64.cmake" -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" -DIREE_HOST_BIN_DIR="$IREE_HOST_BIN_DIR" \
  -DIREE_BUILD_COMPILER=OFF -DIREE_BUILD_TESTS=OFF -DIREE_BUILD_SAMPLES=OFF \
  -DIREE_BUILD_PYTHON_BINDINGS=OFF -DIREE_BUILD_BINDINGS_PYTHON=OFF \
  -DIREE_ENABLE_LIBBACKTRACE=OFF -DIREE_ENABLE_CPUINFO=OFF \
  -DIREE_ERROR_ON_MISSING_SUBMODULES=OFF -DIREE_ENABLE_THIN_ARCHIVES=OFF \
  -DIREE_HAL_DRIVER_DEFAULTS=OFF -DIREE_HAL_DRIVER_LOCAL_SYNC=ON \
  -DIREE_HAL_EXECUTABLE_LOADER_DEFAULTS=OFF -DIREE_HAL_EXECUTABLE_PLUGIN_DEFAULTS=OFF \
  > "$out/configure.log" 2>&1 || { tail -60 "$out/configure.log"; exit 1; }
cmake --build "$out" --parallel "$BUILD_JOBS" --target \
  runtime/src/iree/hal/local/libiree_hal_local_executable_environment.a \
  runtime/src/iree/hal/local/libiree_hal_local_executable_library_util.a \
  > "$out/build.log" 2>&1 || { tail -60 "$out/build.log"; exit 1; }
python3 "$G_HOME/scripts/merge_runtime.py" "$out" "$F_HOME/runtime/lib/riscv64/libiree_runtime.a"
echo "Rebuilt RISC-V Runtime from vendored IREE source."
