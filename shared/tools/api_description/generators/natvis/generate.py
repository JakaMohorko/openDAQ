"""Generate Visual Studio natvis files from the API description.

    python generators/natvis/generate.py [--api ...] [--out output/natvis]
    python generators/natvis/generate.py --check-existing <openDAQ root>

One file per library. For every implementation class: a display string
(the component's local ID, the value of a coretype, or the main
interface), its own members, the reference count, and the base class as
an expanded item, cast with the real template arguments. Base casts and
member lists come from the description, so they follow the code: no
hand-maintained template argument lists.

--check-existing reads the hand-written natvis files and reports member
names and base casts that no longer match the implementation classes.
"""

import argparse
import json
import os
import re
import sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, TOOL)
from apidesc.model import split_template  # noqa: E402

MAX_EXTRA = 3  # variadic packs are matched with up to this many arguments


def q(s):
    return escape(s, {'"': "&quot;"})


class Natvis:
    def __init__(self, api):
        self.api = api
        self.impl = {i["name"]: i for i in api["implementations"]}
        self.intf = {i["name"]: i for i in api["interfaces"]}
        self.alias = {a["name"]: a for a in api.get("typeAliases", [])}
        self.coretypes = {i["name"] for i in api["interfaces"] if i.get("coretype")}
        self.known = set(self.impl) | set(self.intf) | set(self.alias)
        self.stats = {"types": 0, "patterns": 0, "baseCasts": 0}

    # ---------------------------------------------------------------- names
    def qualify(self, expr):
        """Prefix openDAQ type names with daq:: as the debugger spells them."""
        def rep(m):
            n = m.group(0)
            return "daq::" + n if n in self.known or re.match(r"^(I[A-Z]\w*|\w+Ptr)$", n) and not n.startswith("std") else n
        return re.sub(r"(?<![\w:$])[A-Za-z_]\w*", rep, expr)

    def substitute(self, expr, bind):
        """Replace template parameter names (and packs `X...`) in a type expression."""
        def rep(m):
            n, dots = m.group(1), m.group(2)
            if n in bind:
                v = bind[n]
                return ",".join(v) if isinstance(v, list) else v
            return n + (dots or "")
        out = re.sub(r"\b([A-Za-z_]\w*)\s*(\.\.\.)?", rep, expr)
        out = re.sub(r",\s*,", ",", out)
        out = re.sub(r"<\s*,", "<", out)
        out = re.sub(r",\s*>", ">", out)
        return out.replace(" ", "")

    def resolve(self, name, args):
        """Follow aliases to the class the debugger sees."""
        for _ in range(6):
            if name in self.impl:
                return name, args
            a = self.alias.get(name)
            if not a:
                return None, None
            bind = {}
            for i, tp in enumerate(a.get("templateParams") or []):
                if tp.get("variadic"):
                    bind[tp["name"]] = args[i:]
                    break
                bind[tp["name"]] = args[i] if i < len(args) else tp.get("default", "")
            target = self.substitute(a["value"], bind)
            name, args = split_template(target)
        return None, None

    # ---------------------------------------------------------------- one class
    def patterns(self, rec):
        """(type pattern, binding of template params to $T placeholders)."""
        tps = rec.get("templateParams") or []
        if not tps:
            return [(f"daq::{rec['name']}", {})]
        fixed = [t for t in tps if not t.get("variadic")]
        var = [t for t in tps if t.get("variadic")]
        out = []
        extras = range(0, MAX_EXTRA + 1) if var else [0]
        for extra in extras:
            n = len(fixed) + extra
            bind = {}
            for i, t in enumerate(fixed):
                bind[t["name"]] = f"$T{i + 1}"
            if var:
                bind[var[0]["name"]] = [f"$T{len(fixed) + j + 1}" for j in range(extra)]
            stars = ",".join("*" * 1 for _ in range(n))
            out.append((f"daq::{rec['name']}<{stars}>", bind))
        return out

    def chain_member(self, rec, name):
        for c in [rec["name"]] + rec.get("baseChain", []):
            r = self.impl.get(c)
            if r and any(m["name"] == name and not m.get("static") for m in r.get("members", [])):
                return r, next(m for m in r["members"] if m["name"] == name)
        return None, None

    def display(self, rec):
        own = [m for m in rec.get("members", []) if not m.get("static")]
        core = [i for i in rec.get("interfaces", []) if i in self.coretypes and i != "IBaseObject"]
        if core and own:
            if core[0] == "IString" and any(m["name"] == "str" for m in own):
                return "{str,s8}"
            vals = [m for m in own if m["name"] in ("value", "numerator", "denominator", "list", "hashTable", "real", "imaginary")]
            if vals:
                return "{{ " + ", ".join(f"{m['name']}={{{m['name']}}}" for m in vals) + " }}"
        main = next((i for i in rec.get("interfaces", []) if i not in ("IBaseObject", "IInspectable", "ISupportsWeakRef")), None)
        kind = main[1:] if main else rec["name"]
        r, m = self.chain_member(rec, "localId")
        if m:
            return f"{{{{ {kind} {{localId}} }}}}"
        r, m = self.chain_member(rec, "className")
        if m and "PropertyObject" in kind:
            return f"{{{{ {kind} {{className}} }}}}"
        return f"{{{{ {kind} }}}}"

    def type_xml(self, rec):
        xml = []
        r, rc = self.chain_member(rec, "refCount")
        rc_item = None
        if rc:
            t = rc["type"]
            rc_item = "*refCount" if ("*" in t or "unique_ptr" in t) else "refCount"
        disp = self.display(rec)
        for pattern, bind in self.patterns(rec):
            self.stats["patterns"] += 1
            body = [f'  <Type Name="{q(pattern)}">', f"    <DisplayString>{q(disp)}</DisplayString>", "    <Expand>"]
            if rc_item:
                body.append(f'      <Item Name="[refCount]">{q(rc_item)}</Item>')
            for m in rec.get("members", []):
                if m.get("static"):
                    continue
                label = m["name"]
                if m.get("audience") == "module":
                    label += " [module]"
                body.append(f'      <Item Name="{q(label)}">{q(m["name"])}</Item>')
            for b in rec.get("bases", []):
                bname, bargs = split_template(b)
                sub_args = [self.substitute(a, bind) for a in bargs]
                sub_args = [a for a in sub_args if a]
                target, targs = self.resolve(bname, sub_args)
                if not target:
                    continue
                expr = f"{target}<{','.join(targs)}>" if (targs or self.impl[target].get("templateParams")) else target
                cast = self.qualify(expr)
                body.append(f"      <ExpandedItem>({q(cast)}*)this,nd</ExpandedItem>")
                self.stats["baseCasts"] += 1
            body += ["    </Expand>", "  </Type>"]
            xml += body
        self.stats["types"] += 1
        return xml

    def smart_ptr_xml(self, i):
        name = i["smartPtr"]
        pat = f"daq::{name}<*>" if i.get("templated") else f"daq::{name}"
        return [f'  <Type Name="{q(pat)}">', '    <DisplayString Condition="object == nullptr">empty</DisplayString>',
                "    <DisplayString>{object,na}</DisplayString>", "    <Expand>", "      <ExpandedItem>object,na</ExpandedItem>",
                "    </Expand>", "  </Type>"]

    def build(self):
        files = {}
        for lib in self.api["libraries"]:
            xml = ['<?xml version="1.0" encoding="utf-8"?>',
                   f"<!-- Generated from the openDAQ API description ({self.api.get('openDaqVersion')} at "
                   f"{self.api.get('source', {}).get('commit')}). Do not edit. -->",
                   '<AutoVisualizer xmlns="http://schemas.microsoft.com/vstudio/debugger/natvis/2010">']
            for name in sorted(self.impl):
                rec = self.impl[name]
                if rec.get("library") == lib:
                    xml += self.type_xml(rec)
            seen = set()
            for i in sorted(self.intf.values(), key=lambda x: x["name"]):
                if i.get("library") == lib and i.get("smartPtr") and i["smartPtr"] not in seen and i["smartPtr"] not in ("ObjectPtr",):
                    seen.add(i["smartPtr"])
                    xml += self.smart_ptr_xml(i)
            xml.append("</AutoVisualizer>")
            files[lib] = "\n".join(xml) + "\n"
        return files


