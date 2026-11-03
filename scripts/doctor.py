#!/usr/bin/env python3
from pathlib import Path
import os, shutil, subprocess, sys
root=Path(__file__).resolve().parents[1]
checks=[("python3","Python 3.8+"),("cmake","CMake 3.26+"),("ninja","Ninja"),
        (os.environ.get("ARM_CC","clang-19"),"Arm Clang"),
        (os.environ.get("ARM_CXX","clang++-19"),"Arm Clang++"),
        (str(root/"build/host-toolchain/bin/ld.lld") if (root/"build/host-toolchain/bin/clang").exists() else "ld.lld-19","LLD"),
        (os.environ.get("LLVM_AR","llvm-ar-19"),"LLVM archiver"),
        (os.environ.get("ARM_OBJCOPY","llvm-objcopy-19"),"LLVM objcopy"),
        ("g++","host C++17 compiler"),("make","GNU Make"),("dtc","device-tree compiler"),
        ("vcs","licensed Synopsys VCS")]
missing=[]
for program,label in checks:
    found=shutil.which(program)
    print(f"{'OK' if found else 'MISSING':7} {label}: {found or program}")
    if not found: missing.append(program)
gcc=root/"F_gemmini_backend/toolchain/riscv/bin/riscv64-unknown-elf-gcc"
env=dict(os.environ)
env["LD_LIBRARY_PATH"]=str(root/"F_gemmini_backend/toolchain/riscv/lib")+":"+env.get("LD_LIBRARY_PATH","")
try:
    subprocess.run([str(gcc),"--version"],env=env,check=True,stdout=subprocess.DEVNULL)
    print("OK      bundled RISC-V GCC")
except (OSError,subprocess.CalledProcessError):
    missing.append("bundled RISC-V GCC")
print("No automatic installation. See README.md for prerequisites and overrides.")
sys.exit(1 if missing else 0)
