"""Generate a pure-Python (ctypes) openDAQ binding from the API description.

    python generators/python_ctypes/generate.py [--api output/opendaq_api.json]
        [--out output/bindings/python] [--audience module]

Nothing here reads a header. Interface ids, vtable slots, parameter
directions and types, element types of lists and dicts, enums, factories,
libraries and error codes all come from the JSON. `--audience` sets the
lowest audience that is bound: `application` binds the application API,
`module` (default) adds the module API, `core` binds everything. Methods
whose ABI is C++-only keep their slot but are not bound.
"""

import argparse
import json
import keyword
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.normpath(os.path.join(HERE, "..", ".."))
RANK = {"application": 0, "module": 1, "core": 2}

CTYPE = {"uint8": "ctypes.c_uint8", "int8": "ctypes.c_int8", "uint16": "ctypes.c_uint16", "int16": "ctypes.c_int16",
         "uint32": "ctypes.c_uint32", "int32": "ctypes.c_int32", "uint64": "ctypes.c_uint64", "int64": "ctypes.c_int64",
         "float64": "ctypes.c_double", "float32": "ctypes.c_float", "size": "ctypes.c_size_t", "bool": "ctypes.c_bool",
         "cstring": "ctypes.c_char_p", "guid": "_rt.GUID", "void": "None"}
ENUM_CT = {"uint8_t": "ctypes.c_uint8", "int8_t": "ctypes.c_int8", "uint16_t": "ctypes.c_uint16", "int16_t": "ctypes.c_int16",
           "uint32_t": "ctypes.c_uint32", "int32_t": "ctypes.c_int32", "uint64_t": "ctypes.c_uint64",
           "int64_t": "ctypes.c_int64", "EnumType": "ctypes.c_uint32", "int": "ctypes.c_int32", "uint8": "ctypes.c_uint8",
           "Int": "ctypes.c_int64", "UInt": "ctypes.c_uint64", "char": "ctypes.c_int8", "uint32": "ctypes.c_uint32"}


def pyname(n):
    n = n or "arg"
    return n + "_" if keyword.iskeyword(n) or n in ("self", "convert") else n


def snake(n):
    return re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", n).lower()


