#!/usr/bin/env python3
"""Merge component libraries with an MRI script, keeping link order simple."""
from pathlib import Path
import subprocess
import os
import sys
root, output = map(lambda x: Path(x).resolve(), sys.argv[1:])
archives = sorted(root.rglob("*.a"))
if not archives:
    raise SystemExit("No runtime archives found")
script = "CREATE " + str(output) + "\n"
script += "".join("ADDLIB " + str(p) + "\n" for p in archives)
script += "SAVE\nEND\n"
subprocess.run([os.environ.get("LLVM_AR", "llvm-ar-19"), "-M"], input=script, text=True, check=True)
