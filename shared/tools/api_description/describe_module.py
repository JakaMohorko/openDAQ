"""Describe a module repository in the API description format, relative to the core.

    python describe_module.py <module-dir> [--name NAME] [--out output/modules/NAME.json]

The file holds the module's own interfaces, factories, enums and
implementation classes (parsed with the core for context, so bases and
interface ids resolve), references core entries by name, and adds
`coreUsage`: the core classes the module derives from, the core hooks it
overrides, the core members and interfaces it uses, each with the core's
audience tag, and `violations` where the module depends on `core` API, and `interfaceOverrides`
where it replaces the core's implementation of an interface method instead of
using a hook.
Needs output/opendaq_api.json of the tagged core.
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from apidesc.model import Model, split_template  # noqa: E402
from apidesc.emit import Emitter  # noqa: E402
from apidesc.usage import Usage  # noqa: E402


def describe(module_dir, name, root, core_api):
    rel = os.path.relpath(os.path.abspath(module_dir), root).replace("\\", "/")
    m = Model(root, scan_dirs=["core", rel], exclude=("/tests/", "/test/", "/external/", "/build", "/_deps/")).scan()
    vfile = os.path.join(root, "opendaq_version")
    doc = Emitter(m).build(open(vfile).read().strip() if os.path.exists(vfile) else None)
    prefix = rel + "/"

    def own(items):
        return [x for x in items if (x.get("file") or "").startswith(prefix)]

    out = {
        "formatVersion": doc["formatVersion"],
        "module": name,
        "source": {"path": rel},
        "extends": {"openDaqVersion": core_api.get("openDaqVersion"), "commit": core_api.get("source", {}).get("commit")},
        "interfaces": own(doc["interfaces"]),
        "factories": own(doc["factories"]),
        "enums": own(doc["enums"]),
        "implementations": own(doc["implementations"]),
    }
    # usage of the core, judged by the core's tags
    u = Usage(core_api)
    aliases = {}
    for an, a in m.aliases.items():
        t = split_template(a["value"])[0]
        for _ in range(5):
            if t in u.impl or t not in m.aliases:
                break
            t = split_template(m.aliases[t]["value"])[0]
        if t in u.impl:
            aliases[an] = t
    u.add_aliases(aliases)
    u.scan(os.path.abspath(module_dir), name, "module", ("tests", "test", "external"))
    u.finish()
    ev = u.to_json()
    impl = {c["name"]: c for c in core_api["implementations"]}
    intf = {i["name"]: i for i in core_api["interfaces"]}
    uses, violations = [], []
    for cls, members in ev["members"].items():
        rec = impl.get(cls, {})
        by_name = {x["name"]: x for x in rec.get("members", []) + rec.get("functions", [])}
        for mem, e in members.items():
            tag = by_name.get(mem, {})
            entry = {"class": cls, "member": mem, "how": e["how"], "audience": tag.get("audience") or "core (untagged)",
                     "labels": tag.get("labels"), "examples": e["examples"]}
            uses.append(entry)
            if entry["audience"].startswith("core"):
                violations.append(entry)
            elif "override" in e["how"] and "override" not in (tag.get("labels") or []):
                violations.append(dict(entry, problem="overrides a member that is not tagged override"))
    for iname, kinds in ev["interfaces"].items():
        if intf.get(iname, {}).get("audience") == "core":
            violations.append({"interface": iname, "audience": "core",
                               "examples": ev["examples"].get(f"i|{iname}|module", [])})
    # overriding the core's implementation of an interface method bypasses the hooks the core offers
    interface_overrides = []
    for c in out["implementations"]:
        core_chain = [b for b in c.get("baseChain", []) if b in impl]
        for f in c.get("functions", []):
            if not (f.get("override") and f.get("implements")):
                continue
            for b in core_chain:
                if any(x["name"] == f["name"] and not x.get("pure") for x in impl[b].get("functions", [])):
                    interface_overrides.append({"class": c["name"], "method": f"{f['implements']}::{f['name']}",
                                                "coreImplementation": b, "where": f"{c['file']}:{f['line']}"})
                    break
    out["coreUsage"] = {
        "interfaceOverrides": interface_overrides,
        "derivedFrom": {k: sorted({c for _, c in v}) for k, v in ev["derived"].items()},
        "interfaces": sorted(ev["interfaces"]),
        "factories": sorted(ev["factories"]),
        "members": uses,
        "violations": violations,
    }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module_dir")
    ap.add_argument("--name")
    ap.add_argument("--root", default=os.path.normpath(os.path.join(HERE, "..", "..", "..")))
    ap.add_argument("--api", default=os.path.join(HERE, "output", "opendaq_api.json"))
    ap.add_argument("--out")
    a = ap.parse_args()
    name = a.name or os.path.basename(os.path.normpath(a.module_dir))
    with open(a.api, encoding="utf-8") as f:
        core_api = json.load(f)
    out = describe(a.module_dir, name, os.path.abspath(a.root), core_api)
    path = a.out or os.path.join(HERE, "output", "modules", f"{name}.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1)
    cu = out["coreUsage"]
    print(f"{path}: {len(out['interfaces'])} interfaces, {len(out['factories'])} factories, "
          f"{len(out['implementations'])} classes; derives from {', '.join(sorted(cu['derivedFrom'])) or 'nothing'}; "
          f"{len(cu['members'])} core members used, {len(cu['violations'])} violations")
    for o in cu["interfaceOverrides"][:15]:
        print(f"  overrides {o['coreImplementation']}'s {o['method']} in {o['class']} ({o['where']})")
    for v in cu["violations"][:15]:
        print("  violation:", v.get("class", ""), v.get("member", v.get("interface")), v.get("how", ""), v.get("problem", v["audience"]),
              (v.get("examples") or [""])[0])


if __name__ == "__main__":
    main()
