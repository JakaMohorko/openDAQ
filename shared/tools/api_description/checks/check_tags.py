"""Fail when a declaration that needs an audience tag has none (spec 0010, Enforcement).

    python checks/check_tags.py [--api output/opendaq_api.json]

Needs a tag: every interface, exported factory, C++ factory, smart pointer,
enum in an installed header and installed implementation class. Interface
methods follow their interface and need none. Exit code 1 lists the gaps.
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AUDIENCES = {"application", "module", "core"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", default=os.path.join(HERE, "..", "output", "opendaq_api.json"))
    a = ap.parse_args()
    with open(a.api, encoding="utf-8") as f:
        api = json.load(f)
    gaps, bad = [], []

    def need(kind, name, rec):
        aud = rec.get("audience")
        where = f"{rec.get('file')}:{rec.get('line')}"
        if aud is None:
            gaps.append(f"{where}: {kind} {name} has no [api(...)] tag")
        elif aud not in AUDIENCES:
            bad.append(f"{where}: {kind} {name} has unknown audience '{aud}'")
    for i in api["interfaces"]:
        need("interface", i["name"], i)
        for m in i.get("methods", []):
            if m.get("audience") and m["audience"] not in AUDIENCES:
                bad.append(f"{i['file']}:{m['line']}: method {i['name']}::{m['name']} has unknown audience '{m['audience']}'")
    for f in api["factories"]:
        if f.get("createFunction") and f.get("exported"):
            need("factory", f["name"], f)
    for f in api.get("cppFactories", []):
        need("C++ factory", f["name"], f)
    for s in api.get("smartPointers", []):
        need("smart pointer", s["name"], s)
    for e in api["enums"]:
        if e.get("header") and not e.get("outer"):
            need("enum", e["name"], e)
    for c in api["implementations"]:
        if c.get("installed"):
            need("implementation class", c["name"], c)
    for line in gaps + bad:
        print(line)
    print(f"{len(gaps)} untagged, {len(bad)} invalid")
    return 1 if gaps or bad else 0


if __name__ == "__main__":
    sys.exit(main())
