"""Usage evidence: which parts of the API applications and modules use.

Scans application and module source trees (C++, C, Python, AsciiDoc code)
and maps what they reference back onto the API description:

- interfaces: referenced by name, smart pointer name or C binding prefix
- methods: `.name(` / `->name(` calls in C++, snake_case names in Python,
  `daqFoo_name(` in C
- factories: `Name(` calls of C and C++ factories
- implementation members: identifiers used inside the methods of module
  classes that derive from a core implementation class, and overrides of
  its virtual functions

The result is evidence, not a verdict: a name match can be a false
positive (for example a local variable called `context`).
"""

import os
import re
from collections import defaultdict

from .cppparse import parse_file
from .lexer import tokenize

SKIP_DIRS = {".git", "build", "_deps", "external", ".venv", "venv", "node_modules", "__pycache__", "tests", "test",
             "bin", "out", ".vs", "cmake-build-debug", "install"}
EXTS = (".h", ".hpp", ".cpp", ".c", ".py", ".adoc", ".cs", ".md", ".cc")


def snake(name):
    return re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", name).lower()


def py_names(method):
    """Python binding names a method can appear under."""
    s = snake(method)
    out = {s}
    for pre in ("get_", "set_"):
        if s.startswith(pre):
            out.add(s[len(pre):])
    if s.startswith("is_") or s.startswith("has_"):
        out.add(s)
    return out


def iter_files(root, exclude=()):
    if os.path.isfile(root):
        yield root
        return
    for dirpath, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith("build")
                   and os.path.relpath(os.path.join(dirpath, d), root).replace("\\", "/") not in exclude]
        for n in names:
            if n.endswith(EXTS):
                yield os.path.join(dirpath, n)


