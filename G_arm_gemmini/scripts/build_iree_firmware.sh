#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../env.sh"
model=${1:-matmul}
verification=${G_VM_BYTECODE_VERIFY:-0}
case "$verification" in 0|1) ;; *) echo "G_VM_BYTECODE_VERIFY must be 0 or 1" >&2; exit 2 ;; esac
case "$model" in matmul|mnist) ;; *) echo "usage: $0 [matmul|mnist]" >&2; exit 2 ;; esac
"$G_HOME/scripts/compile_model.sh" "$model"
runtime_archive="$G_HOME/runtime/lib/arm/libiree_runtime.a"
runtime_rebuild=0
if [ ! -f "$runtime_archive" ] || \
   [ "$(cat "$G_HOME/runtime/lib/arm/profile.txt" 2>/dev/null || true)" != "armv4-softfloat-topology1-verify$verification" ]; then
  runtime_rebuild=1
fi
for runtime_source in \
  "$G_HOME/scripts/stage_iree.py" \
  "$G_HOME/runtime/armv4/hal/buffer.c" \
  "$G_HOME/runtime/armv4/hal/inline_command_buffer.c"; do
  if [ -f "$runtime_source" ] && [ "$runtime_source" -nt "$runtime_archive" ]; then
    runtime_rebuild=1
  fi
done
if [ "$runtime_rebuild" = 1 ]; then
  "$G_HOME/scripts/build_arm_runtime.sh"
fi
out="$G_HOME/build/iree-firmware/$model"
generated="$G_HOME/build/model/$model"
mkdir -p "$out/host" "$out/npu"
sysroot="${ARM_SYSROOT:-$G_HOME/toolchain/sysroot}"
headers="${ARM_NEWLIB_INCLUDE:-$sysroot/usr/include/newlib}"
gcc_library=$(find "$sysroot/usr/lib/gcc/arm-none-eabi" -maxdepth 2 -name libgcc.a | sort | head -1)
if [ -z "$gcc_library" ] || [ ! -f "$sysroot/usr/lib/arm-none-eabi/newlib/libc.a" ]; then
  echo "Run scripts/prepare_arm_toolchain.sh to obtain Arm newlib/libgcc." >&2; exit 1
fi
flags=(-DIREE_PLATFORM_GENERIC=1 -DIREE_FILE_IO_ENABLE=0 \
       -DIREE_ALLOCATOR_SYSTEM_CTL=iree_allocator_libc_ctl \
       -DIREE_SYNCHRONIZATION_DISABLE_UNSAFE=1 '-DIREE_TIME_NOW_FN={ return 0; }')
model_flags=()
[ "$model" = mnist ] && model_flags+=(-DG_MODEL_MNIST=1)
if [ "${G_HAL_MAPPING_DIAGNOSTIC:-0}" = 1 ]; then
  model_flags+=(-DG_HAL_MAPPING_DIAGNOSTIC=1)
fi
"$ARM_CC" --target=arm-none-eabi -march=armv4 -marm -mfloat-abi=soft \
  -Os -g -ffreestanding -fno-builtin -fno-strict-aliasing \
  -ffunction-sections -fdata-sections -Wno-pointer-sign \
  -fno-unwind-tables -fno-asynchronous-unwind-tables -std=c11 \
  -nostdlib -fuse-ld=lld "${flags[@]}" "${model_flags[@]}" \
  -DIREE_HAL_TOPOLOGY_MAX_DEVICE_COUNT=1 \
  -DIREE_VM_BYTECODE_VERIFICATION_ENABLE="$verification" \
  -isystem "$headers" -I"$G_HOME/build/iree-arm-source/runtime/src" \
  -I"$G_HOME/build/iree-arm/runtime/src" -I"$F_HOME/../include" -I"$F_HOME/runtime/include" \
  -I"$G_HOME/include" -I"$generated" \
  "$G_HOME/firmware/host/start.S" "$G_HOME/firmware/host/iree_main.c" \
  "$G_HOME/firmware/host/npu_device.c" "$G_HOME/firmware/host/transport.c" \
  "$G_HOME/firmware/host/syscalls.c" \
  -Wl,-T,"$G_HOME/firmware/host/runtime.ld",--gc-sections \
  -Wl,-Map,"$out/host/host.map" \
  -L"$sysroot/usr/lib/arm-none-eabi/newlib" -L"$(dirname "$gcc_library")" \
  -Wl,--start-group "$G_HOME/runtime/lib/arm/libiree_runtime.a" -lc -lm -lgcc \
  -Wl,--end-group -o "$out/host/host.elf"
"$ARM_OBJCOPY" -O binary --only-section=.boot "$out/host/host.elf" "$out/host/host.bin"
python3 "$G_HOME/scripts/bin_to_hex.py" "$out/host/host.bin" "$out/host/host.hex"

"$RISCV_CC" -march=rv64gc -mabi=lp64d -mcmodel=medany -O2 -g -std=gnu11 \
  -ffreestanding -fno-common -fno-strict-aliasing -fno-builtin-printf \
  -fno-tree-loop-distribute-patterns \
  -ffunction-sections -fdata-sections -nostdlib -nostartfiles -static \
  -DPREALLOCATE=1 -DMULTITHREAD=1 -DBAREMETAL=1 -DGEMMINI=1 \
  -DG_ENABLE_IREE_DISPATCH=1 -DBAREMETAL_HEAP_SIZE=8388608 \
  "${flags[@]}" -DIREE_DEVICE_SIZE_T=uint64_t -DPRIdsz=PRIu64 \
  -I"$F_HOME/../include" -I"$F_HOME/runtime/include" -I"$G_HOME/include" -I"$generated" \
  -I"$F_HOME/runtime/support" -I"$F_HOME/runtime/gemmini" \
  -I"$F_HOME/runtime/gemmini/include" \
  "$G_HOME/firmware/npu/main.c" "$G_HOME/firmware/npu/dispatch.c" \
  "$F_HOME/kernels/gemmini_matmul.c" "$F_HOME/runtime/crt.S" \
  "$F_HOME/runtime/support/syscalls.c" "$F_HOME/runtime/src/baremetal_libc.c" \
  "$F_HOME/runtime/src/baremetal_alloc.c" "$generated/model_lib.o" \
  -T "$G_HOME/firmware/npu/link.ld" -Wl,--gc-sections \
  -Wl,-Map,"$out/npu/npu.map" \
  "$F_HOME/runtime/lib/riscv64/libiree_runtime.a" \
  -o "$out/npu/npu.elf" -lm -lgcc
python3 "$G_HOME/scripts/make_load_image.py" "$out/npu/npu.elf" \
  "$out/host/host.elf" "$out/loadmem.elf"
echo "Built independent Arm IREE firmware and Rocket device firmware: $out"
