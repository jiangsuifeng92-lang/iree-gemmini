#!/usr/bin/env python3
"""DDR preload ELF: combine Rocket + Arm DDR PT_LOAD segments.

This image is only for +loadmem. The simulator program remains npu.elf (HTIF
symbols); the Arm-local .boot is loaded separately through HOST_MEM_HEX.
"""
from pathlib import Path
import struct
import sys

def read_loads(path):
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[5] != 1:
        raise ValueError("Expected little endian ELF")
    if data[4] == 1:
        h = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
        entry, phoff, phsize, phcount = h[4], h[5], h[9], h[10]
        fmt = "<IIIIIIII"
    elif data[4] == 2:
        h = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
        entry, phoff, phsize, phcount = h[4], h[5], h[9], h[10]
        fmt = "<IIQQQQQQ"
    else:
        raise ValueError("Unsupported ELF class")
    loads = []
    for i in range(phcount):
        p = struct.unpack_from(fmt, data, phoff + i * phsize)
        if p[0] != 1:
            continue
        if data[4] == 1:
            _, offset, va, pa, filesz, memsz, flags, align = p
        else:
            _, flags, offset, va, pa, filesz, memsz, align = p
        loads.append((pa, memsz, flags, data[offset:offset + filesz]))
    return entry, loads

npu, host, destination = sys.argv[1:]
entry, loads = read_loads(npu)
_, host_loads = read_loads(host)
loads += [p for p in host_loads if p[0] >= 0x88000000]
loads.sort(key=lambda p: p[0])
last_end = 0x80000000
for address, memsz, flags, payload in loads:
    if address < last_end or address + memsz > 0x90000000:
        raise ValueError("Overlapping or out-of-DDR load segment")
    last_end = address + memsz
ident = b"\x7fELF\x02\x01\x01" + bytes(9)
header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 243, 1, entry,
                     64, 0, 0, 64, 56, len(loads), 0, 0, 0)
image = bytearray(header + bytes(56 * len(loads)))
for index, (address, memsz, flags, payload) in enumerate(loads):
    offset = ((len(image) + 4095) & ~4095) + (address & 4095)
    image.extend(bytes(offset - len(image)))
    struct.pack_into("<IIQQQQQQ", image, 64 + index * 56, 1, flags, offset,
                     address, address, len(payload), memsz, 4096)
    image.extend(payload)
# FESVR load_elf requires a section string table even for a preload-only image.
strings = b"\0.shstrtab\0"
strings_offset = len(image)
image.extend(strings)
section_offset = (len(image) + 7) & ~7
image.extend(bytes(section_offset - len(image)))
image.extend(bytes(64)) # SHT_NULL
image.extend(struct.pack("<IIQQQQIIQQ", 1, 3, 0, 0, strings_offset,
                         len(strings), 0, 0, 1, 0))
struct.pack_into("<Q", image, 40, section_offset) # e_shoff
struct.pack_into("<HHH", image, 58, 64, 2, 1)   # shentsize/shnum/shstrndx
Path(destination).write_bytes(image)
