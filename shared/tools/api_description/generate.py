"""Generate the openDAQ API description JSON from the source tree.

    python generate.py [--root <openDAQ checkout>] [--out <file>]
"""

import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from apidesc.model import Model  # noqa: E402
from apidesc.emit import Emitter  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.normpath(os.path.join(HERE, "..", "..", "..")))
    ap.add_argument("--out", default=os.path.join(HERE, "output", "opendaq_api.json"))
    ap.add_argument("--dirs", nargs="*", default=["core"])
    args = ap.parse_args()
    t = time.time()
    model = Model(args.root, scan_dirs=args.dirs).scan()
    vfile = os.path.join(args.root, "opendaq_version")
    version = open(vfile).read().strip() if os.path.exists(vfile) else None
    doc = Emitter(model).build(version)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False)
        f.write("\n")
    for p in model.problems:
        print("parse:", p, file=sys.stderr)
    print(f"{args.out}: {len(doc['interfaces'])} interfaces, {len(doc['factories'])} factories, "
          f"{len(doc['implementations'])} implementations, {len(doc['enums'])} enums, {len(doc['errors'])} errors "
          f"in {time.time() - t:.1f}s")


if __name__ == "__main__":
    main()
