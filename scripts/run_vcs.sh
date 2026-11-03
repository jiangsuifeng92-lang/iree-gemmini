#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../env.sh"
"$MNIST_ROOT/scripts/build_vcs.sh"
out="$G_HOME/build/iree-firmware/mnist"
for file in "$out/host/host.hex" "$out/npu/npu.elf" "$out/loadmem.elf"; do
  [ -f "$file" ] || { echo "Missing $file; run make firmware first." >&2; exit 1; }
done
sim="$MNIST_ROOT/build/vcs/simv"
log="$MNIST_ROOT/build/logs/vcs-mnist.log"
args=(+permissive -l "$log"
  "+g_host_mem_hex=$out/host/host.hex" "+loadmem=$out/loadmem.elf"
  "+max-cycles=${MAX_CYCLES:-1000000000}"
  "+dramsim_ini_dir=$MNIST_ROOT/third_party/DRAMSim2")
if [ "${WAVE:-0}" = 1 ]; then
  sim="$MNIST_ROOT/build/vcs/simv-debug"
  args+=("+vcdplusfile=$MNIST_ROOT/build/vcs/mnist.vpd")
fi
args+=(+permissive-off "$out/npu/npu.elf")
(cd "$MNIST_ROOT/build/vcs"; "$sim" "${args[@]}") 2>&1 | tee "$MNIST_ROOT/build/logs/vcs-mnist-console.log"
# DPI UART output can bypass VCS's -l file. Capture both outputs.
grep -Fq 'MNIST PASS (Arm IREE / Rocket / Gemmini)' \
  "$log" "$MNIST_ROOT/build/logs/vcs-mnist-console.log"
echo "MNIST PASS. Logs: $log and $MNIST_ROOT/build/logs/vcs-mnist-console.log"
