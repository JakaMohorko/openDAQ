"""Turns the Model into the API description JSON (format version 1)."""

import os
import re
from collections import defaultdict

from .model import Model, FORMAT_VERSION, LIBRARIES, SCALARS, ERR_RE, EXC_RE, split_template, git_rev, iid_for
from .tags import tags_from_comments, api_tag, doc_from_comments

# statement-level macros whose error codes are what the statement returns
RETURNING = re.compile(r"\breturn\b|DAQ_MAKE_ERROR_INFO|DAQ_EXTEND_ERROR_INFO|makeErrorInfo|DAQ_THROW_EXCEPTION|\bthrow\b")
NOT_RETURNED = re.compile(r"(==|!=)\s*OPENDAQ_ERR_\w+|OPENDAQ_ERR_\w+\s*(==|!=)|OPENDAQ_FAILED|OPENDAQ_SUCCEEDED|_EXCEPT\s*\(")


class Emitter:
    def __init__(self, model: Model):
        self.m = model
        self.smart_pointers = []
        self._macro_errs = {}

    # -- errors --------------------------------------------------------
    def macro_errors(self, name, depth=0):
        if name in self._macro_errs:
            return self._macro_errs[name]
        self._macro_errs[name] = set()
        body = self.m.macros.get(name, "")
        errs = set()
        if depth < 5 and (name.startswith("OPENDAQ_PARAM_") or "return" in body):
            errs |= self.body_errors(body, depth + 1, follow_helpers=False)[0]
        self._macro_errs[name] = errs
        return errs

    def body_errors(self, body, depth=0, follow_helpers=True):
        errs, excs = set(), set()
        # one statement at a time
        for stmt in re.split(r"[;{}]", body):
            if not stmt.strip():
                continue
            if RETURNING.search(stmt):
                clean = NOT_RETURNED.sub(" ", stmt)
                errs |= set(ERR_RE.findall(clean))
            for ex in EXC_RE.findall(stmt):
                code = self.m.exceptions.get(ex)
                if code:
                    errs.add(code)
                excs.add(ex)
            for mac in re.findall(r"\b(OPENDAQ_PARAM_\w+|[A-Z][A-Z0-9_]{4,})\s*\(", stmt):
                if mac in self.m.macros and not mac.startswith(("OPENDAQ_ERR", "DAQ_MAKE_ERROR", "DAQ_EXTEND")):
                    errs |= self.macro_errors(mac, depth)
        errs.discard("OPENDAQ_SUCCESS")
        return errs, excs

    # -- implementations ------------------------------------------------
    def impl_records(self):
        m = self.m
        out = {}
        # classes implementations inherit from are described even when they declare no interface themselves
        bases_used = set()
        for name, rec in m.classes.items():
            if rec.get("decl") and m.class_interfaces(name):
                bases_used.update(m.class_chain(name))
        for name, rec in sorted(m.classes.items()):
            if not rec.get("decl"):
                continue
            rel, p, d = rec["decl"]
            explicit = m.class_interfaces(name)
            # a template whose interface parameter has no default: take the arguments of an alias that instantiates it
            # (PropertyObjectImpl = GenericPropertyObjectImpl<IPropertyObject>)
            if rec.get("params") and any(not tp.get("default") and not tp.get("variadic") for tp in rec["params"]):
                for a in m.aliases.values():
                    an, aargs = split_template(a["value"])
                    if an == name and aargs and not a.get("params"):
                        for x in m.class_interfaces(name, aargs):
                            if x not in explicit:
                                explicit.insert(0, x)
            chain = m.class_chain(name)
            if not explicit and name not in bases_used:
                continue
            if name == "ObjectPtr" or "ObjectPtr" in chain:
                tags = tags_from_comments(d.comments)
                aud, mods = api_tag(tags)
                self.smart_pointers.append({"name": name, "interface": explicit[0] if explicit else None,
                                            "file": rel, "line": d.line, "audience": aud,
                                            "templateParams": [tp["name"] for tp in rec["params"]] if rec.get("params") else None,
                                            "bases": rec["bases"], "header": rel.split("/include/", 1)[1] if "/include/" in rel else None})
                continue
            intfs = []
            for i in explicit:
                x = i
                while x and x in m.interfaces and x not in intfs:
                    intfs.append(x)
                    x = m.interfaces[x]["base"]
            if "/include/" not in rel and "/src/" not in rel:
                continue
            tags = tags_from_comments(d.comments)
            aud, mods = api_tag(tags)
            members, functions = [], []
            serialize_id = None
            for c in d.children or []:
                ct = tags_from_comments(c.comments + c.trailing)
                caud, cmods = api_tag(ct)
                if c.kind == "variable":
                    members.append(dict(name=c.info["name"], type=c.info["type"], access=c.access or "private",
                                        static="static" in c.info.get("specifiers", []),
                                        audience=caud, labels=cmods or None, line=c.line,
                                        init=c.info.get("init")))
                elif c.kind == "function":
                    fn = c.info["name"]
                    if fn == "SerializeId" and c.info.get("body"):
                        mm = re.search(r'return\s+"([^"]+)"', c.info["body"])
                        if mm:
                            serialize_id = mm.group(1)
                    functions.append(dict(
                        name=fn, access=c.access or "private",
                        returns=c.info["return"] or None,
                        params=[{k: v for k, v in pp.items() if k in ("name", "cppType", "default")} for pp in c.info["params"]],
                        virtual="virtual" in c.info["specifiers"] or c.info["override"],
                        override=c.info["override"], pure=c.info["pure"], static="static" in c.info["specifiers"],
                        const=c.info["const"], hasBody=c.info.get("body") is not None,
                        audience=caud, labels=cmods or None, line=c.line,
                        doc=doc_from_comments(c.comments)))
            if serialize_id is None:
                for x in m.out_of_line.get((name, "SerializeId"), []):
                    mm = re.search(r'return\s+"([^"]+)"', x["body"])
                    if mm:
                        serialize_id = mm.group(1)
            for f in functions:
                f["implements"] = None
                if f["override"] or f["virtual"]:
                    for i in intfs:
                        if any(mm["name"] == f["name"] for mm in m.interfaces[i]["methods"]):
                            f["implements"] = i
                            break
            out[name] = {
                "name": name,
                "templateParams": [{k: v for k, v in tp.items() if v not in (None, False)} for tp in rec["params"]] if rec.get("params") else None,
                "header": rel.split("/include/", 1)[1] if "/include/" in rel else None,
                "file": rel,
                "line": d.line,
                "installed": "/include/" in rel,
                "library": next((lib for lib, info in LIBRARIES.items() if any(rel.startswith(x + "/") for x in info["dirs"])), None),
                "bases": rec["bases"],
                "baseChain": chain,
                "interfaces": intfs,
                "audience": aud,
                "labels": mods or None,
                "serializeId": serialize_id,
                "members": members,
                "functions": functions,
            "callbacks": [m.callbacks[k] for k in sorted(m.callbacks)],
                "doc": doc_from_comments(d.comments),
            }
        return out

    # -- method bodies for error mining ---------------------------------
    def method_bodies(self, impls):
        """(class, method) -> list of bodies (in-class and out-of-line)."""
        bodies = defaultdict(list)
        m = self.m
        for name, rec in m.classes.items():
            if not rec.get("decl"):
                continue
            d = rec["decl"][2]
            for c in d.children or []:
                if c.kind == "function" and c.info.get("body"):
                    bodies[(name, c.info["name"])].append(c.info["body"])
        for (cls, meth), lst in m.out_of_line.items():
            for x in lst:
                bodies[(cls, meth)].append(x["body"])
        return bodies

    def method_errors(self, impls):
        bodies = self.method_bodies(impls)
        result = defaultdict(lambda: defaultdict(set))  # (intf, method) -> err -> {impl}
        for iname, irec in self.m.interfaces.items():
            pass
        for cname, crec in impls.items():
            chain = [cname] + crec["baseChain"]
            for iname in crec["interfaces"]:
                for meth in self.m.interfaces[iname]["methods"]:
                    # the most-derived class that defines the method answers the call
                    for k in chain:
                        if (k, meth["name"]) in bodies:
                            for b in bodies[(k, meth["name"])]:
                                errs, _ = self.body_errors(b)
                                # follow one level of same-class helpers named in the body
                                for helper in set(re.findall(r"\b(\w+(?:Internal|Impl))\s*\(", b)):
                                    for k2 in chain:
                                        for hb in bodies.get((k2, helper), []):
                                            errs |= self.body_errors(hb)[0]
                                for e in errs:
                                    result[(iname, meth["name"])][e].add(k)
                            break
        return result

    def type_kind(self, base, enum_names):
        m = self.m
        if base in m.callbacks:
            return "callback"
        if base in m.interfaces or re.match(r"^I[A-Z]\w*<", base):
            return "interface"
        if base in enum_names:
            return "enum"
        if base in SCALARS or m.scalars.get(base) in SCALARS:
            return "scalar"
        if base in m.aliases or base in m.classes:
            rec = self.struct_record(base)
            if rec and rec.get("fields"):
                return "struct"
        return "cpp"

    def struct_record(self, name):
        """Plain struct passed by value through an interface: fields in declaration order."""
        m = self.m
        cls, args = name, []
        if name in m.aliases:
            cls, args = split_template(m.aliases[name]["value"])
        rec = m.classes.get(cls)
        if not rec or not rec.get("decl"):
            return None
        rel, p, d = rec["decl"]
        bind = {}
        for i, prm in enumerate(rec.get("params") or []):
            if i < len(args):
                bind[prm["name"]] = args[i]
        fields = []
        for c in d.children or []:
            if c.kind == "variable" and "static" not in c.info.get("specifiers", []):
                t = c.info["type"]
                t = bind.get(t, t)
                fields.append({"name": c.info["name"], "type": t})
            if c.kind == "type" and c.info.get("keyword") == "union" and c.children:
                for u in c.children:
                    if u.kind == "variable":
                        fields.append({"name": u.info["name"], "type": u.info["type"], "union": True,
                                       "array": u.info.get("array") or None})
                        break
        before = p.src[:d.src_span[0]]
        packs = re.findall(r"#\s*pragma\s+pack\s*\(\s*(push\s*,\s*(\d+)|pop)\s*\)", before)
        pack = None
        for full, n in packs:
            pack = int(n) if n else None
        out = {"name": name, "cppType": f"{cls}<{', '.join(args)}>" if args else cls,
               "header": rel.split("/include/", 1)[1] if "/include/" in rel else None, "fields": fields}
        if pack:
            out["pack"] = pack
        return out

    # ------------------------------------------------------------------
    def build(self, version=None):
        m = self.m
        impls = self.impl_records()
        errs_by_method = self.method_errors(impls)
        # factories: link definitions
        structs_extra = {}
        factories = []
        for func, f in sorted(m.factories.items(), key=lambda x: (x[1]["library"] or "", x[1]["name"])):
            f = dict(f)
            dfn = m.factory_defs.get(f["createFunction"]) if f.get("createFunction") else None
            if dfn:
                impl = dfn["implementation"]
                f["implementation"] = impl
                f["implementationClass"] = split_template(impl)[0]
                f["definedIn"] = dfn["file"]
            factories.append(f)
        # hand-declared exported create functions are factories too
        for fn in m.functions.values():
            ps = fn["params"]
            if fn["name"].startswith("create") and ps and ps[0]["pointer"] == 2 and ps[0]["base"] in m.interfaces                     and not any(f.get("createFunction") == fn["name"] for f in factories):
                f = {"name": fn["name"][len("create"):], "createFunction": fn["name"], "cppHelper": None,
                     "returns": ps[0]["base"], "library": fn["library"], "group": None, "header": fn["header"],
                     "file": fn["file"], "line": fn["line"], "exported": True, "audience": fn["audience"],
                     "params": ps[1:], "doc": fn["doc"], "declaredBy": "function"}
                dfn = m.factory_defs.get(fn["name"])
                if dfn:
                    f["implementation"] = dfn["implementation"]
                    f["implementationClass"] = split_template(dfn["implementation"])[0]
                    f["definedIn"] = dfn["file"]
                factories.append(f)
        factory_funcs = {f.get("createFunction") for f in factories}
        functions = []
        for name in sorted(m.functions):
            fn = dict(m.functions[name])
            if name in factory_funcs:
                fn["factory"] = True
            for prm in fn["params"]:
                prm["base"] = re.sub(r"^daq::", "", prm["base"])
            functions.append(fn)
        for f in factories:
            for prm in f.get("params", []):
                prm["base"] = re.sub(r"^daq::", "", prm["base"])
                prm["kind"] = self.type_kind(prm["base"], {e["name"] for e in m.enums.values() if e.get("header")})
                if prm["kind"] == "struct":
                    structs_extra.setdefault(prm["base"], self.struct_record(prm["base"]))
            if f.get("params") is not None:
                f["abi"] = "c" if all(p["kind"] in ("interface", "enum", "scalar", "struct", "callback") for p in f["params"]) else "cpp"
        for fn in functions:
            for prm in fn["params"]:
                prm["kind"] = self.type_kind(prm["base"], {e["name"] for e in m.enums.values() if e.get("header")})
        by_impl = defaultdict(list)
        for f in factories:
            if f.get("implementationClass"):
                by_impl[f["implementationClass"]].append(f["name"])
        for name, rec in impls.items():
            rec["factories"] = sorted(by_impl.get(name, []))
        # interfaces
        implemented_by = defaultdict(list)
        for name, rec in impls.items():
            for i in rec["interfaces"]:
                implemented_by[i].append(name)
        created_by = defaultdict(list)
        for f in factories:
            if f.get("returns"):
                created_by[f["returns"]].append(f["name"])
        enum_names = {e["name"] for e in m.enums.values() if e.get("header")}
        structs = {}
        for name in sorted(m.interfaces):
            for meth in m.interfaces[name]["methods"]:
                for prm in meth["params"]:
                    prm["base"] = re.sub(r"^daq::", "", prm["base"])
                    kind = self.type_kind(prm["base"], enum_names)
                    if kind == "struct":
                        structs.setdefault(prm["base"], self.struct_record(prm["base"]))
                    prm["kind"] = kind
                ret = re.sub(r"^daq::", "", meth["returns"])
                abi_ok = all(prm["kind"] in ("interface", "enum", "scalar", "struct") for prm in meth["params"])                     and (ret == "ErrCode" or (name == "IUnknown" and ret == "int"))
                meth["abi"] = "c" if abi_ok else "cpp"
            m.interfaces[name]["abi"] = "c" if all(x["abi"] == "c" for x in m.interfaces[name]["methods"]) else "cpp"
        interfaces = []
        for name in sorted(m.interfaces):
            rec = dict(m.interfaces[name])
            rec["implementedBy"] = sorted(implemented_by.get(name, []))
            rec["createdBy"] = sorted(created_by.get(name, []))
            for meth in rec["methods"]:
                found = errs_by_method.get((name, meth["name"]), {})
                documented = [r["code"] for r in (meth.get("doc") or {}).get("retvals", []) if r["code"] != "OPENDAQ_SUCCESS"]
                errors = []
                for code in sorted(set(found) | set(documented)):
                    e = {"code": code}
                    if code in documented:
                        e["documented"] = True
                    if code in found:
                        e["raisedBy"] = sorted(found[code])
                    errors.append(e)
                if errors:
                    meth["errors"] = errors
                meth["effectiveAudience"] = meth["audience"] or rec["audience"]
            interfaces.append(rec)
        # errors table
        exc_by_code = defaultdict(list)
        for ex, code in m.exceptions.items():
            exc_by_code[code].append(ex)
        errors = []
        for name, e in sorted(m.errors.items()):
            if not name.startswith("OPENDAQ_ERR_") and name != "OPENDAQ_SUCCESS" and not name.startswith(
                    ("OPENDAQ_IGNORED", "OPENDAQ_NO_MORE", "OPENDAQ_PARTIAL", "OPENDAQ_LOWER", "OPENDAQ_FALSE", "OPENDAQ_NOTFOUND")):
                continue
            v = m.eval_error(name)
            errors.append({"name": name, "value": v, "hex": f"0x{v:08X}" if v is not None else None,
                           "library": e["library"], "header": e["header"], "exceptions": sorted(exc_by_code.get(name, [])),
                           "comment": e["comment"]})
        # enums
        enums = []
        for name, e in sorted(m.enums.items()):
            if not e["library"] or not e["header"]:
                continue
            e = dict(e)
            val = -1
            members = []
            known = {}
            for mem in e["members"]:
                v = mem["value"]
                if v is None:
                    val += 1
                else:
                    try:
                        expr = re.sub(r"(0x[0-9A-Fa-f]+|\d+)[uUlL]+", r"\1", v)
                        expr = re.sub(r"\b[A-Za-z_]\w*\b", lambda mm: str(known.get(mm.group(0), mm.group(0))), expr)
                        val = int(eval(expr, {"__builtins__": {}}))
                    except Exception:
                        val = None
                known[mem["name"]] = val
                members.append({"name": mem["name"], "value": val, "expr": v} if v else {"name": mem["name"], "value": val})
                if val is None:
                    val = -1
            e["members"] = members
            enums.append(e)
        scalars = {k: {"abi": SCALARS[k][0], "size": SCALARS[k][1]} for k in SCALARS}
        for k, v in m.scalars.items():
            if k not in scalars and v in SCALARS:
                scalars[k] = {"abi": SCALARS[v][0], "size": SCALARS[v][1], "alias": v}
        # serialization registry
        bodies = self.method_bodies(impls)
        WRITE_FN = ("serializeCustomObjectValues", "serialize", "serializeForUpdate", "serializeCustomValues",
                    "serializePropertyValues", "serializeProperties", "serializeMembers")
        READ_FN = ("Deserialize", "DeserializeComponent", "updateObject", "updateInternal", "deserializeCustomObjectValues",
                   "deserializeComponent", "DeserializePropertyObject", "deserializeMembers", "deserializeProperties")

        def keys(cls, fns, pattern):
            found = []
            for c in [cls] + impls[cls].get("baseChain", []):
                for fn in fns:
                    for b in bodies.get((c, fn), []):
                        for k in re.findall(pattern, b):
                            if k not in found:
                                found.append(k)
            return found
        ser = []
        for name, rec in sorted(impls.items()):
            if rec["serializeId"]:
                ser.append({"serializeId": rec["serializeId"], "writtenBy": name,
                            "readBy": f"{name}::Deserialize" if any(fn["name"] == "Deserialize" for fn in rec["functions"]) else None,
                            "registered": rec["serializeId"] in m.deserialize_registered or name in m.deserialize_registered,
                            "keysWritten": keys(name, WRITE_FN, r'\.key\(\s*"([^"]+)"'),
                            "keysRead": keys(name, READ_FN, r'(?:read\w*|hasKey|getKeys?)\(\s*"([^"]+)"')})
        # aliases that name implementation classes, interfaces or smart pointers (Device = DeviceBase<IDevice>)
        known = set(impls) | set(m.interfaces)
        type_aliases = []
        for name, a in sorted(m.aliases.items()):
            target = split_template(a["value"])[0]
            rel = a["file"]
            if target in known or (target in m.aliases and split_template(m.aliases[target]["value"])[0] in known):
                if "/include/" not in rel and "/src/" not in rel:
                    continue
                type_aliases.append({"name": name, "value": a["value"],
                                     "templateParams": [{k: v for k, v in tp.items() if v not in (None, False)} for tp in a["params"]] if a["params"] else None,
                                     "header": rel.split("/include/", 1)[1] if "/include/" in rel else None, "file": rel})
        # hand-written serialization schemas and load rules (spec 0012: schema next to the code, checked by a test)
        schema_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "schemas")
        setup = None
        sp = os.path.join(schema_dir, "setup.schema.json")
        if os.path.exists(sp):
            import json as _json
            schema = _json.load(open(sp, encoding="utf-8"))
            rules = _json.load(open(os.path.join(schema_dir, "setup_rules.json"), encoding="utf-8"))
            setup = {"schema": schema, "rules": rules["rules"], "checkedAgainst": rules["checkedAgainst"]}
            for e in ser:
                if e["serializeId"] in schema["$defs"]:
                    e["schema"] = {"$ref": f"#/setup/schema/$defs/{e['serializeId']}"}
        major = (version or "3").split(".")[0]
        libs = {k: {"binary": v["binary"], "windows": f"{v['binary']}-64-{major}.dll",
                    "linux": f"lib{v['binary']}-64-{major}.so", "macos": f"lib{v['binary']}-64-{major}.dylib",
                    "sourceDirs": v["dirs"]} for k, v in LIBRARIES.items()}
        doc = {
            "formatVersion": FORMAT_VERSION,
            "openDaqVersion": version,
            "source": {"commit": git_rev(m.root)},
            "audiences": {
                "application": "Applications, modules and the core. Stable across minor releases; changes follow the deprecation rules.",
                "module": "Modules and the core, never applications. Stable for modules across minor releases.",
                "core": "The core only. Can change in any release without notice.",
            },
            "libraries": libs,
            "scalars": scalars,
            "structs": [v for k, v in sorted({**structs_extra, **structs}.items()) if v],
            "errors": errors,
            "enums": enums,
            "interfaces": interfaces,
            "factories": factories,
            "functions": functions,
            "callbacks": [m.callbacks[k] for k in sorted(m.callbacks)],
            "cppFactories": sorted(m.cpp_factories, key=lambda x: (x["library"] or "", x["name"])),
            "implementations": [impls[k] for k in sorted(impls)],
            "typeAliases": type_aliases,
            "smartPointers": sorted(self.smart_pointers, key=lambda x: x["name"]),
            "serialization": ser,
            "setup": setup,
        }
        return strip_empty(doc)


def strip_empty(o):
    if isinstance(o, dict):
        return {k: strip_empty(v) for k, v in o.items() if v is not None and v != [] and v != {} and v is not False}
    if isinstance(o, list):
        return [strip_empty(x) for x in o]
    return o
