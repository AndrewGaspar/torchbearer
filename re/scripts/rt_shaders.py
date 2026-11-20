#!/usr/bin/env python3
"""Classify the shaders a census used, from their DXIL disassembly (`rt shaders all disasm`).

  rt_shaders.py RUN_DIR TAG INDEX.json FRAME

Per shader (*.ll in RUN_DIR/rt-shaders): stage, alpha test (dx.op.discard), the $Globals
variables it actually loads (cbufferLoadLegacy rows mapped to the layout's offsets), its textures.
Then, over the census frame, draws / triangles per pass class that are alpha-tested, wind-animated
(vertex shader loads a wind / shake table), pre-skinned (vertex layout carries BLENDINDICES while
the vertex shader has no bone palette) or skinning-transform draws; and the distinct shaders and
pipeline states per pass class.
"""
import collections
import contextlib
import glob
import io
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import rt_census as C  # noqa: E402

VAR = re.compile(r"^;\s+(?:row_major\s+)?(\w+)\s+(g_\w+)(\[(\d+)\])?;\s+; Offset:\s+(\d+)", re.M)
LOAD = re.compile(r'@dx\.op\.cbufferLoadLegacy\.\w+\(i32 59, %dx\.types\.Handle %(?:"[^"]*"|[\w.$]+), i32 (%?[\w.]+)\)')
SIZES = {"float": 4, "float2": 8, "float3": 12, "float4": 16, "int": 4, "uint": 4, "bool": 4,
         "float4x4": 64, "float3x4": 48, "float4x3": 64, "float3x3": 48}


def analyse(path):
    t = open(path, errors="replace").read()
    stage = ("ps" if "; Pixel Shader" in t else "vs" if "; Vertex Shader" in t else
             "cs" if "; Compute Shader" in t else "?")
    loads = LOAD.findall(t)
    rows = {int(m) for m in loads if not m.startswith("%")}
    dynamic = any(m.startswith("%") for m in loads)  # a dynamically indexed array (tables)
    used = set()
    for typ, name, _, cnt, off in VAR.findall(t):
        size = SIZES.get(typ, 16) * (int(cnt) if cnt else 1)
        r0, r1 = int(off) // 16, (int(off) + size - 1) // 16
        if any(r in rows for r in range(r0, r1 + 1)) or (dynamic and cnt):
            used.add(name)
    tex = re.findall(r"^; (g_\w+)\s+texture", t, re.M)
    return {"stage": stage, "discard": "@dx.op.discard" in t, "used": used, "tex": tex,
            "palette": bool(re.search(r"\[(?:[2-9]\d|\d{3}) x \[3 x <4 x float>\]\]", t)),
            "uav": bool(re.search(r"^; \w+\s+UAV", t, re.M))}


def main():
    run, tag, index, frame = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    sh = {os.path.basename(f)[:-3]: analyse(f) for f in glob.glob(os.path.join(run, "rt-shaders", "*.ll"))}
    psos, _ = C.load_pso(run)
    tmp = os.path.join(run, f"rt-cls-{tag}.json")
    argv = sys.argv
    sys.argv = ["x", run, tag, index, "--frame", str(frame), "--json", tmp]
    with contextlib.redirect_stdout(io.StringIO()):
        C.main()
    sys.argv = argv
    passes = json.load(open(tmp))["passes"]
    os.remove(tmp)

    def cls(p):
        rt = p["key"][0]
        if "8192x4096" in rt:
            return "shadow maps"
        if "pNormalRT" in rt:
            return "G-buffer"
        if p["draws"] > 500 and "R11G11B10" in rt:
            return "colour passes (2)"
        return None

    agg = collections.defaultdict(collections.Counter)
    pso_set = collections.defaultdict(set)
    ps_set = collections.defaultdict(set)
    vs_set = collections.defaultdict(set)
    wind_names = {"g_vLeafWindTable", "g_vTrunkWindTable", "g_vSmallWindTable", "g_vLargeWindTable",
                  "g_vBranchShakeTable", "g_vLeafShakeTable", "g_fWindStrength", "g_fWindBendFactor"}
    # Per-PSO triangles are not in the pass JSON; use the draw share of the pass's triangles.
    for p in passes:
        c = cls(p)
        if not c:
            continue
        tot = sum(int(v) for v in p["pso_n"].values()) or 1
        for pid, n in p["pso_n"].items():
            d = psos.get(int(pid), {})
            vs, ps = sh.get(d.get("vs", "-")), sh.get(d.get("ps", "-"))
            pso_set[c].add(pid)
            if d.get("ps", "-") != "-":
                ps_set[c].add(d["ps"])
            if d.get("vs", "-") != "-":
                vs_set[c].add(d["vs"])
            tris = p["tris"] * n / tot
            a = agg[c]
            a["draws"] += n
            a["tris"] += tris
            if (ps and ps["discard"]) or d.get("a2c") == "1":
                a["alpha_draws"] += n
                a["alpha_tris"] += tris
            if d.get("a2c") == "1":
                a["a2c_draws"] += n
            if vs and vs["used"] & wind_names:
                a["wind_draws"] += n
                a["wind_tris"] += tris
            if "BLENDINDICES" in d.get("il", "") and vs and not vs["palette"]:
                a["preskinned_draws"] += n
                a["preskinned_tris"] += tris
    print(f"frame {frame} of {tag}: tris are apportioned by draw count within a pass")
    for c, a in agg.items():
        print(f"{c}: {a['draws']} draws {a['tris'] / 1e6:.2f} Mt | alpha-tested {a['alpha_draws']} draws "
              f"({a['alpha_tris'] / 1e6:.2f} Mt; alpha-to-coverage {a['a2c_draws']}) | wind VS {a['wind_draws']} draws "
              f"({a['wind_tris'] / 1e6:.2f} Mt) | pre-skinned {a['preskinned_draws']} draws "
              f"({a['preskinned_tris'] / 1e6:.2f} Mt) | {len(pso_set[c])} PSOs, {len(vs_set[c])} VS, {len(ps_set[c])} PS")
    n_ps = sum(1 for s in sh.values() if s["stage"] == "ps")
    print(f"captured shaders: {len(sh)} ({n_ps} PS, {sum(1 for s in sh.values() if s['stage'] == 'vs')} VS, "
          f"{sum(1 for s in sh.values() if s['stage'] == 'cs')} CS); PS with discard "
          f"{sum(1 for s in sh.values() if s['stage'] == 'ps' and s['discard'])}; VS loading wind tables "
          f"{sum(1 for s in sh.values() if s['stage'] == 'vs' and s['used'] & wind_names)}; "
          f"bone-palette shaders {sum(1 for s in sh.values() if s['palette'])} (all with UAV output: "
          f"{all(s['uav'] for s in sh.values() if s['palette'])})")


if __name__ == "__main__":
    main()
