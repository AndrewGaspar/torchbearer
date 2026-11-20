#!/usr/bin/env python3
"""Analyse a raytracing-probe census (docs/raytracing.md, `quiver lab call <i> rt census N TAG`).

Inputs, from the run folder: rt-pso.txt (pipeline states and shader hashes, written as they are
created), rt-census-TAG.txt (one D3D12 event per line), rt-shaders/*.dxil (`rt shaders all`), and
the shaderbin index from rt_shaderbin_index.py (maps a captured shader to its effect file,
technique and g_* names).

  rt_census.py RUN_DIR TAG INDEX.json [--frame K] [--passes] [--json OUT]

Prints, for one whole frame in GPU (submission) order: the passes (runs of draws with the same
render targets), their formats, draw / triangle / instance counts and effects; engine markers;
then totals: geometry (unique meshes, vertex / index buffers, heaps), instancing, per-effect
breakdown, pipeline-state counts.
"""
import collections
import glob
import hashlib
import json
import os
import re
import struct
import sys

DXGI = {0: "UNKNOWN", 2: "R32G32B32A32_FLOAT", 6: "R32G32B32_FLOAT", 9: "R16G16B16A16_TYPELESS",
        10: "R16G16B16A16_FLOAT", 11: "R16G16B16A16_UNORM", 13: "R16G16B16A16_SNORM", 14: "R16G16B16A16_SINT",
        16: "R32G32_FLOAT", 19: "R32G8X24_TYPELESS", 20: "D32_FLOAT_S8X24_UINT", 23: "R10G10B10A2_TYPELESS",
        24: "R10G10B10A2_UNORM", 26: "R11G11B10_FLOAT", 27: "R8G8B8A8_TYPELESS", 28: "R8G8B8A8_UNORM",
        29: "R8G8B8A8_UNORM_SRGB", 30: "R8G8B8A8_UINT", 31: "R8G8B8A8_SNORM", 34: "R16G16_FLOAT",
        35: "R16G16_UNORM", 37: "R16G16_SNORM", 39: "R32_TYPELESS", 40: "D32_FLOAT", 41: "R32_FLOAT",
        42: "R32_UINT", 44: "R24G8_TYPELESS", 45: "D24_UNORM_S8_UINT", 49: "R8G8_UNORM", 54: "R16_FLOAT",
        55: "D16_UNORM", 56: "R16_UNORM", 57: "R16_UINT", 61: "R8_UNORM", 65: "A8_UNORM",
        71: "BC1_UNORM", 72: "BC1_UNORM_SRGB", 77: "BC3_UNORM", 80: "BC4_UNORM", 83: "BC5_UNORM",
        87: "B8G8R8A8_UNORM", 88: "B8G8R8X8_UNORM", 91: "B8G8R8A8_UNORM_SRGB", 95: "BC6H_UF16", 98: "BC7_UNORM",
        -1: "(resource format)"}
HEAP = {"0": "?", "1": "DEFAULT", "2": "UPLOAD", "3": "READBACK", "4": "CUSTOM", "?": "?"}


def fmt(n):
    return DXGI.get(int(n), str(n))


def dxil_bc_hash(blob):
    if blob[:4] != b"DXBC":
        return None
    n = struct.unpack_from("<I", blob, 28)[0]
    for i in range(n):
        off = struct.unpack_from("<I", blob, 32 + 4 * i)[0]
        if blob[off:off + 4] == b"DXIL":
            d = blob[off + 8:]
            boff, bsz = struct.unpack_from("<II", d, 16)
            return hashlib.sha1(d[8 + boff:8 + boff + bsz]).hexdigest()[:16]
    return None


def load_pso(run):
    psos, shaders = {}, {}
    for line in open(os.path.join(run, "rt-pso.txt"), errors="replace"):
        p = line.split()
        if not p:
            continue
        if p[0] == "SH":
            shaders[p[1]] = {"size": int(p[2]), "kind": p[3], "digest": p[4]}
        elif p[0] == "PSO":
            d = {"id": int(p[1]), "type": p[3]}
            for kv in p[4:]:
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    d[k] = v
            psos[int(p[1])] = d
    return psos, shaders


def load_index(path):
    by_bc, by_dig = {}, {}
    for r in json.load(open(path)):
        if r["bc"]:
            by_bc.setdefault(r["bc"], r)
        by_dig.setdefault(r["digest"], r)
    return by_bc, by_dig


def shader_info(run, shaders, by_bc, by_dig):
    info = {}
    for h, s in shaders.items():
        r = by_dig.get(s["digest"])
        f = os.path.join(run, "rt-shaders", h + ".dxil")
        if not r and os.path.exists(f):
            bc = dxil_bc_hash(open(f, "rb").read())
            r = by_bc.get(bc)
        info[h] = r
    return info