class Usage:
    def __init__(self, api):
        self.api = api
        self.intf = {i["name"]: i for i in api["interfaces"]}
        self.impl = {i["name"]: i for i in api["implementations"]}
        self.ptr_to_intf = {}
        for i in api["interfaces"]:
            self.ptr_to_intf[i["smartPtr"]] = i["name"]
            alias = (i.get("templated") or {}).get("defaultAliasName")
            if alias:
                self.ptr_to_intf[alias] = i["name"]
            self.ptr_to_intf.setdefault(i["name"][1:] + "Ptr", i["name"])
        self.method_owners = defaultdict(set)
        for i in api["interfaces"]:
            for m in i.get("methods", []):
                self.method_owners[m["name"]].add(i["name"])
        self.py_to_methods = defaultdict(set)
        for name in self.method_owners:
            for p in py_names(name):
                self.py_to_methods[p].add(name)
        self.factory_names = {f["name"] for f in api["factories"]} | {f["name"] for f in api.get("cppFactories", [])}
        self.create_funcs = {f["createFunction"]: f["name"] for f in api["factories"] if f.get("createFunction")}
        # aliases of implementation classes modules derive from (Device = GenericDevice<IDevice>, ...)
        self.impl_alias = {}
        for i in api["implementations"]:
            self.impl_alias[i["name"]] = i["name"]
        self.ev = {
            "interfaces": defaultdict(lambda: defaultdict(set)),  # intf -> kind -> {repo}
            "methods": defaultdict(lambda: defaultdict(set)),  # method name -> kind -> {repo}
            "factories": defaultdict(lambda: defaultdict(set)),
            "members": defaultdict(lambda: defaultdict(list)),  # impl -> member -> [(repo, file, line, how)]
            "derived": defaultdict(list),  # impl -> [(repo, class, file)]
            "examples": defaultdict(list),  # key -> [file:line] (first few)
        }

    def add_aliases(self, aliases):
        for name, target in aliases.items():
            self.impl_alias[name] = target

    def _example(self, key, where):
        lst = self.ev["examples"][key]
        if len(lst) < 3 and where not in lst:
            lst.append(where)

    # ------------------------------------------------------------------
    def scan(self, root, repo, kind, exclude=()):
        """kind: application or module (evidence for proposals), test or docs (shown only)."""
        for f in iter_files(root, exclude):
            rel = os.path.relpath(f, root).replace("\\", "/") if os.path.isdir(root) else os.path.basename(f)
            where = f"{repo}/{rel}"
            try:
                src = open(f, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            if f.endswith(".py"):
                self._scan_py(src, repo, kind, where)
            elif f.endswith(".md"):
                if kind != "docs" and "/docs/" not in f.replace("\\", "/"):
                    continue
                for block in re.findall(r"```[a-zA-Z+#]*\n(.*?)\n```", src, re.S):
                    self._scan_cpp_text(block, repo, kind, where)
                    self._scan_py(block, repo, kind, where)
            elif f.endswith(".adoc"):
                for block in re.findall(r"----\n(.*?)\n----", src, re.S):
                    self._scan_cpp_text(block, repo, kind, where)
                    self._scan_py(block, repo, kind, where)
            elif f.endswith(".cs"):
                self._scan_cpp_text(src, repo, kind, where)
            else:
                self._scan_cpp_text(src, repo, kind, where)
                if f.endswith((".h", ".hpp", ".cpp", ".cc")) and kind in ("module", "application"):
                    self._scan_derived(f, repo, where, kind)

    def _scan_cpp_text(self, src, repo, kind, where):
        toks = [t for t in tokenize(src) if t.kind not in ("comment", "pp", "str")]
        for idx, t in enumerate(toks):
            if t.kind != "id":
                continue
            s = t.text
            prev = toks[idx - 1].text if idx else ""
            nxt = toks[idx + 1].text if idx + 1 < len(toks) else ""
            loc = f"{where}:{t.line}"
            if s in self.intf:
                self.ev["interfaces"][s][kind].add(repo)
                self._example(("i", s, kind), loc)
            elif s in self.ptr_to_intf:
                i = self.ptr_to_intf[s]
                self.ev["interfaces"][i][kind].add(repo)
                self._example(("i", i, kind), loc)
            if prev in (".", "->") and nxt == "(" and s in self.method_owners:
                self.ev["methods"][s][kind].add(repo)
                self._example(("m", s, kind), loc)
            if nxt == "(" and prev not in (".", "->") and s in self.factory_names:
                self.ev["factories"][s][kind].add(repo)
                self._example(("f", s, kind), loc)
            if nxt == "(" and s in self.create_funcs:
                fn = self.create_funcs[s]
                self.ev["factories"][fn][kind].add(repo)
                self._example(("f", fn, kind), loc)
            # C bindings: daqDevice_getName(...)
            m = re.match(r"^daq([A-Z]\w*?)_(\w+)$", s)
            if m and nxt == "(":
                iname = "I" + m.group(1)
                if iname in self.intf:
                    self.ev["interfaces"][iname][kind].add(repo)
                    if m.group(2) in self.method_owners:
                        self.ev["methods"][m.group(2)][kind].add(repo)
                        self._example(("m", m.group(2), kind), loc)
                    elif m.group(2).startswith("create"):
                        fn = m.group(2)[len("create"):]
                        if fn in self.factory_names:
                            self.ev["factories"][fn][kind].add(repo)

    def _scan_py(self, src, repo, kind, where):
        for ln, line in enumerate(src.splitlines(), 1):
            if line.lstrip().startswith("#"):
                continue
            for m in re.finditer(r"\.([a-z_][a-z0-9_]*)\b", line):
                for meth in self.py_to_methods.get(m.group(1), ()):
                    self.ev["methods"][meth][kind].add(repo)
                    self._example(("m", meth, kind), f"{where}:{ln}")
            for m in re.finditer(r"\bopendaq\.([A-Z]\w*)\s*\(", line):
                if m.group(1) in self.factory_names:
                    self.ev["factories"][m.group(1)][kind].add(repo)
                    self._example(("f", m.group(1), kind), f"{where}:{ln}")
            for m in re.finditer(r"\b(I[A-Z]\w+)\b", line):
                if m.group(1) in self.intf:
                    self.ev["interfaces"][m.group(1)][kind].add(repo)
                    self._example(("i", m.group(1), kind), f"{where}:{ln}")
            for m in re.finditer(r"\bopendaq\.([A-Z]\w*)\b", line):
                if "I" + m.group(1) in self.intf:
                    self.ev["interfaces"]["I" + m.group(1)][kind].add(repo)

    # ------------------------------------------------------------------
    def _impl_target(self, base):
        b = re.sub(r"<.*", "", base.split("::")[-1]).strip()
        return self.impl_alias.get(b)

    def _scan_derived(self, path, repo, where, kind="module"):
        """Classes deriving from core implementation classes: overrides and member use."""
        try:
            p, decls = parse_file(path)
        except Exception:
            return
        classes = {}

        def walk(ds):
            for d in ds:
                if d.kind == "type" and d.children is not None and d.info.get("bases"):
                    classes[d.info["name"]] = d
                if d.children and d.kind == "type":
                    walk(d.children)
        walk(decls)
        # out-of-line bodies in this file, keyed by class
        ool = defaultdict(list)
        for d in decls:
            if d.kind == "function" and d.info.get("qualifier") and d.info.get("body"):
                ool[re.sub(r"<.*", "", d.info["qualifier"].split("::")[-1])].append(d)
        targets_by_class = {}
        for cname, d in classes.items():
            for b in d.info["bases"]:
                t = self._impl_target(b)
                if t:
                    targets_by_class[cname] = t
                    self.ev["derived"][t].append((repo, cname, where))
        self._pending_ool = getattr(self, "_pending_ool", defaultdict(list))
        for cls, lst in ool.items():
            self._pending_ool[cls].extend((repo, where, x) for x in lst)
        self._derived_classes = getattr(self, "_derived_classes", {})
        for cname, t in targets_by_class.items():
            self._derived_classes[(repo, cname)] = (t, classes[cname], where, kind)

    def finish(self):
        """Match member use once all files (headers and sources) are scanned."""
        for (repo, cname), (target, d, where, kind) in getattr(self, "_derived_classes", {}).items():
            # implementing (overriding) an interface method of the core class counts as using it
            for ch in d.children or []:
                if ch.kind == "function" and ch.info.get("override") and ch.info["name"] in self.method_owners:
                    self.ev["methods"][ch.info["name"]][kind].add(repo)
                    self._example(("m", ch.info["name"], kind), f"{where}:{ch.line} (override)")
            chain = [target] + self.impl[target].get("baseChain", []) if target in self.impl else [target]
            base_members = {}
            base_virtuals = {}
            for c in chain:
                rec = self.impl.get(c)
                if not rec:
                    continue
                for mem in rec.get("members", []):
                    if mem["access"] != "private":
                        base_members.setdefault(mem["name"], c)
                for fn in rec.get("functions", []):
                    if fn["access"] == "private" or fn.get("implements"):
                        continue
                    if fn["name"] in (c, "~" + c):
                        continue
                    base_members.setdefault(fn["name"], c)
                    if fn.get("virtual"):
                        base_virtuals.setdefault(fn["name"], c)
            own = {ch.info.get("name") for ch in (d.children or []) if ch.kind in ("variable",)}
            bodies = []
            for ch in d.children or []:
                if ch.kind == "function":
                    nm = ch.info["name"]
                    if nm in base_virtuals and (ch.info.get("override") or "virtual" in ch.info.get("specifiers", [])):
                        self.ev["members"][base_virtuals[nm]][nm].append((repo, where, ch.line, "override"))
                    if ch.info.get("body"):
                        bodies.append((where, ch.line, ch.info["body"]))
            for r2, w2, x in getattr(self, "_pending_ool", {}).get(cname, []):
                if r2 != repo:
                    continue
                if x.info["name"] in base_virtuals and x.info.get("override"):
                    pass
                bodies.append((w2, x.line, x.info["body"]))
            for w, line0, body in bodies:
                toks = [t for t in tokenize(body) if t.kind == "id" or t.kind == "punct"]
                for i, t in enumerate(toks):
                    if t.kind != "id" or t.text not in base_members or t.text in own:
                        continue
                    prev = toks[i - 1].text if i else ""
                    prev2 = toks[i - 2].text if i > 1 else ""
                    if prev in (".", "->") and prev2 != "this":
                        continue
                    if prev == "::" and prev2 not in ("Super", "Self") and prev2 not in chain:
                        continue
                    nxt = toks[i + 1].text if i + 1 < len(toks) else ""
                    nxt2 = toks[i + 2].text if i + 2 < len(toks) else ""
                    if nxt == "(":
                        how = "call"
                    elif nxt == "=" and nxt2 != "=":
                        how = "write"
                    else:
                        how = "read"
                    self.ev["members"][base_members[t.text]][t.text].append((repo, w, line0 + t.line - 1, how))

    # ------------------------------------------------------------------
    def to_json(self):
        def kinds(d):
            return {k: sorted(v) for k, v in d.items()}
        out = {
            "interfaces": {k: kinds(v) for k, v in sorted(self.ev["interfaces"].items())},
            "methods": {k: kinds(v) for k, v in sorted(self.ev["methods"].items())},
            "factories": {k: kinds(v) for k, v in sorted(self.ev["factories"].items())},
            "members": {},
            "derived": {k: sorted(set((r, c) for r, c, _ in v)) for k, v in sorted(self.ev["derived"].items())},
            "examples": {"|".join(k): v for k, v in self.ev["examples"].items()},
        }
        for impl, mems in sorted(self.ev["members"].items()):
            out["members"][impl] = {}
            for name, uses in sorted(mems.items()):
                hows = sorted({u[3] for u in uses})
                repos = sorted({u[0] for u in uses})
                out["members"][impl][name] = {"how": hows, "repos": repos,
                                              "examples": [f"{u[1]}:{u[2]}" for u in uses[:3]]}
        return out
