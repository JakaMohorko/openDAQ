"""Generate a plain C header of the openDAQ ABI from the API description.

    python generators/c_header/generate.py [--api ...] [--out output/bindings/c/opendaq_abi.h]
        [--audience module]

C names carry a `daq` prefix (`daqIString`, `daqIStringVtbl`) so they never clash with
platform headers (Windows declares its own IUnknown and IErrorInfo). One struct of
function pointers per interface, in vtable order (the base
interface's slots first), interface ids as constants, enums, by-value
structs, callbacks and the exported factory prototypes. Methods whose ABI
is C++-only, or whose audience is below `--audience`, keep their slot as an
opaque pointer so the layout never shifts. A C compiler checks the result
against the real libraries in checks/test_c_header.c.
"""

import argparse
import json
import os
import re
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.normpath(os.path.join(HERE, "..", ".."))
RANK = {"application": 0, "module": 1, "core": 2}
C_SCALAR = {"uint8": "uint8_t", "int8": "int8_t", "uint16": "uint16_t", "int16": "int16_t", "uint32": "uint32_t",
            "int32": "int32_t", "uint64": "uint64_t", "int64": "int64_t", "float64": "double", "float32": "float",
            "size": "size_t", "bool": "bool", "void": "void", "guid": "daqIntfID"}
C_ENUM_BASE = {"uint8_t": "uint8_t", "int8_t": "int8_t", "uint16_t": "uint16_t", "int16_t": "int16_t", "uint32_t": "uint32_t",
               "int32_t": "int32_t", "uint64_t": "uint64_t", "int64_t": "int64_t", "EnumType": "uint32_t", "int": "int32_t",
               "Int": "int64_t", "UInt": "uint64_t", "char": "int8_t"}


def guid_init(text):
    u = uuid.UUID(text)
    d4 = ", ".join(f"0x{b:02X}" for b in u.bytes[8:])
    return f"{{0x{u.fields[0]:08X}u, 0x{u.fields[1]:04X}u, 0x{u.fields[2]:04X}u, {{{d4}}}}}"


