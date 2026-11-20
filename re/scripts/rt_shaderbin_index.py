#!/usr/bin/env python3
"""Index the game's precompiled shaders (shaders/build/pc_DXIL/*.shaderbin) for docs/raytracing.md.

Each .shaderbin is an RFX effect file holding many DXBC containers with DXIL. For every container:
file, offset, size, container digest (header bytes 4..20), a hash of the DXIL bitcode (stable across
the engine's container rebuild that strips debug parts), shader kind (DXIL program header), the
technique label (last printable string before the container) and the g_* names readable in it
(constant / resource names from the DXIL metadata, e.g. g_vLeafWindTable).

  rt_shaderbin_index.py <install>/shaders/build/pc_DXIL OUT.json
"""
import hashlib
import json
import os
import re
import struct
import sys

KINDS = {0: "ps", 1: "vs", 2: "gs", 3: "hs", 4: "ds", 5: "cs", 6: "lib", 13: "ms", 14: "as"}
NAME = re.compile(rb"g_[A-Za-z][A-Za-z0-9_]{2,60}")
LABEL = re.compile(rb"[A-Za-z_][A-Za-z0-9_ .:\\/-]{3,80}")


def parts(blob):
    if len(blob) < 32 or blob[:4] != b"DXBC":
        return {}
    n = struct.unpack_from("<I", blob, 28)[0]
    out = {}
    for i in range(min(n, 64)):
        off = struct.unpack_from("<I", blob, 32 + 4 * i)[0]
        if off + 8 > len(blob):
            continue
        cc = blob[off:off + 4]
        sz = struct.unpack_from("<I", blob, off + 4)[0]
        out[cc.decode("latin1")] = blob[off + 8:off + 8 + sz]
    return out


def dxil_info(blob):
    p = parts(blob)
    d = p.get("DXIL")
    if not d or len(d) < 24:
        return None, None
    ver = struct.unpack_from("<I", d, 0)[0]
    kind = KINDS.get(ver >> 16, str(ver >> 16))
    boff, bsz = struct.unpack_from("<II", d, 16)
    bc = d[8 + boff:8 + boff + bsz]
    return kind, hashlib.sha1(bc).hexdigest()[:16]


def main():
    src, out = sys.argv[1], sys.argv[2]
    rows = []
    for fn in sorted(os.listdir(src)):
        if not fn.endswith(".shaderbin"):
            continue
        data = open(os.path.join(src, fn), "rb").read()
        pos = 0
        while True:
            i = data.find(b"DXBC", pos)
            if i < 0:
                break
            if i + 32 > len(data):
                break
            size = struct.unpack_from("<I", data, i + 24)[0]
            if size < 32 or i + size > len(data):
                pos = i + 4
                continue
            blob = data[i:i + size]
            kind, bc = dxil_info(blob)
            pre = data[max(0, i - 512):i]
            labels = LABEL.findall(pre)
            label = labels[-1].decode("latin1") if labels else ""
            names = sorted({m.decode() for m in NAME.findall(blob)})
            rows.append({"file": fn[:-10], "off": i, "size": size, "digest": blob[4:20].hex(), "bc": bc,
                         "kind": kind, "label": label, "names": names})
            pos = i + size
    json.dump(rows, open(out, "w"))
    print(f"{len(rows)} containers in {len({r['file'] for r in rows})} files -> {out}")


if __name__ == "__main__":
    main()