class Gen:
    def __init__(self, api, audience):
        self.api = api
        self.limit = RANK[audience]
        self.I = {i["name"]: i for i in api["interfaces"]}
        self.E = {e["name"]: e for e in api["enums"] if e.get("header")}
        self.S = api["scalars"]
        self.ST = {s["name"]: s for s in api.get("structs", [])}
        self.CB = {c["name"]: c for c in api.get("callbacks", [])}
        self.skipped = []
        self.bound_methods = 0

    def allowed(self, aud):
        return aud is None or RANK.get(aud, 0) <= self.limit

    # -------------------------------------------------------------- types
    def scalar_ct(self, base):
        s = self.S.get(base)
        return CTYPE[s["abi"]] if s else None

    def enum_ct(self, base):
        return ENUM_CT.get((self.E[base].get("underlying") or "int").replace("daq::", "").strip(), "ctypes.c_int32")

    def marshal(self, p, where):
        """(argtype, call expression, pre-code lines, out handling or None, python param or None)."""
        kind, base, ptr = p.get("kind"), re.sub(r"<.*", "", p["base"]), p.get("pointer", 0)
        name = pyname(p.get("name") or "arg")
        d = p.get("direction", "in")
        default = p.get("default")
        pdef = None
        if default is not None:
            pdef = {"nullptr": "None", "False": "False", "True": "True", "false": "False", "true": "True"}.get(default)
            if pdef is None and re.match(r"^-?\d+(\.\d+)?$", default):
                pdef = default
            if pdef is None:
                pdef = "None"
        param = f"{name}={pdef}" if pdef is not None else name
        if p.get("arraySize") is not None or (p.get("pointer", 0) >= 1 and base == "void"):
            if d == "out" and ptr == 2:
                v = f"_o_{name}"
                return ("ctypes.POINTER(ctypes.c_void_p)", f"ctypes.byref({v})", [f"{v} = ctypes.c_void_p()"], f"{v}.value", None)
            return ("ctypes.c_void_p", f"{name}", [], None, param)
        if kind == "interface":
            if ptr == 1 and d in ("in", "inout"):
                return ("ctypes.c_void_p", f'_rt.arg_ptr({name}, "{base}", _keep)', [], None, param)
            if ptr == 2:
                v = f"_o_{name}"
                extra = ""
                if p.get("elementType"):
                    extra = f', element="{p["elementType"]}"'
                if p.get("keyType"):
                    extra = f', key="{p["keyType"]}", value="{p["valueType"]}"'
                return ("ctypes.POINTER(ctypes.c_void_p)", f"ctypes.byref({v})", [f"{v} = ctypes.c_void_p()"],
                        f'_rt.wrap({v}.value, "{base}", convert{extra})', None)
        if kind == "scalar":
            ct = self.scalar_ct(base)
            if base in ("ConstCharPtr", "CharPtr") or (base == "char" and ptr == 1):
                if ptr == (1 if base == "char" else 0) and d == "in":
                    return ("ctypes.c_char_p", f"_rt.cstr({name})", [], None, param)
                if d == "out":
                    v = f"_o_{name}"
                    return ("ctypes.POINTER(ctypes.c_char_p)", f"ctypes.byref({v})", [f"{v} = ctypes.c_char_p()"],
                            f"_rt.pystr({v}.value)", None)
            if base == "IntfID":
                if p.get("reference") or (ptr == 1 and (p.get("const") or d == "in")):
                    return ("ctypes.POINTER(_rt.GUID)", f"ctypes.byref(_rt.GUID.parse({name}._iid if isinstance({name}, type) else {name}))",
                            [], None, param)
                if ptr == 1:
                    v = f"_o_{name}"
                    return ("ctypes.POINTER(_rt.GUID)", f"ctypes.byref({v})", [f"{v} = _rt.GUID()"], f"str({v})", None)
            if ptr == 0:
                conv = f"int(bool({name}))" if base in ("Bool", "daq::Bool") else name
                return (ct, conv, [], None, param)
            if ptr == 1 and d in ("out", "inout"):
                v = f"_o_{name}"
                init = f"{ct}({name})" if d == "inout" else f"{ct}()"
                res = f"bool({v}.value)" if base == "Bool" else f"{v}.value"
                return (f"ctypes.POINTER({ct})", f"ctypes.byref({v})", [f"{v} = {init}"], res,
                        param if d == "inout" else None)
        if kind == "enum":
            ct = self.enum_ct(base)
            if ptr == 0:
                return (ct, f"int({name})", [], None, param)
            if ptr == 1:
                v = f"_o_{name}"
                return (f"ctypes.POINTER({ct})", f"ctypes.byref({v})", [f"{v} = {ct}()"], f"_enum({base}, {v}.value)", None)
        if kind == "struct":
            if ptr == 0:
                conv = f"_to_{base}({name})"
                return (f"_S_{base}", conv, [], None, param)
            if ptr == 1:
                v = f"_o_{name}"
                return (f"ctypes.POINTER(_S_{base})", f"ctypes.byref({v})", [f"{v} = _S_{base}()"], f"_from_{base}({v})", None)
        if kind == "callback":
            return (f"_CB_{base}", f"_cb_{base}({name}, _keep)", [], None, param)
        raise ValueError(f"{where}: cannot marshal {p.get('cppType')} ({kind}, ptr {ptr}, {d})")

    # -------------------------------------------------------------- emit
    def method(self, iname, m, slot):
        args, pre, call_args, outs, params, argtypes = [], [], [], [], [], []
        for p in m.get("params", []):
            at, expr, pc, out, param = self.marshal(p, f"{iname}::{m['name']}")
            argtypes.append(at)
            call_args.append(expr)
            pre.extend(pc)
            if out:
                outs.append(out)
            if param:
                params.append(param)
        # required params before defaulted ones (C++ allows defaults only at the end, but outs interleave)
        req = [x for x in params if "=" not in x]
        opt = [x for x in params if "=" in x]
        sig = ", ".join(["self"] + req + opt + ["*", "convert=True"])
        doc = (m.get("doc") or {}).get("brief") or ""
        errs = ", ".join(e["code"].replace("OPENDAQ_ERR_", "") for e in m.get("errors", []))
        lines = [f"    def {m['name']}({sig}):"]
        ds = doc + (f" Errors: {errs}." if errs else "")
        if ds:
            lines.append(f"        {json.dumps(ds)}")
        lines.append("        _keep = []")
        lines += [f"        {x}" for x in pre]
        restype = "ctypes.c_int" if m["returns"] == "int" else "ctypes.c_uint32"
        call = f"_rt.vcall(self._ptr, {slot}, {restype}, [{', '.join(argtypes)}]{''.join(', ' + a for a in call_args)})"
        if m["returns"] == "int":
            lines.append(f"        return {call}")
            return "\n".join(lines)
        lines.append(f'        _rt.check({call}, "{iname}::{m["name"]}")')
        if not outs:
            lines.append("        return None")
        elif len(outs) == 1:
            lines.append(f"        return {outs[0]}")
        else:
            lines.append(f"        return ({', '.join(outs)})")
        return "\n".join(lines)

    def interface(self, i, slot0):
        name = i["name"]
        base = i.get("base")
        parent = base if base in self.I else "_rt.Object"
        body = [f"class {name}({parent}):"]
        brief = (i.get("doc") or {}).get("brief")
        if brief:
            body.append(f"    {json.dumps(brief)}")
        body.append(f'    _iid = "{i["guid"]}"')
        body.append(f'    _name = "{name}"')
        body.append(f'    _audience = "{i.get("audience")}"')
        slot = slot0
        getters, setters = {}, {}
        for m in i.get("methods", []):
            aud = m.get("audience") or i.get("audience")
            if m.get("abi") != "c":
                self.skipped.append(f"{name}::{m['name']} (C++-only ABI)")
            elif not self.allowed(aud):
                self.skipped.append(f"{name}::{m['name']} ({aud})")
            else:
                try:
                    body.append(self.method(name, m, slot))
                    self.bound_methods += 1
                    ins = [p for p in m.get("params", []) if p.get("direction", "in") == "in"]
                    outs = [p for p in m.get("params", []) if p.get("direction") == "out"]
                    if m["name"].startswith(("get", "is")) and not ins and len(outs) == 1 and len(m["name"]) > 3:
                        getters[m["name"]] = m
                    if m["name"].startswith("set") and len(ins) == 1 and not outs:
                        setters[m["name"][3:]] = m
                except ValueError as e:
                    self.skipped.append(str(e))
            slot += 1
        for g in getters:
            stem = g[3:] if g.startswith("get") else g
            prop = snake(stem)
            if prop in ("ptr", "query_interface", "cast_to", "supports_interface") or keyword.iskeyword(prop) or prop.startswith("_"):
                continue
            if g.startswith("is"):
                prop = snake(g)
            setter = setters.get(stem)
            if setter:
                body.append(f"    {prop} = _prop(lambda self: self.{g}(), lambda self, v: self.{setter['name']}(v))")
            else:
                body.append(f"    {prop} = _prop(lambda self: self.{g}())")
        body.append(f'_rt._registry["{name}"] = {name}')
        return "\n".join(body), slot

    def build(self):
        api = self.api
        out = ["# Generated from the openDAQ API description. Do not edit.",
               f"# openDAQ {api.get('openDaqVersion')} at {api.get('source', {}).get('commit')}",
               "import ctypes", "import enum", "_prop = property", "from . import _runtime as _rt", "from ._runtime import OpenDaqError, set_binary_dir", "",
               f"_rt._lib_names.update({json.dumps(api['libraries'])})", "",
               "def _enum(cls, v):", "    try:", "        return cls(v)", "    except ValueError:", "        return v", ""]
        info = {e["value"]: e["name"] for e in api["errors"] if e["name"] in ("OPENDAQ_IGNORED",) and e.get("value")}
        out.append("_rt.INFO_CODES.update({" + ", ".join(f"{k}: {v!r}" for k, v in info.items()) + "})")
        # errors
        names = set()
        for e in api["errors"]:
            if e.get("value") is None or not e["name"].startswith("OPENDAQ_ERR_"):
                continue
            cls = e["exceptions"][0] if e.get("exceptions") else "".join(w.capitalize() for w in e["name"][12:].split("_")) + "Error"
            if cls in names:
                cls = "".join(w.capitalize() for w in e["name"][12:].split("_")) + "Error"
            names.add(cls)
            out.append(f"class {cls}(OpenDaqError):\n    code = {e['value']}\n    name = \"{e['name']}\"\n_rt._error_classes[{e['value']}] = {cls}\n")
        # enums
        for e in sorted(self.E.values(), key=lambda x: x["name"]):
            if e.get("outer") or not self.allowed(e.get("audience")):
                continue
            mem = [(m["name"], m["value"]) for m in e["members"] if m.get("value") is not None]
            if not mem:
                continue
            out.append(f"class {e['name']}(enum.IntEnum):")
            out += [f"    {pyname(n)} = {v}" for n, v in mem]
            out.append("")
        # structs
        for s in self.ST.values():
            fields = []
            for f in s["fields"]:
                t = f["type"].replace("const ", "").strip()
                ct = "ctypes.c_char_p" if t in ("char *", "char*") else self.scalar_ct(t) or {"double": "ctypes.c_double", "float": "ctypes.c_float"}.get(t, "ctypes.c_void_p")
                fields.append(f'("{f["name"]}", {ct})')
            out.append(f"class _S_{s['name']}(ctypes.Structure):")
            if s.get("pack"):
                out.append(f"    _pack_ = {s['pack']}")
            out.append(f"    _fields_ = [{', '.join(fields)}]\n")
            if s["name"].startswith("ComplexFloat"):
                out.append(f"def _to_{s['name']}(v):\n    v = complex(v)\n    return _S_{s['name']}(v.real, v.imag)\n")
                out.append(f"def _from_{s['name']}(s):\n    return complex(s.real, s.imaginary)\n")
            else:
                out.append(f"def _to_{s['name']}(v):\n    return v if isinstance(v, _S_{s['name']}) else _S_{s['name']}(*v)\n")
                out.append(f"def _from_{s['name']}(s):\n    return s\n")
        # callbacks: Python callables as C function pointers
        for c in self.CB.values():
            if c["name"] not in ("FuncCall", "ProcCall"):
                continue
            n_out = sum(1 for p in c["params"] if p.get("pointer") == 2)
            argt = ", ".join("ctypes.c_void_p" if p.get("pointer") == 1 else "ctypes.POINTER(ctypes.c_void_p)" for p in c["params"])
            out.append(f"_CB_{c['name']} = ctypes.CFUNCTYPE(ctypes.c_uint32, {argt})")
            out.append(f"def _cb_{c['name']}(fn, keep):")
            out.append("    def tramp(args" + (", result" if n_out else "") + "):")
            out.append("        try:")
            out.append("            a = _rt.wrap(args, \"IBaseObject\", True, ) if args else None")
            out.append("            if a is not None and _rt._registry['IBaseObject'] and isinstance(a, list):")
            out.append("                r = fn(*a)")
            out.append("            else:")
            out.append("                r = fn() if a is None else fn(a)")
            if n_out:
                out.append("            o = _rt.from_python(r)")
                out.append("            if o is not None:")
                out.append("                _rt.vcall(o._ptr, 1, ctypes.c_int, [])")
                out.append("            result[0] = o._ptr if o is not None else None")
            out.append("            return 0")
            out.append("        except Exception:")
            out.append("            return 0x80000014")
            out.append(f"    cb = _CB_{c['name']}(tramp)")
            out.append("    _callbacks.append(cb)  # the openDAQ object may call it at any time; keep it alive")
            out.append("    return cb\n")
        out.insert(out.index("def _enum(cls, v):"), "_callbacks = []")
        # interfaces in base-first order
        done = set()
        order = []

        def visit(n):
            if n in done or n not in self.I:
                return
            visit(self.I[n].get("base"))
            done.add(n)
            order.append(n)
        for n in sorted(self.I):
            visit(n)
        slots = {}
        for n in order:
            i = self.I[n]
            if i.get("templateParams"):
                continue
            base = i.get("base")
            start = slots.get(base, 0) if base else 0
            code, end = self.interface(i, start)
            slots[n] = end
            if self.allowed(i.get("audience")) or any(self.allowed(self.I[x].get("audience")) for x in order if self.I[x].get("base") == n):
                out.append(code + "\n")
            else:
                out.append(f'class {n}({base if base in self.I else "_rt.Object"}):\n    _iid = "{i["guid"]}"\n    _name = "{n}"\n_rt._registry["{n}"] = {n}\n')
        # factories
        fnames = set()
        for f in sorted(api["factories"], key=lambda x: x["name"]):
            if not f.get("createFunction") or not f.get("exported") or f.get("abi") != "c" or not self.allowed(f.get("audience")):
                continue
            if f["returns"] not in self.I:
                continue
            nm = f["name"]
            if nm in fnames or nm in self.I or nm in self.E:
                nm = nm + "Factory"
            fnames.add(nm)
            params, argtypes, call_args, pre = [], ["ctypes.POINTER(ctypes.c_void_p)"], ["ctypes.byref(_o)"], []
            try:
                for p in f.get("params", []):
                    p = dict(p, direction="in")
                    at, expr, pc, o, param = self.marshal(p, f"factory {nm}")
                    if param is None:
                        raise ValueError(f"factory {nm}: parameter {p.get('name')} ({p.get('cppType')}) is not an input")
                    argtypes.append(at)
                    call_args.append(expr)
                    pre += pc
                    params.append(param)
            except ValueError as e:
                self.skipped.append(str(e))
                continue
            req = [x for x in params if "=" not in x]
            opt = [x for x in params if "=" in x]
            brief = (f.get("doc") or {}).get("brief") or ""
            out.append(f"def {nm}({', '.join(req + opt + ['*', 'convert=False'])}):")
            if brief:
                out.append(f"    {json.dumps(brief)}")
            out.append("    _keep = []")
            out.append("    _o = ctypes.c_void_p()")
            out += [f"    {x}" for x in pre]
            out.append(f'    _f = _rt.lib("{f["library"]}").{f["createFunction"]}')
            out.append("    _f.restype = ctypes.c_uint32")
            out.append(f"    _f.argtypes = [{', '.join(argtypes)}]")
            out.append(f'    _rt.check(_f({", ".join(call_args)}), "{f["createFunction"]}")')
            out.append(f'    return _rt.wrap(_o.value, "{f["returns"]}", convert)\n')
        return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", default=os.path.join(TOOL, "output", "opendaq_api.json"))
    ap.add_argument("--out", default=os.path.join(TOOL, "output", "bindings", "python"))
    ap.add_argument("--audience", default="module", choices=list(RANK))
    a = ap.parse_args()
    api = json.load(open(a.api, encoding="utf-8"))
    g = Gen(api, a.audience)
    code = g.build()
    pkg = os.path.join(a.out, "opendaq_ct")
    os.makedirs(pkg, exist_ok=True)
    shutil.copy(os.path.join(HERE, "runtime.py"), os.path.join(pkg, "_runtime.py"))
    with open(os.path.join(pkg, "__init__.py"), "w", encoding="utf-8", newline="\n") as f:
        f.write(code)
    with open(os.path.join(pkg, "SKIPPED.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(g.skipped) + "\n")
    print(f"{pkg}: {g.bound_methods} methods bound, {len(g.skipped)} skipped (see SKIPPED.txt), {len(code) // 1024} KiB")


if __name__ == "__main__":
    sys.exit(main())
