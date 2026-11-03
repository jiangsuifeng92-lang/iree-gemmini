#!/usr/bin/env bash
MNIST_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export MNIST_ROOT
export H_HOME="$MNIST_ROOT"
export F_HOME="$MNIST_ROOT/F_gemmini_backend"
export G_HOME="$MNIST_ROOT/G_arm_gemmini"
export IREE_SOURCE_DIR="$MNIST_ROOT/third_party/iree"
export IREE_BUNDLED_LLVM_DIR="$MNIST_ROOT/third_party/llvm-project"
export IREE_BUILD_ROOT="$MNIST_ROOT/build/compiler"
export IREE_COMPILE="$IREE_BUILD_ROOT/third_party/iree/tools/iree-compile"
export IREE_HOST_BIN_DIR="$IREE_BUILD_ROOT/third_party/iree/tools"
export BUILD="$IREE_BUILD_ROOT"
export BUILD_JOBS=${BUILD_JOBS:-4}
export G_BUILD_JOBS="$BUILD_JOBS"
# Optional offline LLVM build; all generated tools remain under build/.
if [ -x "$MNIST_ROOT/build/host-toolchain/bin/clang" ]; then
  tool_bin="$MNIST_ROOT/build/host-toolchain/bin"
  export PATH="$tool_bin:$PATH"
  export CC=${CC:-$tool_bin/clang}
  export CXX=${CXX:-$tool_bin/clang++}
  export ARM_CC=${ARM_CC:-$tool_bin/clang}
  export ARM_CXX=${ARM_CXX:-$tool_bin/clang++}
  export ARM_OBJCOPY=${ARM_OBJCOPY:-$tool_bin/llvm-objcopy}
  export LLVM_AR=${LLVM_AR:-$tool_bin/llvm-ar}
  export LLVM_RANLIB=${LLVM_RANLIB:-$tool_bin/llvm-ranlib}
else
  export CC=${CC:-clang-19}
  export CXX=${CXX:-clang++-19}
  export ARM_CC=${ARM_CC:-clang-19}
  export ARM_CXX=${ARM_CXX:-clang++-19}
  export ARM_OBJCOPY=${ARM_OBJCOPY:-llvm-objcopy-19}
  export LLVM_AR=${LLVM_AR:-llvm-ar-19}
  export LLVM_RANLIB=${LLVM_RANLIB:-llvm-ranlib-19}
fi
export RISCV=${RISCV:-$F_HOME/toolchain/riscv}
export RISCV_CC=${RISCV_CC:-$RISCV/bin/riscv64-unknown-elf-gcc}
export ARM_SYSROOT=${ARM_SYSROOT:-$G_HOME/toolchain/sysroot}
export PATH="$RISCV/bin:$PATH"
export LD_LIBRARY_PATH="$RISCV/lib:$IREE_BUILD_ROOT/third_party/iree/lib:${LD_LIBRARY_PATH:-}"
