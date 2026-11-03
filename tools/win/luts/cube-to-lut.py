#!/usr/bin/env python3
"""Convert the baked .cube LUTs to Opal for Windows' binary format and rewrite index.json to point at them.
Text .cube files take tens of seconds to parse at startup (13 x 64^3 lines); the binary loads instantly.

Format (.lut): "OPALLUT1" (8 bytes), uint32 size N, then N^3 RGBA texels as little-endian float16, red fastest,
then green, then blue (same order as .cube), alpha = 1. Loaded straight into a DXGI R16G16B16A16_FLOAT 3D texture.
usage: cube-to-lut.py <luts dir>   (converts in place, removes the .cube files)
"""
import json, os, struct, sys
from array import array

d = sys.argv[1]
idx_path = os.path.join(d, "index.json")
idx = json.load(open(idx_path))
looks = idx["looks"] if isinstance(idx, dict) else idx
for e in looks:
    src = os.path.join(d, e["file"])
    if not src.endswith(".cube") or not os.path.exists(src):
        continue
    n, vals = 0, []
    with open(src) as f:
        for line in f:
            if line.startswith("LUT_3D_SIZE"):
                n = int(line.split()[1])
                continue
            c = line[:1]
            if c.isdigit() or c in "-.":
                r, g, b = line.split()[:3]
                vals += (float(r), float(g), float(b), 1.0)
    assert n > 1 and len(vals) == n * n * n * 4, src
    out = src[:-5] + ".lut"
    with open(out, "wb") as f:
        f.write(b"OPALLUT1" + struct.pack("<I", n))
        f.write(struct.pack("<%de" % len(vals), *vals))
    os.remove(src)
    e["file"] = os.path.basename(out)
json.dump(idx, open(idx_path, "w"), indent=1)
print("converted %d LUTs in %s" % (len(looks), d))
