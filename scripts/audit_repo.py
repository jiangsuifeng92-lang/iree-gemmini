#!/usr/bin/env python3
from pathlib import Path
import os, sys
root=Path(__file__).resolve().parents[1]
ignored={"build","G_arm_gemmini/build","G_arm_gemmini/runtime/lib","F_gemmini_backend/runtime/lib",".git"}
errors=[]
files=total=0
for current,dirs,names in os.walk(root,followlinks=False):
    base=Path(current)
    for name in list(dirs):
        p=base/name
        rel=str(p.relative_to(root))
        if rel in ignored or name=="__pycache__":
            dirs.remove(name)
        elif p.is_symlink():
            errors.append(f"symlink directory: {rel}")
            dirs.remove(name)
        elif name==".git":
            errors.append(f"nested Git metadata: {rel}")
            dirs.remove(name)
    for name in names:
        p=base/name
        rel=str(p.relative_to(root))
        if p.is_symlink(): errors.append(f"symlink file: {rel}")
        elif name==".git": errors.append(f"nested Git metadata: {rel}")
        elif p.is_file():
            files+=1
            size=p.stat().st_size
            total+=size
            if size>100*1024*1024: errors.append(f"Git-host size limit (>100 MiB): {rel}")
for listing in ["rtl/filelist.f","sim/dpi_sources.f"]:
    for line in (root/listing).read_text().splitlines():
        if not line or line.startswith("#"): continue
        if Path(line).is_absolute() or not (root/line).is_file():
            errors.append(f"invalid portable filelist entry: {line}")
for name in ["third_party/iree/compiler/CMakeLists.txt","third_party/iree/runtime/src/iree/vm/bytecode/dispatch.c",
             "third_party/llvm-project/llvm/CMakeLists.txt","third_party/riscv-isa-sim/fesvr/tsi.cc",
             "third_party/riscv-isa-sim/riscv/cfg.h","sim/csrc/SimTSI.cc",
             "rtl/TestDriver.v","rtl/TestHarness.sv","rtl/Rocket.sv",
             "rtl/arm9_compatiable_code.v","F_gemmini_backend/compiler/Transforms/GemminiGraphOptimization.cpp",
             "F_gemmini_backend/apps/mnist/gen/riscv_int8/cnn.mlir"]:
    if not (root/name).is_file(): errors.append("missing required source: "+name)
print(f"Git source inventory: {files} regular files; {total/1024**3:.3f} GiB")
if errors:
    print("\n".join(errors))
    sys.exit(1)
print("PASS: no source symlinks, nested gitlinks, missing filelist paths or >100 MiB files.")