class Gen:
    def __init__(self, api, audience):
        self.api = api
        self.limit = RANK[audience]
        self.I = {i["name"]: i for i in api["interfaces"]}
        self.E = {e["name"]: e for e in api["enums"] if e.get("header") and not e.get("outer")}
        self.S = api["scalars"]
        self.ST = {s["name"]: s for s in api.get("structs", [])}
        self.CB = {c["name"]: c for c in api.get("callbacks", [])}
        self.opaque = 0

    def allowed(self, aud):
        return aud is None or RANK.get(aud, 0) <= self.limit

    def ctype(self, p):
        """C spelling of a parameter, or None when it has no C form."""
        base = re.sub(r"<.*", "", p["base"])
        kind, ptr = p.get("kind"), p.get("pointer", 0)
        const = "const " if p.get("const") is True else ""
        if kind == "interface":
            return f"daq{base}{'*' * ptr}"
        if kind == "enum":
            return f"daq{base}{'*' * ptr}"
        if kind == "struct":
            return f"{const}daq{base}{'*' * ptr}"
        if kind == "callback":
            return f"daq{base}"
        if kind == "scalar":
            if base == "IntfID":
                return f"const daqIntfID*" if p.get("reference") or (ptr and p.get("const")) else f"daqIntfID{'*' * max(ptr, 1)}"
            if base == "ConstCharPtr":
                return "const char" + "*" * (ptr + 1)
            if base == "CharPtr":
                return "char" + "*" * (ptr + 1)
            if base == "char":
                return f"{const}char{'*' * ptr}"
            s = self.S.get(base)
            if not s:
                return None
            ct = C_SCALAR[s["abi"]]
            if s["abi"] == "cstring":
                return "const char" + "*" * (ptr + 1)
            if base in ("Bool",):
                ct = "daqBool"
            return f"{const}{ct}{'*' * ptr}"
        if base == "void" and ptr >= 1:
            return f"{const}void{'*' * ptr}"
        return None

    def method_ptr(self, iname, m, slot):
        rt = "int" if m["returns"] == "int" else "daqErrCode"
        args = [f"daq{iname}* self"]
        for p in m.get("params", []):
            ct = self.ctype(p)
            if ct is None:
                return None
            args.append(f"{ct} {p.get('name') or 'arg'}")
        return f"    {rt} (DAQ_CALL *{m['name']})({', '.join(args)});"

    def chain(self, name):
        out = []
        x = name
        while x and x in self.I:
            out.insert(0, x)
            x = self.I[x].get("base")
        return out

    def build(self):
        a = self.api
        L = ["/* Generated from the openDAQ API description. Do not edit. */",
             f"/* openDAQ {a.get('openDaqVersion')} at {a.get('source', {}).get('commit')} */",
             "#ifndef OPENDAQ_ABI_H", "#define OPENDAQ_ABI_H", "",
             "#include <stdint.h>", "#include <stddef.h>", "#include <stdbool.h>", "",
             "#if defined(_WIN32) && !defined(_WIN64)", "#define DAQ_CALL __stdcall", "#else", "#define DAQ_CALL", "#endif",
             "#ifdef __cplusplus", 'extern "C" {', "#endif", "",
             "typedef uint32_t daqErrCode;", "typedef uint8_t daqBool;",
             "typedef struct daqIntfID { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } daqIntfID;", "",
             "/* error codes */"]
        for e in a["errors"]:
            if e.get("value") is not None:
                L.append(f"#define {e['name']} 0x{e['value']:08X}u")
        L += ["", "/* enums */"]
        for e in sorted(self.E.values(), key=lambda x: x["name"]):
            under = C_ENUM_BASE.get((e.get("underlying") or "int").replace("daq::", "").strip(), "int32_t")
            L.append(f"typedef {under} daq{e['name']};")
            for mem in e["members"]:
                if mem.get("value") is not None:
                    L.append(f"#define daq{e['name']}_{mem['name']} (({under}){mem['value']}{'u' if mem['value'] >= 0 and not under.startswith('int') else ''})")
        L += ["", "/* structs passed by value */"]
        for s in self.ST.values():
            if s.get("pack"):
                L.append(f"#pragma pack(push, {s['pack']})")
            fields = []
            for f in s["fields"]:
                t = f["type"].replace("const ", "").strip()
                ct = {"double": "double", "float": "float", "char *": "const char*", "Int": "int64_t"}.get(t, "void*")
                fields.append(f"    {ct} {f['name']};")
            L += [f"typedef struct daq{s['name']} {{"] + fields + [f"}} daq{s['name']};"]
            if s.get("pack"):
                L.append("#pragma pack(pop)")
        L += ["", "/* interfaces */"]
        names = sorted(n for n, i in self.I.items() if not i.get("templateParams"))
        L += [f"typedef struct daq{n} daq{n};" for n in names]
        L += [""]
        for c in self.CB.values():
            args = []
            ok = True
            for p in c["params"]:
                ct = self.ctype(dict(p, kind="interface" if re.match(r"^I[A-Z]", p["base"]) else p.get("kind", "scalar")))
                if ct is None:
                    ok = False
                args.append(ct or "void*")
            if ok:
                L.append(f"typedef daqErrCode (*daq{c['name']})({', '.join(args)});")
        L.append("")
        for n in names:
            i = self.I[n]
            L.append(f"static const daqIntfID daqIID_{n} = {guid_init(i['guid'])};")
        L.append("")
        for n in names:
            i = self.I[n]
            brief = (i.get("doc") or {}).get("brief")
            if brief:
                L.append(f"/* {brief.replace('*/', '* /')} */")
            L.append(f"typedef struct daq{n}Vtbl {{")
            slot = 0
            for c in self.chain(n):
                L.append(f"    /* {c} */")
                seen = set()
                for m in self.I[c].get("methods", []):
                    aud = m.get("effectiveAudience") or self.I[c].get("audience")
                    line = None
                    if m.get("abi") == "c" and self.allowed(aud):
                        line = self.method_ptr(n, m, slot)
                    name = m["name"] if m["name"] not in seen else f"{m['name']}_{slot}"
                    seen.add(m["name"])
                    if line is None:
                        line = f"    void* _slot{slot}_{m['name']};  /* {'C++ only' if m.get('abi') != 'c' else aud} */"
                        self.opaque += 1
                    elif name != m["name"]:
                        line = line.replace(f"*{m['name']})", f"*{name})")
                    L.append(line)
                    slot += 1
            L += [f"}} daq{n}Vtbl;", f"struct daq{n} {{ const daq{n}Vtbl* vtbl; }};", ""]
        L.append("/* exported factories */")
        for f in sorted(a["factories"], key=lambda x: x["name"]):
            if not f.get("createFunction") or not f.get("exported") or f.get("abi") != "c" or not self.allowed(f.get("audience")):
                continue
            if f["returns"] not in self.I:
                continue
            args = [f"daq{f['returns']}** obj"]
            ok = True
            for p in f.get("params", []):
                ct = self.ctype(p)
                if ct is None:
                    ok = False
                    break
                args.append(f"{ct} {p.get('name') or 'arg'}")
            if ok:
                L.append(f"daqErrCode {f['createFunction']}({', '.join(args)});  /* {f.get('library')} */")
        L += ["", "#ifdef __cplusplus", "}", "#endif", "#endif", ""]
        return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", default=os.path.join(TOOL, "output", "opendaq_api.json"))
    ap.add_argument("--out", default=os.path.join(TOOL, "output", "bindings", "c", "opendaq_abi.h"))
    ap.add_argument("--audience", default="module", choices=list(RANK))
    a = ap.parse_args()
    with open(a.api, encoding="utf-8") as f:
        api = json.load(f)
    g = Gen(api, a.audience)
    text = g.build()
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"{a.out}: {len(text) // 1024} KiB, {g.opaque} opaque slots")


if __name__ == "__main__":
    sys.exit(main())
