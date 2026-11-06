"""Collect usage evidence from applications and modules.

    python scan_usage.py [--sources sources.json] [--out output/usage.json]

sources.json lists source trees with a kind ('application' or 'module');
paths are relative to the openDAQ checkout root. Missing trees are
skipped with a note.
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from apidesc.usage import Usage  # noqa: E402
from apidesc.model import split_template  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    root_default = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
    ap.add_argument("--root", default=root_default)
    ap.add_argument("--api", default=os.path.join(HERE, "output", "opendaq_api.json"))
    ap.add_argument("--sources", default=os.path.join(HERE, "config", "usage_sources.json"))
    ap.add_argument("--out", default=os.path.join(HERE, "output", "usage.json"))
    args = ap.parse_args()
    api = json.load(open(args.api, encoding="utf-8"))
    sources = json.load(open(args.sources, encoding="utf-8"))
    u = Usage(api)
    # implementation aliases from the core (Device = GenericDevice<...>, FunctionBlock = FunctionBlockImpl<>)
    sys.path.insert(0, HERE)
    from apidesc.model import Model
    m = Model(args.root).scan()
    aliases = {}
    for name, a in m.aliases.items():
        t = split_template(a["value"])[0]
        for _ in range(5):
            if t in u.impl or t not in m.aliases:
                break
            t = split_template(m.aliases[t]["value"])[0]
        if t in u.impl:
            aliases[name] = t
    u.add_aliases(aliases)
    scanned = []
    for s in sources["sources"]:
        path = os.path.normpath(os.path.join(args.root, s["path"]))
        if not os.path.isdir(path):
            print(f"skip (missing): {s['path']}", file=sys.stderr)
            continue
        u.scan(path, s["name"], s["kind"])
        scanned.append(s)
    u.finish()
    out = u.to_json()
    out["sources"] = scanned
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    print(f"{args.out}: {len(out['interfaces'])} interfaces, {len(out['methods'])} method names, "
          f"{len(out['factories'])} factories, members of {len(out['members'])} implementations")


if __name__ == "__main__":
    main()