def effect_of(pso, info):
    """Effect file / technique of a PSO (its pixel shader's, else vertex / compute)."""
    for k in ("ps", "vs", "cs"):
        h = pso.get(k)
        if h and h != "-" and info.get(h):
            r = info[h]
            return r["file"], r["label"]
    return "?", "?"


def parse_census(path):
    lines = open(path, errors="replace").read().splitlines()
    return lines


def main():
    args = sys.argv[1:]
    run, tag, index = args[0], args[1], args[2]
    frame = int(args[args.index("--frame") + 1]) if "--frame" in args else 1
    show_passes = "--passes" in args
    out_json = args[args.index("--json") + 1] if "--json" in args else None
    psos, shaders = load_pso(run)
    by_bc, by_dig = load_index(index)
    info = shader_info(run, shaders, by_bc, by_dig)
    lines = parse_census(os.path.join(run, f"rt-census-{tag}.txt"))

    res = {}     # ptr -> description
    bufs = {}    # ptr -> (size, heap)
    names = {}   # ptr -> pool name
    seg = {}     # list -> current segment (list of events)
    submitted = []  # (frame, [events]) in submission order
    fr = -1
    for ln in lines:
        p = ln.split()
        if not p:
            continue
        t = p[0]
        if t == "F":
            fr = int(p[1])
            continue
        if t == "R":
            res[p[1]] = {"w": int(p[2]), "h": int(p[3]), "d": int(p[4]), "fmt": fmt(p[5]),
                         "flags": p[7].split("=")[1], "samples": p[8].split("=")[1], "heap": p[10].split("=")[1]}
            continue
        if t == "BUF":
            bufs[p[1]] = (int(p[2]), HEAP.get(p[3].split("=")[1], "?"))
            continue
        if t == "RN":
            names[p[2]] = p[1]
            continue
        if t == "L":
            if p[2] == "reset":
                seg[p[1]] = [("reset", p)]
            continue
        if t == "E":
            for l in p[4:]:
                if l in seg:
                    submitted.append((fr + 1, seg.pop(l)))
            continue
        if t == "M":
            l = p[3].split("=")[1]
            if l in seg:
                seg[l].append(("M", p))
            continue
        if t in ("P", "O", "V", "I", "T", "D", "X", "K", "BAR", "CR", "CD", "EV", "XI", "CP", "CT", "CB"):
            l = p[1]
            if l in seg:
                seg[l].append((t, p))

    def rt_name(ptr):
        r = res.get(ptr)
        nm = names.get(ptr, "")
        if not r:
            return f"{nm or ptr}"
        s = f"{r['w']}x{r['h']} {r['fmt']}"
        return f"{nm}({s})" if nm else s

    # Walk frame `frame` in submission order.
    passes = []
    cur = None
    totals = collections.Counter()
    eff = collections.defaultdict(collections.Counter)
    meshes = {}
    vb_used, ib_used = {}, {}
    pso_used = set()
    a2c = 0
    markers = []
    inst_draws = []
    for f, evs in submitted:
        if f != frame:
            continue
        state = {"pso": 0, "rt": "-", "ds": "-", "vb": [], "ib": None, "topo": 4}
        if evs and evs[0][0] == "reset":
            m = re.search(r"pso=(\d+)", " ".join(evs[0][1]))
            state["pso"] = int(m.group(1)) if m else 0
        for t, p in evs:
            if t == "P":
                state["pso"] = int(p[2])
            elif t == "T":
                state["topo"] = int(p[2])
            elif t == "O":
                rts = [x for x in p[3].split(",") if x and x != "-"]
                state["rt"] = " + ".join(rt_name(x.split(":")[0]) for x in rts) or "-"
                ds = p[4].split("=", 1)[1]
                state["ds"] = rt_name(ds.split(":")[0]) if ds not in ("-", "?") else ds
            elif t == "V":
                state["vb"] = p[4:]
                for v in p[4:]:
                    ptr = v.split("+")[0]
                    sz, hp = bufs.get(ptr, (0, "?"))
                    vb_used[ptr] = (sz, hp)
            elif t == "I":
                state["ib"] = p[2] if p[2] != "-" else None
                if state["ib"]:
                    ptr = p[2].split("+")[0]
                    ib_used[ptr] = bufs.get(ptr, (0, "?")) + (fmt(p[4]),)
            elif t == "M":
                markers.append((len(passes), p[1], p[2]))
            elif t in ("D", "X", "K"):
                key = (state["rt"], state["ds"]) if t != "K" else ("compute", "")
                if cur is None or cur["key"] != key:
                    cur = {"key": key, "draws": 0, "dispatch": 0, "tris": 0, "inst_draws": 0, "instances": 0,
                           "psos": set(), "pso_n": collections.Counter(), "effects": collections.Counter(), "markers": [m for m in markers if m[0] == len(passes)]}
                    passes.append(cur)
                pso = psos.get(state["pso"], {})
                pso_used.add(state["pso"])
                cur["pso_n"][state["pso"]] += 1
                e = effect_of(pso, info)
                if t == "K":
                    cur["dispatch"] += 1
                    totals["dispatch"] += 1
                    cur["effects"][f"{e[0]}:{e[1]}"] += 1
                    cur["psos"].add(state["pso"])
                    continue
                if t == "X":
                    ic, n, si, bv = int(p[2]), int(p[3]), int(p[4]), int(p[5])
                else:
                    ic, n, si, bv = int(p[2]), int(p[3]), int(p[4]), 0
                tris = (ic // 3 if state["topo"] == 4 else max(ic - 2, 0) if state["topo"] == 5 else 0) * n
                cur["draws"] += 1
                cur["tris"] += tris
                cur["psos"].add(state["pso"])
                cur["effects"][f"{e[0]}:{e[1]}"] += 1
                if n > 1:
                    cur["inst_draws"] += 1
                    cur["instances"] += n
                totals["draws"] += 1
                totals["tris"] += tris
                if pso.get("a2c") == "1":
                    a2c += 1
                ee = eff[e[0]]
                ee["draws"] += 1
                ee["tris"] += tris
                ee["instances"] += n
                if n > 1:
                    ee["inst_draws"] += 1
                # Geometry identity: index buffer location + range + base vertex + VB0.
                if t == "X":
                    k = (state["ib"], si, ic, bv, state["vb"][0] if state["vb"] else None)
                    m = meshes.setdefault(k, {"tris": ic // 3, "draws": 0, "inst": 0, "effect": e[0],
                                              "pass": state["rt"], "il": pso.get("il", "")})
                    m["draws"] += 1
                    m["inst"] += n

    print(f"frame {frame}: {totals['draws']} draws, {totals['dispatch']} dispatches, "
          f"{totals['tris'] / 1e6:.2f} M triangles submitted, {len(pso_used)} pipeline states used "
          f"(of {len(psos)} created so far), alpha-to-coverage draws {a2c}")
    if show_passes:
        print("\n# passes in submission order: draws / dispatches / M tris / instanced draws (instances) / PSOs / top effects")
        for i, ps in enumerate(passes):
            mk = " ".join(f"[{m[1]}:{m[2]}]" for m in ps["markers"])
            top = ", ".join(f"{k}x{v}" for k, v in ps["effects"].most_common(4))
            print(f"{i:3d} {mk} RT {ps['key'][0]} | DS {ps['key'][1]}")
            print(f"     {ps['draws']} d {ps['dispatch']} k {ps['tris'] / 1e6:.3f} Mt {ps['inst_draws']} ({ps['instances']}) "
                  f"{len(ps['psos'])} psos | {top}")
    print("\n# effects (by pixel shader's effect file): draws, M tris, instanced draws, instances")
    for k, v in sorted(eff.items(), key=lambda kv: -kv[1]["tris"]):
        print(f"  {k:24s} {v['draws']:6d} {v['tris'] / 1e6:8.3f} {v['inst_draws']:6d} {v['instances']:7d}")
    # Unique meshes.
    ut = sum(m["tris"] for m in meshes.values())
    print(f"\n# geometry: {len(meshes)} unique indexed meshes (IB range + base vertex + VB0), {ut / 1e6:.2f} M unique triangles")
    vbt = sum(s for s, h in vb_used.values())
    ibt = sum(v[0] for v in ib_used.values())
    print(f"  vertex buffers bound: {len(vb_used)} resources, {vbt / 2**20:.1f} MiB; by heap: " +
          ", ".join(f"{h} {n}" for h, n in collections.Counter(h for s, h in vb_used.values()).items()))
    print(f"  index buffers bound: {len(ib_used)} resources, {ibt / 2**20:.1f} MiB; formats: " +
          ", ".join(f"{f} {n}" for f, n in collections.Counter(v[2] for v in ib_used.values()).items()))
    pos = collections.Counter()
    for m in meshes.values():
        mm = re.search(r"POSITION:0:(\d+):(\d+)", m["il"])
        pos[fmt(mm.group(1)) if mm else "?"] += m["tris"]
    print("  POSITION formats (by unique triangles): " + ", ".join(f"{k} {v / 1e6:.2f} M" for k, v in pos.most_common()))
    per_eff = collections.Counter()
    for m in meshes.values():
        per_eff[m["effect"]] += m["tris"]
    print("  unique triangles by effect: " + ", ".join(f"{k} {v / 1e6:.2f} M" for k, v in per_eff.most_common(12)))
    if out_json:
        json.dump({"passes": [{**p, "psos": sorted(p["psos"]), "pso_n": dict(p["pso_n"]), "effects": dict(p["effects"])}
                              for p in passes],
                   "effects": {k: dict(v) for k, v in eff.items()}, "unique_tris": ut, "meshes": len(meshes)},
                  open(out_json, "w"), default=str)


if __name__ == "__main__":
    main()
