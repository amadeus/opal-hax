#!/usr/bin/env python3
"""Pull the camera-side firmware packages out of Opal.app's libdepthai-core.dylib.

The device firmware (Myriad X, OS independent) and bootloader are embedded as xz-compressed tarballs
("fwp"). Usage: extract-opal-fw.py [path/to/Opal.app] [outdir]
Defaults: "Opal Backup.app" at the repo root, tools/vendor/opal-fw (gitignored: proprietary).
"""
import io, lzma, os, re, subprocess, sys, tarfile, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
app = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "Opal Backup.app")
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "tools", "vendor", "opal-fw")
lib = os.path.join(app, "Contents", "Frameworks", "libdepthai-core.dylib")
os.makedirs(out, exist_ok=True)
with tempfile.TemporaryDirectory() as t:
    thin = os.path.join(t, "dai")
    subprocess.run(["lipo", "-thin", "arm64", lib, "-output", thin], check=True)
    data = open(thin, "rb").read()
names = set(m.group(0).decode() for m in re.finditer(rb"depthai-(device|bootloader)-fwp-[0-9a-f]{40}\.tar\.xz", data))
found = {}
for m in re.finditer(rb"\xfd7zXZ\x00", data):
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    chunk = data[m.start():m.start() + 64_000_000]
    try:
        tarbytes = dec.decompress(chunk)
        members = [x.name for x in tarfile.open(fileobj=io.BytesIO(tarbytes)).getmembers()]
    except Exception:
        continue
    raw = chunk[:len(chunk) - len(dec.unused_data)]
    kind = "bootloader" if any(n.startswith("depthai-bootloader") for n in members) else "device"
    name = next(n for n in names if n.startswith("depthai-%s-fwp-" % kind))
    open(os.path.join(out, name), "wb").write(raw)
    found[kind] = name
    print("%s: %s (%d bytes) %s" % (kind, name, len(raw), members))
if set(found) != {"device", "bootloader"}:
    sys.exit("did not find both firmware packages")
