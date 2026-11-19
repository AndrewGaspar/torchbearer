#!/usr/bin/env python3
"""Analyse a stereo_battery.sh run (docs/stereo.md): RUN_DIR with frame_*_<tag>_<eye>.bmp.

sbs captures (*_SBS.bmp) are 3840x1080: left eye in [0,1920), right eye in [1920,3840), both
rendered from the same game update. alternate captures are consecutive 1920-wide frames.
"""
import glob
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

run = sys.argv[1]
P00 = float(sys.argv[2]) if len(sys.argv) > 2 else 1.2088
W = 1920
tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stereo_disparity.py")


def files(tag, eye):
    return sorted(glob.glob(os.path.join(run, f"frame_*_{tag}_{eye}.bmp")))


def disparity(left, right, crop_l=None, crop_r=None, eye=0.0325, extra=()):
    out = os.path.join(run, "tmp_disp.json")
    cmd = [sys.executable, "-I", tool, left, right, "--proj00", str(P00), "--eye", str(eye), "--json", out,
           "--max-shift", "120", *extra]
    if crop_l:
        cmd += ["--crop", crop_l, "--crop-right", crop_r]
    subprocess.run(cmd, check=True, capture_output=True)
    d = json.load(open(out))
    return {(int(p[0]), int(p[1])): p[2] for p in d["points"]}, d["summary"]


def halves(f, **kw):
    return disparity(f, f, "0,0,1920,1080", "1920,0,3840,1080", **kw)


def stats(v):
    v = np.asarray(v)
    return f"median {np.median(v):+.3f}  p5..p95 {np.percentile(v, 5):+.2f}..{np.percentile(v, 95):+.2f}  n={len(v)}"


res = {}
print("== control: sbs at separation 0 (both halves = same update, same camera) ==")
for f in files("sep0", "SBS"):
    a = np.asarray(Image.open(f)).astype(int)
    diff = np.abs(a[:, :W] - a[:, W:])
    pts, _ = halves(f)
    print(os.path.basename(f), f"pixel |L-R| mean {diff.mean():.4f} max {diff.max()}  disparity", stats(list(pts.values())))

print("== separation sweep (same-update pairs) ==")
sweep = {}
for tag, mm in (("sep1625", 16.25), ("sep325", 32.5), ("sep650", 65.0)):
    f = files(tag, "SBS")[-1]
    pts, s = halves(f, eye=mm / 1000)
    sweep[mm] = pts
    print(f"{mm:6.2f} mm/eye:", stats(list(pts.values())))
for a, b in ((16.25, 32.5), (16.25, 65.0), (32.5, 65.0)):
    ks = [k for k in sweep[a] if k in sweep[b] and sweep[a][k] > 2.0]
    r = np.array([sweep[b][k] / sweep[a][k] for k in ks])
    print(f"ratio {b}/{a} requested {b / a:.3f}: median {np.median(r):.4f}  IQR {np.percentile(r, 25):.3f}..{np.percentile(r, 75):.3f}  n={len(r)}")
K = 2 * 0.0325 * P00 * (W / 2)
d = np.array([v for v in sweep[32.5].values()])
print(f"K(32.5 mm) = {K:.2f} px*m; disparity range {np.percentile(d, 1):.1f}..{np.percentile(d, 99):.1f} px "
      f"-> depth {K / max(np.percentile(d, 99), 1e-3):.2f}..{K / max(np.percentile(d, 1), 0.3):.1f} m")
for lo, hi in ((20, 60), (10, 20), (5, 10), (1, 5)):
    n = int(((d >= lo) & (d < hi)).sum())
    print(f"  {lo:2d}-{hi:2d} px: {n:4d} patches  (depth {K / hi:5.1f}..{K / lo:5.1f} m)")

print("== convergence 4 m vs parallel (same separation) ==")
pc, _ = halves(files("conv4", "SBS")[-1])
pp = sweep[32.5]
ks = [k for k in pp if k in pc and pp[k] > 0.5]
shift = np.array([pp[k] - pc[k] for k in ks])
print("parallel - converged:", stats(shift), f" expected uniform {K / 4:.2f} px")
print(f"recovered Zc = {K / np.median(shift):.3f} m (configured 4.000)")
p = np.array([pp[k] for k in ks]); c = np.array([pc[k] for k in ks])
near, far = c[K / p < 4], c[K / p > 4]
print(f"converged: nearer than 4 m median {np.median(near):+.2f} px (n={len(near)}), farther {np.median(far):+.2f} px (n={len(far)})")

print("== synthetic pan 0.5 deg/update: sbs halves vs alternate (frame-sequential) pairs ==")
for f in files("pansbs", "SBS"):
    pts, _ = halves(f, extra=("--region", "far:0,100,1920,500"))
    far = [v for (x, y), v in pts.items() if y < 500]
    print("sbs ", os.path.basename(f), "far-field", stats(far))
L, R = files("panalt", "L"), files("panalt", "R")
for l in L:
    fl = int(os.path.basename(l).split("_")[1])
    for r in R:
        if int(os.path.basename(r).split("_")[1]) == fl + 1:
            pts, _ = disparity(l, r, "0,0,1920,1080", "0,0,1920,1080")
            far = [v for (x, y), v in pts.items() if y < 500]
            print("alt ", os.path.basename(l), "->", os.path.basename(r), "far-field", stats(far))
print("== alternate at separation 0 (consecutive frames, no pan) ==")
L, R = files("alt0", "L"), files("alt0", "R")
for l in L:
    fl = int(os.path.basename(l).split("_")[1])
    for r in R:
        if int(os.path.basename(r).split("_")[1]) == fl + 1:
            a = np.asarray(Image.open(l)).astype(int)[:, :W]
            b = np.asarray(Image.open(r)).astype(int)[:, :W]
            pts, _ = disparity(l, r, "0,0,1920,1080", "0,0,1920,1080")
            print("alt0", os.path.basename(l), f"pixel |L-R| mean {np.abs(a - b).mean():.4f}  disparity", stats(list(pts.values())))
