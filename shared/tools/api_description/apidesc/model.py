"""Builds the API description model from parsed declarations."""

import os
import re
import subprocess
import uuid
from collections import defaultdict

from .cppparse import parse_file, describe_type
from .lexer import tokenize
from .tags import tags_from_comments, tags_from_comment, api_tag, rt_attrs, doc_from_comments, is_tag_comment

FORMAT_VERSION = 1

LIBRARIES = {
    "coretypes": {"dirs": ["core/coretypes", "core/corecontainers"], "binary": "daqcoretypes"},
    "coreobjects": {"dirs": ["core/coreobjects"], "binary": "daqcoreobjects"},
    "opendaq": {"dirs": ["core/opendaq"], "binary": "opendaq"},
}

# C ABI scalar types used in interface signatures
SCALARS = {
    "Bool": ("uint8", 1), "Int": ("int64", 8), "UInt": ("uint64", 8), "Float": ("float64", 8),
    "SizeT": ("size", 8), "ErrCode": ("uint32", 4), "ConstCharPtr": ("cstring", 8), "CharPtr": ("cstring", 8),
    "EnumType": ("uint32", 4), "char": ("int8", 1), "bool": ("bool", 1), "int": ("int32", 4),
    "uint8_t": ("uint8", 1), "int8_t": ("int8", 1), "uint16_t": ("uint16", 2), "int16_t": ("int16", 2),
    "uint32_t": ("uint32", 4), "int32_t": ("int32", 4), "uint64_t": ("uint64", 8), "int64_t": ("int64", 8),
    "size_t": ("size", 8), "double": ("float64", 8), "float": ("float32", 4), "void": ("void", 0),
    "Int32": ("int32", 4), "UInt32": ("uint32", 4), "Int16": ("int16", 2), "UInt16": ("uint16", 2),
    "Int8": ("int8", 1), "UInt8": ("uint8", 1), "Int64": ("int64", 8), "UInt64": ("uint64", 8),
    "IntfID": ("guid", 16), "ClockTick": ("int64", 8), "Byte": ("uint8", 1), "daq::Bool": ("uint8", 1),
}

ERR_RE = re.compile(r"\b(OPENDAQ_ERR_[A-Z0-9_]+)\b")
EXC_RE = re.compile(r"\b(?:throw|DAQ_THROW_EXCEPTION)\s*\(?\s*(\w+Exception)\b")


def iid_for(name, ns="daq"):
    """Interface id as computed by DEFINE_INTFID: UUIDv5 over '<name>.<ns>'."""
    return str(uuid.uuid5(uuid.NAMESPACE_DNS, f"{name}.{ns}")).upper()


def library_of(rel):
    rel = rel.replace("\\", "/")
    for lib, info in LIBRARIES.items():
        for d in info["dirs"]:
            if rel.startswith(d + "/"):
                return lib
    return None


def header_include(rel):
    """'core/opendaq/server/include/opendaq/server.h' -> 'opendaq/server.h'"""
    rel = rel.replace("\\", "/")
    if "/include/" in rel:
        return rel.split("/include/", 1)[1]
    return None


def group_of(rel):
    parts = rel.replace("\\", "/").split("/")
    return parts[2] if len(parts) > 3 and parts[1] == "opendaq" else parts[1]


