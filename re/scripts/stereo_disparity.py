#!/usr/bin/env python3
"""Per-patch horizontal disparity between two captured eye images (Torchbearer stereo).

Adapted from Ishimura's tools/stereo_disparity.py (ds-decomp). A pan shifts every patch by the
same amount; an eye translation shifts each patch by  2e*|P00|*(W/2)/z  (inverse to depth). So the
script reports every confident patch's shift and the depth it implies, plus region medians.

  stereo_disparity.py L.bmp R.bmp [--crop x0,y0,x1,y1 (of each input)] [--proj00 1.2087]
                      [--eye 0.0325] [--patch 32] [--stride 24] [--max-shift 80]
                      [--region name:x0,y0,x1,y1 ...] [--json out.json]

Positive shift = the feature sits further RIGHT in L than in R (uncrossed/parallel disparity).
"""
import argparse
import json
import sys

import numpy as np
from PIL import Image


def load(path, crop):
    im = Image.open(path).convert("L")
    a = np.asarray(im, dtype=np.float64)
    if crop:
        x0, y0, x1, y1 = crop
        a = a[y0:y1, x0:x1]
    return a


def match(L, R, x, y, p, smax):
    """Shift d such that L[y, x+d] ~ R[y, x]: search R's patch in L along the row."""
    ref = R[y:y + p, x:x + p]
    if ref.std() < 2.0:
        return None
    lo, hi = max(-smax, -x), min(smax, L.shape[1] - p - x)
    if hi - lo < 3:
        return None
    ssd = np.array([((L[y:y + p, x + d:x + d + p] - ref) ** 2).mean() for d in range(lo, hi + 1)])
    i = int(np.argmin(ssd))
    if i == 0 or i == len(ssd) - 1:
        return None
    a, b, c = ssd[i - 1], ssd[i], ssd[i + 1]
    den = a - 2 * b + c
    sub = 0.5 * (a - c) / den if den > 0 else 0.0
    conf = (np.median(ssd) - b) / (np.median(ssd) + 1e-9)
    return lo + i + sub, conf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("left")
    ap.add_argument("right")
    ap.add_argument("--crop", default="")
    ap.add_argument("--crop-right", default="", help="crop for the right image if it differs (SBS halves)")
    ap.add_argument("--proj00", type=float, default=0.0)
    ap.add_argument("--eye", type=float, default=0.0325, help="per-eye offset, metres")
    ap.add_argument("--patch", type=int, default=32)
    ap.add_argument("--stride", type=int, default=24)
    ap.add_argument("--max-shift", type=int, default=80)
    ap.add_argument("--min-conf", type=float, default=0.5)
    ap.add_argument("--region", action="append", default=[])
    ap.add_argument("--json", default="")
    a = ap.parse_args()
    cl = [int(v) for v in a.crop.split(",")] if a.crop else None
    cr = [int(v) for v in a.crop_right.split(",")] if a.crop_right else cl
    L, R = load(a.left, cl), load(a.right, cr)
    if L.shape != R.shape:
        sys.exit(f"size mismatch {L.shape} vs {R.shape}")
    H, W = L.shape
    K = 2 * a.eye * a.proj00 * (W / 2) if a.proj00 else 0.0
    pts = []
    for y in range(0, H - a.patch, a.stride):
        for x in range(0, W - a.patch, a.stride):
            m = match(L, R, x, y, a.patch, a.max_shift)
            if m and m[1] >= a.min_conf:
                pts.append((x + a.patch // 2, y + a.patch // 2, m[0], m[1]))
    pts = np.array(pts) if pts else np.zeros((0, 4))
    out = {"patches": len(pts), "K_px_m": K}
    if len(pts):
        d = pts[:, 2]
        out.update(median=float(np.median(d)), p5=float(np.percentile(d, 5)), p95=float(np.percentile(d, 95)),
                   min=float(d.min()), max=float(d.max()))
    regions = {}
    for r in a.region:
        name, box = r.split(":")
        x0, y0, x1, y1 = [int(v) for v in box.split(",")]
        sel = pts[(pts[:, 0] >= x0) & (pts[:, 0] < x1) & (pts[:, 1] >= y0) & (pts[:, 1] < y1)] if len(pts) else pts
        if len(sel):
            md = float(np.median(sel[:, 2]))
            regions[name] = {"n": int(len(sel)), "median": md, "p10": float(np.percentile(sel[:, 2], 10)),
                             "p90": float(np.percentile(sel[:, 2], 90)),
                             "implied_depth_m": (K / md) if (K and md > 0.05) else None}
        else:
            regions[name] = {"n": 0}
    out["regions"] = regions
    print(json.dumps(out, indent=1))
    if a.json:
        json.dump({"summary": out, "points": pts.tolist()}, open(a.json, "w"))


if __name__ == "__main__":
    main()
