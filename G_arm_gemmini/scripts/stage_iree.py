#!/usr/bin/env python3
"""Copy vendored IREE sources, with local ARMv4 compatibility overrides."""
from pathlib import Path
import os
import shutil
import sys
source, destination = (Path(p).resolve() for p in sys.argv[1:])
destination.mkdir(parents=True, exist_ok=True)
for entry in source.iterdir():
    if entry.name in ("runtime", ".git"):
        continue
    target = destination / entry.name
    if not target.exists():
        shutil.copytree(entry, target, symlinks=False) if entry.is_dir() else shutil.copy2(entry, target)
for directory, dirs, files in os.walk(source / "runtime"):
    dirs[:] = [d for d in dirs if d not in (".git", "__pycache__")]
    relative = Path(directory).relative_to(source)
    (destination / relative).mkdir(parents=True, exist_ok=True)
    for filename in files:
        target = destination / relative / filename
        if not target.exists():
            shutil.copy2(Path(directory) / filename, target)
for name in ("mutex", "notification"):
    relative = Path(f"runtime/src/iree/base/threading/{name}.c")
    contents = (source / relative).read_text()
    # Spin hints do not provide synchronization; this platform is single-thread.
    # ARMv4 has no YIELD instruction. A compiler barrier preserves the loop.
    if 'asm volatile("yield");' not in contents:
        raise SystemExit("IREE spin hint changed; review ARMv4 compatibility patch")
    target = destination / relative
    if target.is_symlink():
        target.unlink()
    target.write_text(contents.replace('asm volatile("yield");',
        '__asm__ volatile("" ::: "memory"); /* G: ARMv4 has no YIELD */'))
relative = Path("runtime/src/iree/base/internal/fpu_state.c")
target = destination / relative
if target.is_symlink():
    target.unlink()
target.write_text((source / relative).read_text().replace(
    "defined(IREE_ARCH_ARM_32)",
    "(defined(IREE_ARCH_ARM_32) && !defined(__SOFTFP__))"))
relative = Path("runtime/src/iree/hal/topology.h")
target = destination / relative
if target.is_symlink():
    target.unlink()
target.write_text((source / relative).read_text().replace(
    "#define IREE_HAL_TOPOLOGY_MAX_DEVICE_COUNT 32",
    "#ifndef IREE_HAL_TOPOLOGY_MAX_DEVICE_COUNT\n"
    "#define IREE_HAL_TOPOLOGY_MAX_DEVICE_COUNT 32\n#endif"))
# Keep HAL map diagnostics in G; this source is otherwise shared with F.
relative = Path("runtime/src/iree/hal/local/inline_command_buffer.c")
target = destination / relative
if target.is_symlink():
    target.unlink()
override = destination.parent.parent / "runtime/armv4/hal/inline_command_buffer.c"
target.write_text(override.read_text())
relative = Path("runtime/src/iree/hal/buffer.c")
target = destination / relative
if target.is_symlink():
    target.unlink()
override = destination.parent.parent / "runtime/armv4/hal/buffer.c"
target.write_text(override.read_text())
# Include native reference tags in failures while bringing up the ARMv4 VM.
relative = Path("runtime/src/iree/vm/ref.h")
target = destination / relative
if target.is_symlink():
    target.unlink()
contents = (source / relative).read_text()
old = '''  return IREE_LIKELY(ref.type == type)
             ? iree_ok_status()
             : iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                ref.type == IREE_VM_REF_TYPE_NULL
                                    ? "ref is null"
                                    : "ref type mismatch");'''
new = '''  if (IREE_LIKELY(ref.type == type)) return iree_ok_status();
  return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
      "ref type mismatch actual=%08x expected=%08x ptr=%08x",
      (unsigned)ref.type, (unsigned)type, (unsigned)(uintptr_t)ref.ptr);'''
if old not in contents:
    raise SystemExit("IREE ref check changed; review diagnostic patch")
target.write_text(contents.replace(old, new))
relative = Path("runtime/src/iree/vm/bytecode/dispatch.c")
target = destination / relative
if target.is_symlink():
    target.unlink()
contents = (source / relative).read_text()
old = '''    return iree_status_annotate(call_status,
                                iree_make_cstring_view("while calling import"));'''
new = '''    iree_string_view_t name = iree_vm_function_name(&call.function);
    return iree_status_annotate_f(call_status, "while calling import %.*s",
                                 (int)name.size, name.data);'''
if old not in contents:
    raise SystemExit("IREE import call changed; review diagnostic patch")
target.write_text(contents.replace(old, new))
