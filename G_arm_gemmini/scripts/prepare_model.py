#!/usr/bin/env python3
from pathlib import Path
import re
import sys
out = Path(sys.argv[1])
header = out / "model_lib.h"
queries = re.findall(r"\b([A-Za-z_]\w*_library_query)\s*\(", header.read_text())
queries = list(dict.fromkeys(queries))
if not queries:
    raise SystemExit("Compiler did not produce any native library query functions")
(out / "model_registry.h").write_text(
    '#include "model_lib.h"\n'
    f'#define G_LIBRARY_COUNT {len(queries)}u\n'
    'static const iree_hal_executable_library_query_fn_t g_library_queries[] = {\n'
    + ''.join('  ' + q + ',\n' for q in queries) + '};\n')
data = (out / "model.vmfb").read_bytes()
(out / "model_vmfb.h").write_text(
    '#include <stdint.h>\n#include <stddef.h>\n'
    '__attribute__((section(".model"), aligned(64)))\n'
    'static const uint8_t model_vmfb_data[] = {\n'
    + ''.join('  ' + ','.join(str(b) for b in data[i:i+16]) + ',\n'
              for i in range(0, len(data), 16))
    + '};\nstatic const size_t model_vmfb_size = sizeof(model_vmfb_data);\n')
