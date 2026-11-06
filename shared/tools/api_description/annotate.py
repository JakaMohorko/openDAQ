"""Write audience tags into the headers.

    python annotate.py                      apply config/classification.json
    python annotate.py --apply edits.json   apply a review-page export first
    python annotate.py --dry-run            print what would change

Each tag is one `// [api(<audience>[, <labels>])]` line directly above the
declaration (spec 0010). An existing api line in that position is
replaced. Method tags are written only where the method differs from its
interface; elsewhere an existing method tag is removed. Locations are
looked up in a fresh parse of the tree, so the tool can run repeatedly.
"""

import argparse
import json
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from apidesc.model import Model  # noqa: E402
from apidesc.emit import Emitter  # noqa: E402

TAG_RE = re.compile(r"^\s*//\s*\[api\([^)]*\)\]\s*$")


def tag_text(aud, labels):
    return "// [api(" + ", ".join([aud] + list(labels or [])) + ")]"


def locate(api, root):
    """key -> list of (rel file, line)."""
    loc = defaultdict(list)
    for i in api["interfaces"]:
        loc[f"interface:{i['name']}"].append((i["file"], i["line"]))
        for m in i.get("methods", []):
            loc[f"method:{i['name']}.{m['name']}"].append((i["file"], m["line"]))
    for f in api["factories"]:
        if f.get("createFunction"):
            loc[f"factory:{f['createFunction']}"].append((f["file"], f["line"]))
    for f in api.get("cppFactories", []):
        loc[f"cppfactory:{f['header']}#{f['name']}/{len(f.get('params', []))}"].append((f["file"], f["line"]))
    for e in api["enums"]:
        if e.get("header") and not e.get("outer"):
            loc[f"enum:{e['name']}"].append((e["file"], e["line"]))
    for sp in api.get("smartPointers", []):
        loc[f"smartptr:{sp['name']}"].append((sp["file"], sp["line"]))
    for c in api["implementations"]:
        loc[f"class:{c['name']}"].append((c["file"], c["line"]))
        for mem in c.get("members", []):
            loc[f"member:{c['name']}.{mem['name']}"].append((c["file"], mem["line"]))
        for fn in c.get("functions", []):
            if not fn.get("implements"):
                loc[f"function:{c['name']}.{fn['name']}"].append((c["file"], fn["line"]))
    return loc


def apply_edits(cls, edits):
    """Merge a review-page export ({key: {audience, labels}}) into the classification."""
    by_key = {e["key"]: e for e in cls["entries"]}
    n = 0
    for key, ch in edits.get("changes", edits).items():
        if key in by_key:
            e = by_key[key]
            if "audience" in ch:
                e["audience"] = ch["audience"] or None
            if "labels" in ch:
                e["labels"] = ch["labels"]
            if ch.get("note"):
                e["reviewNote"] = ch["note"]
            e["reason"] = "Reviewed." if not ch.get("note") else "Reviewed: " + ch["note"]
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.normpath(os.path.join(HERE, "..", "..", "..")))
    ap.add_argument("--classification", default=os.path.join(HERE, "config", "classification.json"))
    ap.add_argument("--apply", help="review-page export to merge first")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--kinds", nargs="*", help="only these kinds (interface, method, factory, cppfactory, enum, class, member, function)")
    args = ap.parse_args()
    cls = json.load(open(args.classification, encoding="utf-8"))
    if args.apply:
        n = apply_edits(cls, json.load(open(args.apply, encoding="utf-8")))
        print(f"merged {n} reviewed entries")
        with open(args.classification, "w", encoding="utf-8", newline="\n") as f:
            json.dump(cls, f, indent=1)
            f.write("\n")
    model = Model(args.root).scan()
    vfile = os.path.join(args.root, "opendaq_version")
    api = Emitter(model).build(open(vfile).read().strip() if os.path.exists(vfile) else None)
    loc = locate(api, args.root)
    want = defaultdict(dict)  # file -> line -> tag or None (remove)
    missing = []
    for e in cls["entries"]:
        if args.kinds and e["kind"] not in args.kinds:
            continue
        key = e["key"]
        places = loc.get(key)
        if not places:
            missing.append(key)
            continue
        aud = e.get("audience")
        if e["kind"] == "method" and (aud is None or (aud == e.get("interfaceAudience") and not e.get("labels"))):
            tag = None
        else:
            tag = tag_text(aud, e.get("labels")) if aud else None
        for rel, line in places:
            if "/src/" in rel and e["kind"] in ("class", "member", "function"):
                continue
            want[rel][line] = tag
    if not args.kinds:
        # tags that no longer sit above a described declaration (the declaration was reclassified away)
        orphans = 0
        for d in ("core",):
            for dp, _, names in os.walk(os.path.join(args.root, d)):
                for n in names:
                    if not n.endswith(".h"):
                        continue
                    rel = os.path.relpath(os.path.join(dp, n), args.root).replace("\\", "/")
                    with open(os.path.join(dp, n), encoding="utf-8", errors="replace") as f:
                        rows = f.read().splitlines()
                    for i, row in enumerate(rows):
                        if TAG_RE.match(row) and (i + 2) not in want.get(rel, {}):
                            want[rel][i + 2] = None
                            orphans += 1
        print(f"{orphans} orphaned tags to remove")
    changed = 0
    for rel, lines in sorted(want.items()):
        path = os.path.join(args.root, rel)
        raw = open(path, "rb").read()
        crlf = b"\r\n" in raw
        text = raw.decode("utf-8")
        trailing_nl = text.endswith("\n")
        rows = text.split("\r\n" if crlf else "\n")
        if trailing_nl:
            rows = rows[:-1]
        # bottom-up so earlier line numbers stay valid
        for line in sorted(lines, reverse=True):
            tag = lines[line]
            idx = line - 1
            if idx >= len(rows):
                continue
            indent = re.match(r"^\s*", rows[idx]).group(0)
            has = idx > 0 and TAG_RE.match(rows[idx - 1])
            if tag is None:
                if has:
                    del rows[idx - 1]
                continue
            new = indent + tag
            if has:
                rows[idx - 1] = new
            else:
                rows.insert(idx, new)
        out = ("\r\n" if crlf else "\n").join(rows) + (("\r\n" if crlf else "\n") if trailing_nl else "")
        if out != text:
            changed += 1
            if not args.dry_run:
                with open(path, "wb") as f:
                    f.write(out.encode("utf-8"))
    print(f"{changed} files {'would change' if args.dry_run else 'changed'}; {len(missing)} keys not found")
    for k in missing[:20]:
        print("  missing:", k)


if __name__ == "__main__":
    main()