def check_existing(api, root):
    """Find members and casts in the hand-written natvis files that no longer exist."""
    impl = {i["name"]: i for i in api["implementations"]}
    report = []
    for dp, _, names in os.walk(os.path.join(root, "core")):
        for n in names:
            if not n.endswith(".natvis"):
                continue
            path = os.path.join(dp, n)
            rel = os.path.relpath(path, root).replace("\\", "/")
            text = open(path, encoding="utf-8").read()
            for m in re.finditer(r'<Type Name="daq::(\w+)([^"]*)">(.*?)</Type>', text, re.S):
                cname, body = m.group(1), m.group(3)
                rec = impl.get(cname)
                if not rec:
                    report.append(f"{rel}: type daq::{cname} is not an implementation class in the description")
                    continue
                members = set()
                for c in [cname] + rec.get("baseChain", []):
                    for mem in (impl.get(c) or {}).get("members", []):
                        members.add(mem["name"])
                for item in re.finditer(r"<(?:Item[^>]*|ExpandedItem[^>]*|DisplayString[^>]*|StringView)>(.*?)</", body):
                    expr = item.group(1).replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")
                    for cast in re.findall(r"\(daq::(\w+)<([^)]*)>\*\)this", expr):
                        base, args = cast[0], cast[1]
                        actual = [split_template(b) for b in rec.get("bases", [])]
                        if not any(base == a[0] or base in rec.get("baseChain", []) for a in actual):
                            report.append(f"{rel}: daq::{cname} casts to daq::{base}, which is not one of its bases ({', '.join(rec.get('bases', []))})")
                        else:
                            for a in actual:
                                if a[0] == base:
                                    want = [x.replace("...", "").strip() for x in a[1]]
                                    have = [x.replace("daq::", "") for x in args.split(",")]
                                    fixed = [w for w in want if not w.startswith("$")]
                                    named = [h for h in have if not h.startswith("$")]
                                    if [x for x in named if x not in want] :
                                        report.append(f"{rel}: daq::{cname} casts to daq::{base}<{args}>, but the base is {base}<{', '.join(a[1])}>; the cast never matches the real type")
                    plain = re.sub(r"\(daq::[^)]*\)this", "", expr)
                    # format specifiers: str,s8  object,na  x,view(simple)  hash,x
                    plain = re.sub(r",\s*[a-z][a-z0-9]*(\([a-z]*\))?(?=\s*(\}|$))", "", plain)
                    if any(b.startswith("Functor<") for b in rec.get("bases", [])):
                        members.add("functor")  # partial specializations are not described
                    for ident in re.findall(r"(?<![\w.:>$])([a-z_]\w*)(?=\s*(?:[,.\-\[)}]|$|\.|->))", plain):
                        if ident in ("this", "na", "nd", "s8", "x", "view", "simple", "true", "false", "nullptr", "sizeof"):
                            continue
                        if ident not in members:
                            report.append(f"{rel}: daq::{cname} refers to '{ident}', which is not a member of the class or its bases")
    return sorted(set(report))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", default=os.path.join(TOOL, "output", "opendaq_api.json"))
    ap.add_argument("--out", default=os.path.join(TOOL, "output", "natvis"))
    ap.add_argument("--check-existing", metavar="ROOT")
    ap.add_argument("--xsd", help="natvis.xsd to validate against (Visual Studio ships it)")
    a = ap.parse_args()
    api = json.load(open(a.api, encoding="utf-8"))
    if a.check_existing:
        rep = check_existing(api, a.check_existing)
        print("\n".join(rep))
        print(f"{len(rep)} findings in the hand-written natvis files")
        return 0
    g = Natvis(api)
    files = g.build()
    os.makedirs(a.out, exist_ok=True)
    for lib, text in files.items():
        with open(os.path.join(a.out, f"{lib}.natvis"), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    print(f"{a.out}: {g.stats['types']} classes, {g.stats['patterns']} type patterns, {g.stats['baseCasts']} base casts")
    if a.xsd:
        from lxml import etree
        schema = etree.XMLSchema(etree.parse(a.xsd))
        bad = 0
        for lib in files:
            doc = etree.parse(os.path.join(a.out, f"{lib}.natvis"))
            if not schema.validate(doc):
                bad += 1
                for e in list(schema.error_log)[:10]:
                    print(f"  {lib}.natvis:{e.line}: {e.message}")
        print("natvis.xsd validation:", "passed" if not bad else f"{bad} files failed")
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
