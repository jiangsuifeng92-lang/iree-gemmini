#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../env.sh"
VCS=${VCS:-vcs}
command -v "$VCS" >/dev/null || { echo "VCS is not on PATH. Load the server's VCS module first." >&2; exit 1; }
"$MNIST_ROOT/scripts/build_dpi.sh"
python3 "$MNIST_ROOT/scripts/make_filelists.py"
out="$MNIST_ROOT/build/vcs"
mkdir -p "$out"
name=simv
extra=()
if [ "${WAVE:-0}" = 1 ]; then
  name=simv-debug
  extra=(+define+DEBUG -debug_access+all -kdb -lca)
fi
cflags="-O3 -std=c++17 -I$MNIST_ROOT/sim/csrc -I$MNIST_ROOT/third_party/riscv-isa-sim -I$MNIST_ROOT/build/dpi/spike -I$MNIST_ROOT/third_party/DRAMSim2"
ldflags="-Wl,--start-group $MNIST_ROOT/build/dpi/spike/libfesvr.a $MNIST_ROOT/build/dpi/dramsim/libdramsim.a -Wl,--end-group -pthread"
mapfile -t csrc < "$out/dpi.f"
(cd "$out"
 "$VCS" -full64 -sverilog +rad -j"$BUILD_JOBS" -timescale=1ns/10ps -top TestDriver \
   +define+VCS +define+MODEL=TestHarness +define+CLOCK_PERIOD=1.0 \
   +define+RESET_DELAY=777.7 '+define+PRINTF_COND=TestDriver.printf_cond' \
   '+define+STOP_COND=!TestDriver.reset' \
   +define+RANDOMIZE_MEM_INIT +define+RANDOMIZE_REG_INIT \
   +define+RANDOMIZE_GARBAGE_ASSIGN +define+RANDOMIZE_INVALID_ASSIGN \
   "+incdir+$MNIST_ROOT/rtl" -f "$out/rtl.f" \
   -CFLAGS "$cflags" -LDFLAGS "$ldflags" "${csrc[@]}" "${extra[@]}" \
   -o "$out/$name" -Mdir="$out/csrc-$name") 2>&1 | tee "$MNIST_ROOT/build/logs/vcs-build.log"
