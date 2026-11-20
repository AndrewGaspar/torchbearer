#!/usr/bin/env python3
"""List the pipeline states of selected passes of a census frame (docs/raytracing.md).

  rt_pass_psos.py RUN_DIR TAG INDEX.json FRAME PASS[,PASS...]

For each pass (numbering as rt_census.py --passes prints it): every pipeline state used, its
draw count, effect / technique of the pixel (else vertex) shader, render-target formats, blend
enable and write mask of RT0, alpha-to-coverage, cull, depth state, and the g_* names of its
shaders.
"""
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import rt_census as C  # noqa: E402


def main():
    run, tag, index, frame, which = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
    want = {int(x) for x in which.split(",")}
    psos, shaders = C.load_pso(run)
    by_bc, by_dig = C.load_index(index)
    info = C.shader_info(run, shaders, by_bc, by_dig)
    tmp = os.path.join(run, f"rt-passes-{tag}-{frame}.json")
    old = sys.argv
    sys.argv = ["x", run, tag, index, "--frame", str(frame), "--json", tmp]
    import io
    import contextlib
    with contextlib.redirect_stdout(io.StringIO()):
        C.main()
    sys.argv = old
    passes = json.load(open(tmp))["passes"]
    os.remove(tmp)
    for i in sorted(want):
        p = passes[i]
        print(f"pass {i}: RT {p['key'][0]} | DS {p['key'][1]} ({p['draws']} draws)")
        for pid in p["psos"]:
            d = psos.get(int(pid), {})
            eff = C.effect_of(d, info)
            nm = set()
            for k in ("vs", "ps", "cs"):
                h = d.get(k)
                if h and info.get(h):
                    nm |= set(info[h]["names"])
            n = p.get("pso_n", {}).get(str(pid), 0)
            print(f"  pso {pid} ({n} draws): {eff[0]}:{eff[1]} vs={d.get('vs')} ps={d.get('ps')} rtv={d.get('rtv')} dsv={d.get('dsv')} "
                  f"blend0={d.get('blend0')} wmask0={d.get('wmask0')} a2c={d.get('a2c')} cull={d.get('cull')} "
                  f"depth={d.get('depth')}/{d.get('dwrite')}/{d.get('dfunc')} dbias={d.get('dbias')} so={d.get('so')}")
            print(f"      il={d.get('il')}")
            print(f"      names: {' '.join(sorted(nm))}")


if __name__ == "__main__":
    main()
