#!/usr/bin/env python3
"""Turn a pipecap session into the pipeline templates the Windows app loads.

usage: make-template.py <session dir> [out dir]   (default out: tools/vendor/opal-template, gitignored: Opal's assets)
Writes, per variant (full = normal app mode, lowpower = Opal's low-power mode):
  <variant>.json            nodes (readable properties), connections, globalProperties, assets {key: file, alignment}
  assets/<sha256>.bin       asset bytes (NN blobs, on-camera scripts, sensor tuning), shared between variants
Picks the first captured pipeline with the most nodes as "full" and the one with the fewest as "lowpower".
"""
import hashlib, json, os, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "tools", "vendor", "opal-template")
os.makedirs(os.path.join(out, "assets"), exist_ok=True)
pipes = []
n = 1
while os.path.exists(os.path.join(src, "pipeline-%d.json" % n)):
    pipes.append((n, json.load(open(os.path.join(src, "pipeline-%d.json" % n)))))
    n += 1
by_size = sorted(pipes, key=lambda p: (-len(p[1]["pipeline"]["nodes"]), p[0]))
variants = {"full": by_size[0], "lowpower": by_size[-1]}
for name, (idx, j) in variants.items():
    storage = bytes(j["assetStorage"])
    assets = {}
    for key, a in j["assets"]["map"].items():
        data = storage[a["offset"]:a["offset"] + a["size"]]
        h = hashlib.sha256(data).hexdigest()
        open(os.path.join(out, "assets", h + ".bin"), "wb").write(data)
        assets[key] = {"file": "assets/%s.bin" % h, "alignment": a["alignment"], "size": a["size"]}
    p = j["pipeline"]
    tmpl = {"source": "pipecap %s pipeline-%d" % (os.path.basename(os.path.normpath(src)), idx),
            "globalProperties": p["globalProperties"], "nodes": p["nodes"], "connections": p["connections"], "assets": assets}
    json.dump(tmpl, open(os.path.join(out, name + ".json"), "w"), indent=1)
    print("%s: pipeline-%d, %d nodes, %d assets" % (name, idx, len(p["nodes"]), len(assets)))
