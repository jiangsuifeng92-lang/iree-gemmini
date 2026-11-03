#!/usr/bin/env python3
import pathlib
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
if len(data) > 64 * 1024:
    raise SystemExit("host firmware is larger than its 64 KiB boot RAM")
data = data.ljust(64 * 1024, b"\0")
pathlib.Path(sys.argv[2]).write_text("".join(
    f"{int.from_bytes(data[i:i+4], 'little'):08x}\n"
    for i in range(0, len(data), 4)))
