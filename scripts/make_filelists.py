#!/usr/bin/env python3
from pathlib import Path
root = Path(__file__).resolve().parents[1]
out = root / "build/vcs"
out.mkdir(parents=True, exist_ok=True)
for name in ("rtl", "dpi"):
    listing = root / ("rtl/filelist.f" if name == "rtl" else "sim/dpi_sources.f")
    paths = []
    for line in listing.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        p = root / line.strip()
        if not p.is_file():
            raise SystemExit(f"Missing simulation source: {p}")
        paths.append(str(p))
    (out / f"{name}.f").write_text("\n".join(paths) + "\n")
print(f"Build-local absolute filelists: {out}")
