#!/usr/bin/env python3
"""Summarise a geometry-capture validation (`rt scene validate MASK img`, docs/raytracing.md).

  rt_validate.py STATUS.json [RUN_DIR/rt-validate.raw OUT.jpg]

STATUS.json: the `quiver lab call <i> rt scene status` output (its trace.validate.bins: primary
rays from the camera against the G-buffer depth). Prints the error histogram over geometry
pixels, split by the normal buffer's alpha (1 = the foliage material term). With a raw image:
a colour-coded JPEG (green |err| < 0.2 %, light green < 1 %, yellow < 5 %, red ray nearer, blue
ray farther, magenta geometry missed, purple sky hit, black sky).
"""
import json
import sys

NAMES = ["sky+miss", "sky+hit", "miss", "<0.2%", "<1%", "<5%", "nearer", "farther"]


def main():
    st = json.load(open(sys.argv[1]))
    st = st.get("result", st)
    v = st["trace"]["validate"]
    b = v["bins"]
    print(f"frame {v['frame']} mask {v['mask']:#x} pixels {v['pixels']}")
    for half, label in ((0, "other"), (8, "normal.a=1 (foliage)")):
        geo = sum(b[half + 2: half + 8])
        if not geo:
            continue
        row = "  ".join(f"{NAMES[i]} {100.0 * b[half + i] / geo:5.1f}%" for i in range(2, 8))
        print(f"{label:22s} geometry px {geo:8d}: {row}")
    sky = b[0] + b[1]
    if sky:
        print(f"sky px {sky}: hit {100.0 * b[1] / sky:.1f}%")
    geo = sum(b[2:8]) + sum(b[10:16])
    good = b[3] + b[4] + b[11] + b[12]
    print(f"all geometry: within 1 %: {100.0 * good / geo:.2f}%  within 5 %: "
          f"{100.0 * (good + b[5] + b[13]) / geo:.2f}%  missed: {100.0 * (b[2] + b[10]) / geo:.2f}%")
    if len(sys.argv) > 3:
        import numpy as np
        from PIL import Image
        import struct
        raw = open(sys.argv[2], "rb").read()
        _, w, h, fmt, pitch = struct.unpack_from("<5I", raw, 0)
        a = np.frombuffer(raw, np.uint8, offset=20, count=pitch * h).reshape(h, pitch)[:, : w * 4].reshape(h, w, 4)
        Image.fromarray(a[:, :, :3]).resize((w // 2, h // 2)).save(sys.argv[3], quality=80)
        print("image", sys.argv[3])


if __name__ == "__main__":
    main()
