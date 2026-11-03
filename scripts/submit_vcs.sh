#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/../env.sh"
queue=${1:-short}
mkdir -p "$MNIST_ROOT/build/logs"
bsub -q "$queue" -n "${VCS_CPUS:-4}" -R "span[hosts=1]" \
  -o "$MNIST_ROOT/build/logs/lsf-vcs-%J.log" /usr/bin/env bash "$MNIST_ROOT/scripts/run_vcs.sh"
