"""Compare two API descriptions and report what breaks whom.

    python generators/api_diff/diff.py <old.json> <new.json> [--md report.md] [--json report.json]

Severity:
  abi      a compiled application or module stops working: an interface id
           changes, a vtable slot moves or disappears, an exported factory
           disappears, an enum or error code changes value
  api      code no longer compiles or behaves differently: parameter types,
           directions or defaults change, a module-facing member of an
           implementation class changes or disappears
  setup    saved setups read differently: serialized keys change
  info     additions and audience changes

The audience of each finding comes from the newer description (the older
one may not be tagged); a finding on a `core` entry is downgraded to info.
"""

import argparse
import json
import sys
from collections import defaultdict

RANK = {"abi": 0, "api": 1, "setup": 2, "info": 3}


def load(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def sig(m):
    return [(p.get("base"), p.get("pointer", 0), p.get("direction"), p.get("const")) for p in m.get("params", [])]


class Diff:
    def __init__(self, old, new):
        self.old, self.new = old, new
        self.items = []

    def add(self, severity, kind, name, text, audience=None):
        if audience == "core" and severity != "info":
            text += f" (core audience; was {severity})"
            severity = "info"
        self.items.append({"severity": severity, "kind": kind, "name": name, "audience": audience, "text": text})

    def interfaces(self):
        o = {i["name"]: i for i in self.old["interfaces"]}
        n = {i["name"]: i for i in self.new["interfaces"]}
        for name in sorted(set(o) - set(n)):
            self.add("abi", "interface", name, "Interface removed.", None)
        for name in sorted(set(n) - set(o)):
            self.add("info", "interface", name, f"Interface added ({n[name].get('audience') or 'untagged'}).", n[name].get("audience"))
        for name in sorted(set(o) & set(n)):
            a, b = o[name], n[name]
            aud = b.get("audience")
            if a.get("guid") != b.get("guid"):
                self.add("abi", "interface", name, f"Interface id changed {a.get('guid')} -> {b.get('guid')}.", aud)
            if a.get("base") != b.get("base"):
                self.add("abi", "interface", name, f"Base changed {a.get('base')} -> {b.get('base')}; every slot after the base moves.", aud)
            am = a.get("methods", [])
            bm = b.get("methods", [])
            an = [m["name"] for m in am]
            bn = [m["name"] for m in bm]
            bmap = defaultdict(list)
            for m in bm:
                bmap[m["name"]].append(m)
            for i, m in enumerate(am):
                maud = (bmap[m["name"]][0].get("effectiveAudience") if bmap.get(m["name"]) else None) or aud
                if m["name"] not in bn:
                    self.add("abi", "method", f"{name}::{m['name']}", "Method removed; its slot and every later slot are gone or moved.", aud)
                    continue
                j = bn.index(m["name"]) if an.count(m["name"]) == 1 else i
                if j != i:
                    self.add("abi", "method", f"{name}::{m['name']}", f"Vtable slot moved within the interface ({i} -> {j}).", maud)
                nm = bm[j] if j < len(bm) and bm[j]["name"] == m["name"] else bmap[m["name"]][0]
                if sig(m) != sig(nm):
                    self.add("abi", "method", f"{name}::{m['name']}",
                             f"Signature changed: ({', '.join(p.get('cppType', '') for p in m.get('params', []))}) -> "
                             f"({', '.join(p.get('cppType', '') for p in nm.get('params', []))}).", maud)
                else:
                    for pa, pb in zip(m.get("params", []), nm.get("params", [])):
                        if pa.get("default") != pb.get("default"):
                            self.add("api", "method", f"{name}::{m['name']}", f"Default of '{pb.get('name')}' changed {pa.get('default')} -> {pb.get('default')}.", maud)
                        if pa.get("elementType") != pb.get("elementType"):
                            self.add("api", "method", f"{name}::{m['name']}", f"Element type of '{pb.get('name')}' changed {pa.get('elementType')} -> {pb.get('elementType')}.", maud)
            added = [m for m in bn if m not in an]
            for m in added:
                pos = bn.index(m)
                tail = pos < len(an)
                ma = bmap[m][0].get("effectiveAudience") or aud
                if tail:
                    self.add("abi", "method", f"{name}::{m}", f"Method inserted at slot {pos}, before existing methods; later slots move.", ma)
                else:
                    self.add("info", "method", f"{name}::{m}", "Method appended.", ma)

    def factories(self):
        o = {f["createFunction"]: f for f in self.old["factories"] if f.get("createFunction")}
        n = {f["createFunction"]: f for f in self.new["factories"] if f.get("createFunction")}
        for k in sorted(set(o) - set(n)):
            if o[k].get("exported"):
                self.add("abi", "factory", k, "Exported factory removed.", None)
        for k in sorted(set(n) - set(o)):
            self.add("info", "factory", k, "Factory added.", n[k].get("audience"))
        for k in sorted(set(o) & set(n)):
            a, b = o[k], n[k]
            if sig(a) != sig(b) and a.get("exported"):
                self.add("abi", "factory", k, f"Parameters changed: ({', '.join(p.get('cppType', '') for p in a.get('params', []))}) -> "
                                              f"({', '.join(p.get('cppType', '') for p in b.get('params', []))}).", b.get("audience"))
            if a.get("returns") != b.get("returns"):
                self.add("abi", "factory", k, f"Returns {a.get('returns')} -> {b.get('returns')}.", b.get("audience"))
            if a.get("exported") and not b.get("exported"):
                self.add("abi", "factory", k, "No longer exported.", b.get("audience"))

    def enums(self):
        o = {e["name"]: e for e in self.old["enums"]}
        n = {e["name"]: e for e in self.new["enums"]}
        for k in sorted(set(o) & set(n)):
            om = {m["name"]: m.get("value") for m in o[k]["members"]}
            nm = {m["name"]: m.get("value") for m in n[k]["members"]}
            for mem in sorted(set(om) - set(nm)):
                self.add("abi", "enum", f"{k}::{mem}", "Enumerator removed.", n[k].get("audience"))
            for mem in sorted(set(om) & set(nm)):
                if om[mem] != nm[mem]:
                    self.add("abi", "enum", f"{k}::{mem}", f"Value changed {om[mem]} -> {nm[mem]}.", n[k].get("audience"))
            for mem in sorted(set(nm) - set(om)):
                self.add("info", "enum", f"{k}::{mem}", f"Enumerator added ({nm[mem]}).", n[k].get("audience"))
        for k in sorted(set(o) - set(n)):
            self.add("api", "enum", k, "Enum removed.", None)
        oe = {e["name"]: e.get("value") for e in self.old["errors"]}
        ne = {e["name"]: e.get("value") for e in self.new["errors"]}
        for k in sorted(set(oe) & set(ne)):
            if oe[k] != ne[k]:
                self.add("abi", "error", k, f"Error code value changed {oe[k]:#x} -> {ne[k]:#x}.", None)
        for k in sorted(set(oe) - set(ne)):
            self.add("api", "error", k, "Error code removed.", None)
        for k in sorted(set(ne) - set(oe)):
            self.add("info", "error", k, "Error code added.", None)

    def implementations(self):
        o = {c["name"]: c for c in self.old["implementations"]}
        n = {c["name"]: c for c in self.new["implementations"]}
        for k in sorted(set(o) & set(n)):
            b = n[k]
            if b.get("audience") != "module":
                continue
            om = {m["name"]: m for m in o[k].get("members", [])}
            of = defaultdict(list)
            for f in o[k].get("functions", []):
                of[f["name"]].append(f)
            for m in b.get("members", []):
                if m.get("audience") == "module":
                    if m["name"] not in om:
                        self.add("info", "member", f"{k}::{m['name']}", "Module member added.", "module")
                    elif om[m["name"]].get("type") != m.get("type"):
                        self.add("api", "member", f"{k}::{m['name']}", f"Type changed {om[m['name']].get('type')} -> {m.get('type')}.", "module")
            for f in b.get("functions", []):
                if f.get("audience") != "module":
                    continue
                olds = of.get(f["name"])
                if not olds:
                    self.add("info", "function", f"{k}::{f['name']}", "Module function added" + (" (overridable)." if "override" in (f.get("labels") or []) else "."), "module")
                    continue
                if not any([p.get("cppType") for p in x.get("params", [])] == [p.get("cppType") for p in f.get("params", [])] for x in olds):
                    self.add("api", "function", f"{k}::{f['name']}",
                             f"Signature changed ({', '.join(p.get('cppType', '') for p in olds[0].get('params', []))}) -> "
                             f"({', '.join(p.get('cppType', '') for p in f.get('params', []))}); module overrides no longer override.", "module")
            nf = {f["name"] for f in b.get("functions", [])}
            for name, olds in of.items():
                if name not in nf and not olds[0].get("implements") and olds[0].get("virtual") and olds[0].get("access") != "private":
                    self.add("api", "function", f"{k}::{name}", "Virtual hook removed; module overrides compile but are never called (or fail with override).", "module")
            if set(o[k].get("interfaces", [])) - set(b.get("interfaces", [])):
                self.add("api", "class", k, f"No longer implements {', '.join(sorted(set(o[k]['interfaces']) - set(b['interfaces'])))}.", "module")

    def serialization(self):
        o = {e["serializeId"]: e for e in self.old.get("serialization", [])}
        n = {e["serializeId"]: e for e in self.new.get("serialization", [])}
        for k in sorted(set(o) - set(n)):
            self.add("setup", "serialization", k, "Serialize ID no longer written or read.", None)
        for k in sorted(set(o) & set(n)):
            for what in ("keysWritten", "keysRead"):
                a, b = set(o[k].get(what) or []), set(n[k].get(what) or [])
                if a - b:
                    self.add("setup", "serialization", k, f"{what}: no longer {', '.join(sorted(a - b))}.", None)
                if b - a:
                    self.add("info", "serialization", k, f"{what}: now also {', '.join(sorted(b - a))}.", None)

    def run(self):
        self.interfaces()
        self.factories()
        self.enums()
        self.implementations()
        self.serialization()
        self.items.sort(key=lambda x: (RANK[x["severity"]], x["kind"], x["name"]))
        return self.items


def fill(target, source):
    """Copy audiences from another description onto entries that have none."""
    si = {i["name"]: i for i in source["interfaces"]}
    for i in target["interfaces"]:
        src = si.get(i["name"])
        if src and not i.get("audience"):
            i["audience"] = src.get("audience")
        sm = {m["name"]: m for m in (src or {}).get("methods", [])}
        for m in i.get("methods", []):
            if not m.get("effectiveAudience"):
                m["effectiveAudience"] = (sm.get(m["name"]) or {}).get("effectiveAudience") or i.get("audience")
    sf = {f.get("createFunction"): f for f in source["factories"]}
    for f in target["factories"]:
        if not f.get("audience") and f.get("createFunction") in sf:
            f["audience"] = sf[f["createFunction"]].get("audience")
    se = {e["name"]: e for e in source["enums"]}
    for e in target["enums"]:
        if not e.get("audience") and e["name"] in se:
            e["audience"] = se[e["name"]].get("audience")
    sc = {c["name"]: c for c in source["implementations"]}
    for c in target["implementations"]:
        src = sc.get(c["name"])
        if not src:
            continue
        c.setdefault("audience", src.get("audience"))
        sm = {m["name"]: m for m in src.get("members", [])}
        for m in c.get("members", []):
            if not m.get("audience") and m["name"] in sm:
                m["audience"] = sm[m["name"]].get("audience")
        sfn = {f["name"]: f for f in src.get("functions", [])}
        for f in c.get("functions", []):
            if not f.get("audience") and f["name"] in sfn:
                f["audience"], f["labels"] = sfn[f["name"]].get("audience"), sfn[f["name"]].get("labels")


def markdown(items, old, new):
    count = defaultdict(int)
    for x in items:
        count[x["severity"]] += 1
    lines = [f"# openDAQ API changes: {old.get('openDaqVersion')} ({old.get('source', {}).get('commit')}) to "
             f"{new.get('openDaqVersion')} ({new.get('source', {}).get('commit')})", "",
             "Generated by `generators/api_diff/diff.py` from two API descriptions. Audiences come from the newer one.", "",
             "| Severity | Findings |", "| -------- | -------- |"]
    lines += [f"| {s} | {count[s]} |" for s in RANK]
    for s in RANK:
        group = [x for x in items if x["severity"] == s]
        if not group:
            continue
        lines += ["", f"## {s}", "", "| Kind | Name | Audience | Change |", "| ---- | ---- | -------- | ------ |"]
        lines += [f"| {x['kind']} | `{x['name']}` | {x['audience'] or ''} | {x['text'].replace('|', '/')} |" for x in group]
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--md")
    ap.add_argument("--json")
    ap.add_argument("--audiences", help="description whose tags supply audiences the compared files lack (e.g. main)")
    a = ap.parse_args()
    old, new = load(a.old), load(a.new)
    if a.audiences:
        fill(new, load(a.audiences))
    items = Diff(old, new).run()
    if a.md:
        with open(a.md, "w", encoding="utf-8", newline="\n") as f:
            f.write(markdown(items, old, new))
    if a.json:
        with open(a.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(items, f, indent=1)
    c = defaultdict(int)
    for x in items:
        c[x["severity"]] += 1
    print(", ".join(f"{k}: {c[k]}" for k in RANK))
    return 1 if c["abi"] else 0


if __name__ == "__main__":
    sys.exit(main())