class Model:
    def __init__(self, root, scan_dirs=("core",), exclude=("/tests/", "/test/")):
        self.root = os.path.abspath(root)
        self.scan_dirs = scan_dirs
        self.exclude = exclude
        self.files = {}  # rel -> (parser, decls)
        self.problems = []
        self.interfaces = {}
        self.enums = {}
        self.factories = {}  # by create function
        self.factory_defs = {}  # create function -> impl info
        self.cpp_factories = []
        self.classes = {}  # name -> record (definitions + forward template params)
        self.aliases = {}  # name -> (template params, value)
        self.out_of_line = defaultdict(list)  # (class, method) -> [body]
        self.macros = {}  # name -> body text
        self.exceptions = {}  # FooException -> ERR
        self.errors = {}
        self.scalars = {}
        self.deserialize_registered = {}  # serializeId-ish name -> file
        self.file_rt_attrs = {}
        self.factory_delegates = {}
        self.functions = {}  # exported free functions (extern "C")
        self.callbacks = {}  # C function-pointer typedefs  # create function -> create function it forwards to

    # ------------------------------------------------------------------
    def scan(self):
        for d in self.scan_dirs:
            base = os.path.join(self.root, d)
            for dirpath, _, names in os.walk(base):
                relp = os.path.relpath(dirpath, self.root).replace("\\", "/") + "/"
                if any(x in relp for x in self.exclude):
                    continue
                for n in sorted(names):
                    if not n.endswith((".h", ".cpp", ".hpp")):
                        continue
                    full = os.path.join(dirpath, n)
                    rel = os.path.relpath(full, self.root).replace("\\", "/")
                    p, decls = parse_file(full)
                    self.problems.extend(p.problems)
                    self.files[rel] = (p, decls)
        for rel, (p, decls) in self.files.items():
            self._collect_pp(rel, p)
        for rel, (p, decls) in self.files.items():
            file_comments = [t for t in p.toks if t.kind == "comment" and t.text.startswith("/*#")]
            self.file_rt_attrs[rel] = rt_attrs(tags_from_comments(file_comments))
            for d in decls:
                self._top(rel, p, d)
        self._resolve_classes()
        for _ in range(3):
            for f, g in self.factory_delegates.items():
                if f not in self.factory_defs and g in self.factory_defs:
                    self.factory_defs[f] = dict(self.factory_defs[g], via=g)
        return self

    # ------------------------------------------------------------------
    def _collect_pp(self, rel, p):
        for t in p.toks:
            if t.kind != "pp":
                continue
            m = re.match(r"#\s*define\s+(\w+)(\([^)]*\))?\s*(.*)", t.text, re.S)
            if not m:
                continue
            name, body = m.group(1), m.group(3).replace("\\\n", " ").replace("\\\r\n", " ")
            self.macros.setdefault(name, body)
            if name.startswith("OPENDAQ_ERR_") or name == "OPENDAQ_SUCCESS" or name.startswith("OPENDAQ_ERRTYPE_") \
                    or name.startswith("OPENDAQ_IGNORED") or name.startswith("OPENDAQ_NO_MORE") or name.startswith("OPENDAQ_PARTIAL") \
                    or name.startswith("OPENDAQ_LOWER") or name.startswith("OPENDAQ_FALSE") or name.startswith("OPENDAQ_NOTFOUND"):
                comment = None
                cm = re.search(r"//\s*(.*)$", body)
                if cm:
                    comment = cm.group(1).strip()
                    body = body[:cm.start()]
                self.errors[name] = {"name": name, "expr": " ".join(body.split()), "header": header_include(rel),
                                     "library": library_of(rel), "comment": comment}

    def eval_error(self, name, seen=None):
        seen = seen or set()
        e = self.errors.get(name)
        if not e or name in seen:
            return None
        expr = e["expr"]
        seen.add(name)

        def repl(m):
            n = m.group(0)
            if n in self.errors:
                v = self.eval_error(n, seen)
                return str(v) if v is not None else n
            return n

        expr = re.sub(r"OPENDAQ_ERROR_CODE\s*\(([^,]+),([^)]+)\)", r"(0x80000000 | ((\1) << 16) | (\2))", expr)
        expr = re.sub(r"\b[A-Z_][A-Z0-9_]+\b", repl, expr)
        expr = re.sub(r"(0x[0-9A-Fa-f]+|\d+)[uUlL]+", r"\1", expr)
        try:
            return int(eval(expr, {"__builtins__": {}}))
        except Exception:
            return None

    # ------------------------------------------------------------------
    def _top(self, rel, p, d, outer=None):
        k = d.kind
        if k == "interface":
            self._interface(rel, p, d)
        elif k == "macro":
            self._macro(rel, p, d)
        elif k == "type":
            self._type(rel, p, d, outer)
        elif k == "using":
            name = d.info.get("name")
            val = d.info.get("value")
            if name and val:
                tp = d.info.get("template_params")
                self.aliases.setdefault(name, {"params": tp, "value": val, "file": rel})
                if library_of(rel) == "coretypes" and not tp and re.match(r"^[\w:]+$", val):
                    self.scalars.setdefault(name, val)
        elif k == "typedef" and "/include/" in rel:
            from .cppparse import tokens_text
            text = tokens_text(d.head)
            mm = re.match(r"^typedef\s+(.+?)\s*\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)$", text)
            if mm:
                from .cppparse import parse_params
                params = parse_params([t for t in tokenize(mm.group(3)) if t.kind != "comment"])
                self.callbacks[mm.group(2)] = {"name": mm.group(2), "returns": mm.group(1), "params": params,
                                               "library": library_of(rel), "header": header_include(rel)}
        elif k == "function":
            q = d.info.get("qualifier")
            if not q and d.info.get("body") is None and "/include/" in rel                     and any(t.text in ("PUBLIC_EXPORT", "LIBRARY_FACTORY") for t in d.head):
                tags = tags_from_comments(d.comments)
                aud, mods = api_tag(tags)
                self.functions.setdefault(d.info["name"], {
                    "name": d.info["name"], "returns": d.info["return"], "params": d.info["params"],
                    "library": library_of(rel), "header": header_include(rel), "file": rel, "line": d.line,
                    "audience": aud, "doc": doc_from_comments(d.comments)})
                return
            if q and d.info.get("body") is not None:
                cls = re.sub(r"<.*", "", q.split("::")[-1])
                self.out_of_line[(cls, d.info["name"])].append({"file": rel, "line": d.line, "body": d.info["body"],
                                                                "params": d.info["params"]})
            elif not q and d.info.get("body") is not None and rel.endswith(".h") and "/include/" in rel:
                self._cpp_factory(rel, d)
            elif not q and d.info.get("body") is not None and d.info["name"].startswith("create"):
                # hand-written extern "C" create functions
                body = d.info["body"]
                m = re.search(r"createObject\s*<\s*[\w:]+\s*,\s*([\w:]+(?:<[^<>]*>)?)", body)
                if m:
                    self.factory_defs.setdefault(d.info["name"], {"implementation": m.group(1).strip(), "file": rel,
                                                                  "line": d.line, "how": "createObject"})
                else:
                    calls = re.findall(r"(create\w+)\s*\(", body)
                    if calls:
                        self.factory_delegates[d.info["name"]] = calls[0]

    def _interface(self, rel, p, d, name=None, base=None, macro=None):
        if name is None:
            args = d.info["args"]
            name = args[0]
            base = args[1] if len(args) > 1 else None
            macro = d.info["macro"]
        tags = tags_from_comments(d.comments)
        aud, mods = api_tag(tags)
        fattrs = self.file_rt_attrs.get(rel, {})
        own = rt_attrs(tags)
        smart = None
        for a in fattrs.get("interfaceSmartPtr", []):
            if a and a[0] == name:
                smart = a[1]
        templated = None
        for a in fattrs.get("templated", []):
            templated = {}
            for x in a:
                if ":" in x:
                    kk, vv = x.split(":", 1)
                    templated[kk.strip()] = vv.strip()
                else:
                    templated.setdefault("args", []).append(x)
        if smart is None:
            smart = name[1:] + "Ptr"
        guid = iid_for(name) if "TEMPLATED" not in macro else None
        if macro == "DEFINE_EXTERNAL_INTFID":
            guid = "00000000-0000-0000-C000-000000000046"
        rec = {
            "name": name,
            "guid": guid,
            "base": base,
            "library": library_of(rel),
            "group": group_of(rel),
            "header": header_include(rel),
            "file": rel,
            "line": d.line,
            "audience": aud,
            "labels": mods,
            "coretype": "coretype" in mods,
            "templateParams": ["T"] if macro.endswith("_T") else (["T", "U"] if macro.endswith("_T_U") else None),
            "smartPtr": smart,
            "templated": templated,
            "rtgen": {k: v for k, v in fattrs.items() if k not in ("interfaceSmartPtr", "templated")},
            "doc": doc_from_comments(d.comments),
            "methods": [],
        }
        if not rec["rtgen"]:
            del rec["rtgen"]
        idx = 0
        for m in d.children or []:
            if m.kind != "function":
                continue
            mt = tags_from_comments(m.comments)
            maud, mmods = api_tag(mt)
            attrs = rt_attrs(mt)
            doc = doc_from_comments(m.comments)
            params = [self._param(pp, attrs, doc) for pp in m.info["params"]]
            meth = {
                "name": m.info["name"],
                "index": idx,
                "audience": maud,
                "labels": mmods,
                "returns": m.info["return"],
                "const": m.info["const"],
                "params": params,
                "doc": doc,
                "line": m.line,
            }
            other = {kk: vv for kk, vv in attrs.items() if kk not in ("elementType", "templateType", "arrayArg", "allowNull")}
            if other:
                meth["rtgen"] = other
            if not meth["labels"]:
                del meth["labels"]
            if not doc:
                del meth["doc"]
            idx += 1
            rec["methods"].append(meth)
        if not rec["labels"]:
            del rec["labels"]
        self.interfaces[name] = rec

    def _param(self, pp, attrs, doc):
        out = dict(pp)
        name = pp.get("name")
        for a in attrs.get("elementType", []):
            if a and a[0] == name:
                out["elementType"] = a[1] if len(a) == 2 else None
                if len(a) == 3:
                    out["keyType"], out["valueType"] = a[1], a[2]
                    out.pop("elementType")
        for a in attrs.get("templateType", []):
            if a and a[0] == name:
                out["templateType"] = a[1:] if len(a) > 2 else a[1]
                # on containers RTGen's templateType names the element (or key and value) types
                if pp["base"] == "IList" and len(a) == 2 and "elementType" not in out:
                    out["elementType"] = a[1]
                if pp["base"] == "IDict" and len(a) == 3 and "keyType" not in out:
                    out["keyType"], out["valueType"] = a[1], a[2]
        for a in attrs.get("arrayArg", []):
            if a and a[0] == name:
                out["arraySize"] = a[1] if len(a) > 1 else None
        for a in attrs.get("allowNull", []):
            if a and a[0] == name:
                out["allowNull"] = True
        ddir = (doc or {}).get("params", {}).get(name, {}).get("dir")
        out["direction"] = self._direction(pp, ddir)
        if ddir and ddir.replace("in,out", "inout") != out["direction"]:
            out["docDirection"] = ddir
        if (doc or {}).get("params", {}).get(name, {}).get("text"):
            out["doc"] = doc["params"][name]["text"]
        return out

    @staticmethod
    def _direction(pp, ddir):
        base, ptr = pp["base"], pp["pointer"]
        if ptr == 1 and (re.match(r"^I[A-Z]\w*$", base) or re.match(r"^I[A-Z]\w*<", base)):
            # a single-pointer interface argument can only be an input; the object itself may be modified
            return "inout" if ddir in ("in,out", "inout") else "in"
        if ddir in ("out",):
            return "out"
        if ddir in ("in,out", "inout"):
            return "inout"
        if ddir == "in":
            return "in"
        base, ptr = pp["base"], pp["pointer"]
        if ptr >= 2:
            return "out"
        if ptr == 1:
            if base in ("void",):
                return "in"
            if base in ("char", "ConstCharPtr") and pp.get("const"):
                return "in"
            if re.match(r"^I[A-Z]\w*$", base) or re.match(r"^I[A-Z]\w*<", base):
                return "in"
            if pp.get("const") is True:
                return "in"
            return "out"
        return "in"

    # ------------------------------------------------------------------
    FACTORY_DECL = {
        "OPENDAQ_DECLARE_CLASS_FACTORY": "plain",
        "OPENDAQ_DECLARE_CLASS_FACTORY_WITH_INTERFACE": "intf",
        "OPENDAQ_DECLARE_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC": "intf_func",
        "OPENDAQ_DECLARE_CLASS_FACTORY_FUNC_WITH_INTERFACE": "func_only",
    }
    FACTORY_DEF = {
        "OPENDAQ_DEFINE_CLASS_FACTORY": "plain",
        "OPENDAQ_DEFINE_CLASS_FACTORY_WITH_INTERFACE": "intf",
        "OPENDAQ_DEFINE_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC": "intf_func",
        "OPENDAQ_DEFINE_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC_OBJ": "obj",
        "OPENDAQ_DEFINE_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC_OBJ_TEMPLATE": "obj",
        "OPENDAQ_DEFINE_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC_OBJ_PAR": "obj",
        "OPENDAQ_DEFINE_CLASS_SINGLETON_FACTORY": "singleton",
    }

    @staticmethod
    def _rejoin(args):
        out = []
        for a in args:
            if out and out[-1].count("<") > out[-1].count(">"):
                out[-1] += ", " + a
            else:
                out.append(a)
        return out

    def _factory_params(self, flat):
        params = []
        for i in range(0, len(flat) - 1, 2):
            typ, nm = flat[i], flat[i + 1]
            toks = [t for t in tokenize(typ) if t.kind != "comment"]
            params.append(describe_type(toks, nm))
        return params

    def _macro(self, rel, p, d):
        name = d.info["name"]
        args = self._rejoin(d.info["args"])
        if name in self.FACTORY_DECL:
            kind = self.FACTORY_DECL[name]
            exp = args[0]
            obj = args[1]
            if kind == "plain":
                intf, func, rest = "I" + obj, "create" + obj, args[2:]
            elif kind == "intf":
                intf, func, rest = args[2], "create" + obj, args[3:]
            elif kind == "intf_func":
                intf, func, rest = args[2], args[3], args[4:]
            else:
                intf, func, rest = args[2], "create" + obj, args[3:]
            tags = tags_from_comments(d.comments)
            aud, mods = api_tag(tags)
            self.factories[func] = {
                "name": obj,
                "createFunction": func,
                "cppHelper": None if kind == "func_only" else obj + "_Create",
                "returns": intf,
                "library": library_of(rel),
                "group": group_of(rel),
                "header": header_include(rel),
                "file": rel,
                "line": d.line,
                "exported": exp == "LIBRARY_FACTORY",
                "audience": aud,
                "labels": mods,
                "params": self._factory_params(rest),
                "doc": doc_from_comments(d.comments),
            }
            if not mods:
                del self.factories[func]["labels"]
        elif name.startswith("OPENDAQ_DECLARE_CLASS_FACTORY_WITH_INTERFACE_TEMPLATE"):
            tags = tags_from_comments(d.comments)
            aud, mods = api_tag(tags)
            info = args[0].strip("()").split(",")
            self.factories["template:" + ",".join(x.strip() for x in info)] = {
                "name": info[1].strip() if len(info) > 1 else args[0],
                "createFunction": None, "templated": True, "raw": args, "returns": info[2].strip() if len(info) > 2 else None,
                "library": library_of(rel), "group": group_of(rel), "header": header_include(rel), "file": rel,
                "line": d.line, "exported": "LIBRARY_FACTORY" in args[0], "audience": aud, "params": [],
                "doc": doc_from_comments(d.comments),
            }
        elif name in self.FACTORY_DEF:
            kind = self.FACTORY_DEF[name]
            obj = args[1]
            if kind == "plain":
                func, impl = "create" + obj, obj + "Impl"
            elif kind == "intf":
                func, impl = "create" + obj, obj + "Impl"
            elif kind == "intf_func":
                func, impl = args[3], obj + "Impl"
            elif kind == "singleton":
                func, impl = "create" + obj, obj + "Impl"
            else:
                func, impl = args[3], obj
            self.factory_defs[func] = {"implementation": impl, "file": rel, "line": d.line, "how": name}
        elif name.startswith("OPENDAQ_DEFINE_CUSTOM_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC_OBJ"):
            self.factory_defs[args[2]] = {"implementation": f"ObjectCreator<{args[1].strip('()')}>", "file": rel,
                                          "line": d.line, "how": name}
        elif name == "OPENDAQ_REGISTER_DESERIALIZE_FACTORY":
            self.deserialize_registered[args[0]] = rel
        elif name in ("DEFINE_EXCEPTION", "DEFINE_EXCEPTION_BASE"):
            a = args[1:] if name == "DEFINE_EXCEPTION_BASE" else args
            if len(a) >= 2:
                self.exceptions[a[0] + "Exception"] = a[1]

    def _cpp_factory(self, rel, d):
        ret = d.info["return"]
        body = d.info["body"]
        calls = sorted(set(re.findall(r"\b(\w+)_Create\b", body)) | set(re.findall(r"\b(create[A-Z]\w*)\s*\(", body)))
        impl = re.findall(r"createWithImplementation\s*<\s*(\w+)\s*,\s*([\w:<>]+)", body)
        if not (calls or impl or ret.endswith("Ptr") or "Ptr<" in ret):
            return
        tags = tags_from_comments(d.comments)
        aud, mods = api_tag(tags)
        self.cpp_factories.append({
            "name": d.info["name"],
            "returns": ret,
            "header": header_include(rel),
            "file": rel,
            "line": d.line,
            "library": library_of(rel),
            "template": d.info.get("template"),
            "params": d.info["params"],
            "calls": calls,
            "implementation": impl[0][1] if impl else None,
            "audience": aud,
            "doc": doc_from_comments(d.comments),
        })

    # ------------------------------------------------------------------
    def _type(self, rel, p, d, outer):
        kw = d.info.get("keyword")
        name = d.info.get("name")
        if not name:
            return
        if kw and kw.startswith("enum"):
            if d.info.get("enumerators") is None:
                return
            tags = tags_from_comments(d.comments)
            aud, mods = api_tag(tags)
            self.enums.setdefault(name, {
                "name": name, "scoped": kw == "enum class", "underlying": d.info.get("underlying"),
                "library": library_of(rel), "header": header_include(rel), "file": rel, "line": d.line,
                "audience": aud, "doc": doc_from_comments(d.comments),
                "outer": outer,
                "members": d.info["enumerators"],
            })
            return
        if d.children and "/include/" in rel and name.startswith("I") and kw == "struct":
            idm = [c for c in d.children if c.kind == "macro" and c.info["name"] in ("DEFINE_INTFID", "DEFINE_EXTERNAL_INTFID")]
            if idm:
                bases = d.info.get("bases") or []
                self._interface(rel, p, d, name, bases[0] if bases else None, idm[0].info["name"])
                return
        rec = self.classes.setdefault(name, {"name": name, "defs": [], "fwd_params": None})
        if d.info.get("forward"):
            tp = d.info.get("template_params")
            if tp and any(x.get("default") for x in tp):
                rec["fwd_params"] = tp
            return
        if d.info.get("specialization"):
            return
        rec["defs"].append((rel, p, d))
        for c in d.children or []:
            if c.kind == "type" and c.info.get("keyword", "").startswith("enum"):
                self._type(rel, p, c, outer=name)

    # ------------------------------------------------------------------
    def _resolve_classes(self):
        """Pick one definition per class and compute bases with template params."""
        self.impl = {}
        for name, rec in self.classes.items():
            if not rec["defs"]:
                continue
            # prefer the installed header definition
            rec["defs"].sort(key=lambda x: (0 if "/include/" in x[0] else 1, x[0]))
            rel, p, d = rec["defs"][0]
            tp = d.info.get("template_params") or None
            if rec["fwd_params"] and tp and len(rec["fwd_params"]) == len(tp):
                tp = [dict(a, default=b.get("default")) for a, b in zip(tp, rec["fwd_params"])]
            rec["params"] = tp
            rec["bases"] = d.info.get("bases", [])
            rec["decl"] = (rel, p, d)

    # ------------------------------------------------------------------
    def class_interfaces(self, name, args=None, depth=0, seen=None):
        """Interfaces implemented by class `name` instantiated with `args` (list of names)."""
        seen = seen or set()
        key = (name, tuple(args or ()))
        if depth > 30 or key in seen:
            return []
        seen.add(key)
        params, bases = None, None
        if name in self.classes and self.classes[name].get("decl"):
            params = self.classes[name].get("params")
            bases = self.classes[name].get("bases", [])
        elif name in self.aliases:
            params = self.aliases[name]["params"]
            bases = [self.aliases[name]["value"]]
        else:
            return []
        bind = {}
        args = list(args or [])
        for i, prm in enumerate(params or []):
            if prm["variadic"]:
                bind[prm["name"]] = args[i:]
                break
            if i < len(args):
                bind[prm["name"]] = [args[i]]
            elif prm.get("default"):
                bind[prm["name"]] = [prm["default"]]
            else:
                bind[prm["name"]] = []
        out = []
        for b in bases:
            bname, bargs = split_template(b)
            sub = []
            for a in bargs:
                a2 = a.replace("...", "").strip()
                if a2 in bind:
                    sub.extend(bind[a2])
                else:
                    sub.append(a)
            for a in sub:
                an = split_template(a)[0]
                if an in self.interfaces and an not in out:
                    out.append(an)
            if bname in self.interfaces and bname not in out:
                out.append(bname)
            for x in self.class_interfaces(bname, sub, depth + 1, seen):
                if x not in out:
                    out.append(x)
        return out

    def class_chain(self, name, depth=0):
        """Base implementation classes, nearest first."""
        out = []
        if depth > 30:
            return out
        if name in self.classes and self.classes[name].get("decl"):
            bases = self.classes[name]["bases"]
        elif name in self.aliases:
            bases = [self.aliases[name]["value"]]
        else:
            return out
        for b in bases:
            bn = split_template(b)[0]
            if bn in self.interfaces or not (bn in self.classes or bn in self.aliases):
                continue
            out.append(bn)
            out.extend(x for x in self.class_chain(bn, depth + 1) if x not in out)
        return out


def split_template(s):
    s = s.strip()
    if "<" not in s:
        return s.split("::")[-1], []
    name = s[:s.index("<")].strip().split("::")[-1]
    inner = s[s.index("<") + 1: s.rindex(">")]
    args, cur, depth = [], "", 0
    for ch in inner:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    return name, args


def git_rev(root):
    try:
        return subprocess.check_output(["git", "-C", root, "rev-parse", "--short=9", "HEAD"], text=True).strip()
    except Exception:
        return None
