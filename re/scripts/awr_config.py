#!/usr/bin/env python3
"""Decode AWR's Documents/Remedy/AlanWakeRemastered/config (GameProperties::save, RVA 0x1dca80).

usage: awr_config.py CONFIG [CONFIG2]   (two files: print only the fields that differ)

Layout from the static analysis in docs/settings.md (format version 0x9f); confirmed by diffing
the file after changes made both through the game's own menu and through torchbearer.
"""
import struct
import sys

# (kind, name): i = int32, f = float32, b = u8, h = u16, q = int64, s = u32 length + bytes
TOP = [
    ("i", "version"), ("i", "vsync"), ("b", "hud"), ("i", "g955680"), ("i", "shadows"),
    ("i", "g955684"), ("i", "g955674"), ("b", "msaa"), ("i", "g955690"), ("i", "anisotropy"),
    ("b", "fxaa"), ("i", "g95567c"), ("b", "godrays"), ("b", "ssao"), ("i", "volumetrics"),
    ("f", "lod_distance"), ("b", "english_speech"), ("f", "p2c0"), ("f", "p2c4"),
] + [("i", f"p{o:x}") for o in range(0x2c8, 0x2ec, 4)] + [
    ("f", "p2ec"), ("f", "grass_distance"), ("i", "terrain"), ("i", "dlss"), ("i", "taa"),
    ("f", "resolution_scale"),
]
# GameProperties+0xb0 sub-object (FUN_1401dcf20); names are the menu items they back.
PROFILE = [
    ("i", "commentary"), ("f", "brightness"), ("b", "subtitles"), ("f", "subtitle_size"),
    ("f", "subtitle_background"), ("f", "music"), ("f", "effects"), ("f", "speech"),
    ("f", "cinematic"), ("b", "invert_y"), ("b", "invert_x"), ("f", "gamepad_sensitivity"),
    ("b", "vibration"), ("i", "gamepad_scheme"), ("s", "name"), ("f", "mouse_sensitivity_x"),
    ("f", "mouse_sensitivity_y"), ("b", "p100"), ("b", "p135"), ("f", "draw_distance"),
    ("i", "quality"),
]
TAIL = [
    ("b", "p101"), ("b", "p102"), ("b", "episode"), ("f", "fov"), ("b", "direct_aiming"),
    ("b", "motion_blur"), ("b", "film_grain"), ("i", "p20c"), ("s", "p210"),
    ("b", "swap_sticks"), ("b", "swap_triggers"), ("q", "p2b8"), ("i", "modern_camera"),
    ("f", "hud_size"),
]
# Then the collectibles block (FUN_140117910), not decoded here.


def decode(buf):
    out = {}
    o = 0

    def rd(kind):
        nonlocal o
        if kind == "i":
            v = struct.unpack_from("<i", buf, o)[0]; o += 4
        elif kind == "f":
            v = round(struct.unpack_from("<f", buf, o)[0], 4); o += 4
        elif kind == "b":
            v = buf[o]; o += 1
        elif kind == "q":
            v = struct.unpack_from("<q", buf, o)[0]; o += 8
        elif kind == "h":
            v = struct.unpack_from("<H", buf, o)[0]; o += 2
        else:
            n = struct.unpack_from("<I", buf, o)[0]
            v = buf[o + 4:o + 4 + n].decode("latin1"); o += 4 + n
        return v

    for k, n in TOP + PROFILE:
        out[n] = (o, rd(k))
    for i in range(5):  # five std::set<u8/u16> of unlocks
        start = o
        c = rd("b")
        items = [rd("h" if i == 3 else "b") for _ in range(c)]
        out[f"set{i}"] = (start, items)
    for k, n in TAIL:
        out[n] = (o, rd(k))
    return out


def main():
    a = decode(open(sys.argv[1], "rb").read())
    if len(sys.argv) > 2:
        b = decode(open(sys.argv[2], "rb").read())
        for k in a:
            if a[k][1] != b[k][1]:
                print(f"{a[k][0]:#05x} {k:22s} {a[k][1]} -> {b[k][1]}")
        return
    for k, (off, v) in a.items():
        print(f"{off:#05x} {k:22s} {v}")


if __name__ == "__main__":
    main()
