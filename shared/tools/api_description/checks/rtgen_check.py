"""Run RTGen over every interface/factory header and record its output.

    python checks/rtgen_check.py <outdir>
    python checks/rtgen_check.py --compare <outdirA> <outdirB>

Used to prove that the api tags do not change what RTGen generates: run
once on the untagged tree and once on the tagged tree, then compare. The
only expected differences are RTGen's "Ignoring unknown rtComment
function: api" warning and shifted line numbers in the logs.
"""

import argparse
import concurrent.futures as cf
import filecmp
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
RTGEN = os.path.join(ROOT, "shared", "tools", "RTGen", "bin", "rtgen.exe")
LIBS = {"core/coretypes": "CoreTypes", "core/corecontainers": "CoreTypes", "core/coreobjects": "CoreObjects", "core/opendaq": "OpenDAQ"}
LANGS = ["cpp", "python", "csharp", "c", "delphi"]


def headers():
    out = []
    for base, lib in LIBS.items():
        for dp, _, names in os.walk(os.path.join(ROOT, base)):
            if "include" not in dp.replace("\\", "/").split("/"):
                continue
            for n in names:
                if not n.endswith(".h"):
                    continue
                p = os.path.join(dp, n)
                src = open(p, encoding="utf-8", errors="replace").read()
                if re.search(r"DECLARE_OPENDAQ_INTERFACE|OPENDAQ_DECLARE_CLASS_FACTORY", src):
                    out.append((os.path.relpath(p, ROOT).replace("\\", "/"), lib))
    return sorted(out)


def run_one(args):
    rel, lib, lang, outdir = args
    dest = os.path.join(outdir, lang, rel.replace("/", "__"))
    os.makedirs(dest, exist_ok=True)
    cmd = [RTGEN, "-nt", "--lang", lang, "--library", lib, "--namespace", "daq", "-d", dest, "-f", dest,
           "--source", os.path.join(ROOT, rel)]
    if lang == "cpp":
        cmd.append("-p")
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    log = (r.stdout + r.stderr)
    return rel, lang, r.returncode, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--compare", nargs=2)
    ap.add_argument("--jobs", type=int, default=6)
    a = ap.parse_args()
    if a.compare:
        return compare(*a.compare)
    hs = headers()
    jobs = [(rel, lib, lang, a.outdir) for rel, lib in hs for lang in LANGS]
    results = {}
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for rel, lang, rc, log in ex.map(run_one, jobs):
            results[f"{lang}:{rel}"] = {"rc": rc, "log": log}
    with open(os.path.join(a.outdir, "results.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, indent=1)
    bad = sum(1 for r in results.values() if r["rc"] != 0)
    print(f"{len(hs)} headers x {len(LANGS)} languages, {bad} non-zero exits")


def compare(a, b):
    ra = json.load(open(os.path.join(a, "results.json"), encoding="utf-8"))
    rb = json.load(open(os.path.join(b, "results.json"), encoding="utf-8"))
    diffs = []
    for k in sorted(set(ra) | set(rb)):
        if k not in ra or k not in rb:
            diffs.append(f"only in one run: {k}")
            continue
        if ra[k]["rc"] != rb[k]["rc"]:
            diffs.append(f"exit code {ra[k]['rc']} -> {rb[k]['rc']}: {k}")
        # inserted tag lines shift the line numbers in error messages of runs that already fail
        la = [re.sub(r"line \d+:\d+", "line N", ln) for ln in ra[k]["log"].splitlines() if "unknown rtComment function: api" not in ln]
        lb = [re.sub(r"line \d+:\d+", "line N", ln) for ln in rb[k]["log"].splitlines() if "unknown rtComment function: api" not in ln]
        if la != lb:
            diffs.append(f"log differs beyond the api warning: {k}")
    n_files = 0
    for dp, _, names in os.walk(a):
        for n in names:
            if n == "results.json":
                continue
            pa = os.path.join(dp, n)
            pb = os.path.join(b, os.path.relpath(pa, a))
            n_files += 1
            if not os.path.exists(pb):
                diffs.append(f"missing in second run: {os.path.relpath(pa, a)}")
            elif not filecmp.cmp(pa, pb, shallow=False):
                diffs.append(f"generated file differs: {os.path.relpath(pa, a)}")
    for dp, _, names in os.walk(b):
        for n in names:
            pb = os.path.join(dp, n)
            if n != "results.json" and not os.path.exists(os.path.join(a, os.path.relpath(pb, b))):
                diffs.append(f"new in second run: {os.path.relpath(pb, b)}")
    print(f"compared {n_files} generated files and {len(ra)} runs: {len(diffs)} differences")
    for d in diffs[:50]:
        print("  " + d)
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
