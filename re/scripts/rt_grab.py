#!/usr/bin/env python3
"""Decode render-target grabs of the raytracing probe (`rt grab`, docs/raytracing.md).

A grab file: "TBRT", width, height, DXGI format, row pitch (u32 each), then the rows.
  rt_grab.py FILE.raw [OUT_PREFIX]      -> per-channel stats; OUT_PREFIX-<ch>.png per channel
Formats: 10 R16G16B16A16_FLOAT, 87 B8G8R8A8_UNORM, 28 R8G8B8A8_UNORM, 41 R32_FLOAT,
34 R16G16_FLOAT, 54 R16_FLOAT, 26 R11G11B10_FLOAT (not decoded).
"""
import struct
import sys

import numpy as np
from PIL import Image


def load(path):
    b = open(path, "rb").read()
    magic, w, h, fmt, pitch = struct.unpack_from("<5I", b, 0)
    assert magic == 0x54524254, "not a grab"
    rows = np.frombuffer(b, np.uint8, offset=20, count=pitch * h).reshape(h, pitch)
    if fmt == 10:
        a = rows[:, : w * 8].copy().view(np.float16).reshape(h, w, 4).astype(np.float32)
        names = "RGBA"
    elif fmt in (87, 28):
        a = rows[:, : w * 4].reshape(h, w, 4).astype(np.float32) / 255
        names = "BGRA" if fmt == 87 else "RGBA"
    elif fmt == 41:
        a = rows[:, : w * 4].copy().view(np.float32).reshape(h, w, 1)
        names = "R"
    elif fmt == 34:
        a = rows[:, : w * 4].copy().view(np.float16).reshape(h, w, 2).astype(np.float32)
        names = "RG"
    elif fmt == 54:
        a = rows[:, : w * 2].copy().view(np.float16).reshape(h, w, 1).astype(np.float32)
        names = "R"
    else:
        raise SystemExit(f"format {fmt} not decoded")
    return a, names, fmt


def main():
    a, names, fmt = load(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else None
    print(f"{sys.argv[1]}: {a.shape[1]}x{a.shape[0]} format {fmt}")
    for i, n in enumerate(names):
        c = a[:, :, i]
        fin = c[np.isfinite(c)]
        q = np.percentile(fin, [0, 1, 50, 99, 100]) if fin.size else [0] * 5
        print(f"  {n}: min {q[0]:.4g} p1 {q[1]:.4g} median {q[2]:.4g} p99 {q[3]:.4g} max {q[4]:.4g} mean {fin.mean():.4g}")
        if out:
            lo, hi = q[1], q[3]
            v = np.clip((c - lo) / (hi - lo if hi > lo else 1), 0, 1)
            Image.fromarray((np.nan_to_num(v) * 255).astype(np.uint8)).save(f"{out}-{n}.png")


if __name__ == "__main__":
    main()
