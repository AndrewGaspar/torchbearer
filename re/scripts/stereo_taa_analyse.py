#!/usr/bin/env python3
"""Analyse a stereo_taa_battery.sh run (docs/stereo.md "Temporal state per eye").

  stereo_taa_analyse.py RUN_DIR [--player x0,y0,x1,y1]

Per variant (off / shared / pereye), on 3840x1080 sbs captures (left eye = [0,1920), right eye =
[1920,3840), both from one game update):
  sep0      |L-R| (same camera, same update: anything here is per-pass state that differs)
  pan0      |L-R| under a 0.5 deg/update pan at separation 0: an eye that reprojects the OTHER eye's
            history (or motion vectors) no longer matches its twin; per-eye state keeps L == R
  sep325    disparity field, joined per patch with the off variant (TAA off: the reference field)
  pan325    far-field disparity under the pan (must stay the static value)
  sharpness mean |gradient| of each eye, whole frame and the player box (smear lowers it)
"""
import argparse
import glob
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("run")
ap.add_argument("--player", default="1000,470,1450,1080", help="player box in eye coordinates")
ap.add_argument("--proj00", type=float, default=1.2088)
ap.add_argument("--eye-width", type=int, default=1920, help="960 for output = hsbs at 1920x1080")
ap.add_argument("--variants", default="off,shared,pereye,mbshared,mbpereye",
                help="tags in order; the first is the reference disparity field")
args = ap.parse_args()
run = args.run
W = args.eye_width
PB = [int(v) for v in args.player.split(",")]
tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stereo_disparity.py")
VARIANTS = args.variants.split(",")


def files(tag, eye="SBS"):
    return sorted(glob.glob(os.path.join(run, f"frame_*_{tag}_{eye}.bmp")))


def load(f):
    return np.asarray(Image.open(f).convert("RGB")).astype(np.float64)


def halves(f):
    a = load(f)
    return a[:, :W], a[:, W:2 * W]


def box(a, b=PB):
    return a[b[1]:b[3], b[0]:b[2]]


def grad(a):
    g = a.mean(axis=2)
    return float(np.abs(np.diff(g, axis=1)).mean() + np.abs(np.diff(g, axis=0)).mean())


def disparity(f, eye=0.0325, extra=()):
    out = os.path.join(run, "tmp_taa_disp.json")
    cmd = [sys.executable, "-I", tool, f, f, "--crop", f"0,0,{W},1080", "--crop-right", f"{W},0,{2 * W},1080",
           "--proj00", str(args.proj00), "--eye", str(eye), "--json", out, "--max-shift", "120", *extra]
    subprocess.run(cmd, check=True, capture_output=True)
    d = json.load(open(out))
    return {(int(p[0]), int(p[1])): p[2] for p in d["points"]}


def st(v):
    v = np.asarray(list(v))
    if len(v) == 0:
        return "n=0"
    return f"median {np.median(v):+.3f}  p5..p95 {np.percentile(v, 5):+.2f}..{np.percentile(v, 95):+.2f}  n={len(v)}"



base = None
for v in VARIANTS:
    print(f"\n######## {v}")
    for f in files(f"{v}sep0"):
        L, R = halves(f)
        d = np.abs(L - R)
        print(f"  sep0    {os.path.basename(f)}  |L-R| mean {d.mean():.4f}/255 max {d.max():.0f}  player {box(d).mean():.4f}")
    for f in files(f"{v}pan0"):
        L, R = halves(f)
        d = np.abs(L - R)
        print(f"  pan0    {os.path.basename(f)}  |L-R| mean {d.mean():.4f}/255 max {d.max():.0f}  player {box(d).mean():.4f}"
              f"  sharpness L {grad(L):.3f} R {grad(R):.3f}")
    for f in files(f"{v}sep325"):
        L, R = halves(f)
        pts = disparity(f)
        if v == VARIANTS[0]:
            base = pts
        line = f"  sep325  {os.path.basename(f)}  disparity {st(pts.values())}"
        print(line)
        if base is not None and v != VARIANTS[0]:
            ks = [k for k in pts if k in base]
            dd = np.array([pts[k] - base[k] for k in ks])
            print(f"          vs off, per patch: diff {st(dd)}  |diff| mean {np.abs(dd).mean():.3f} px")
        far = [d for (x, y), d in pts.items() if y < 500]
        print(f"          far-field (y<500) {st(far)}")
        print(f"          sharpness L {grad(L):.3f} R {grad(R):.3f}  player L {grad(box(L)):.3f} R {grad(box(R)):.3f}")
    for f in files(f"{v}pan325"):
        pts = disparity(f, extra=("--region", "far:0,100,1920,500"))
        far = [d for (x, y), d in pts.items() if y < 500]
        L, R = halves(f)
        print(f"  pan325  {os.path.basename(f)}  far-field {st(far)}  sharpness L {grad(L):.3f} R {grad(R):.3f}"
              f"  player L {grad(box(L)):.3f} R {grad(box(R)):.3f}")
