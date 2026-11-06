"""Call every getter of every live object with NULL output pointers.

    OPENDAQ_BIN=<build>/bin python checks/null_probe.py [--report output/null_probe.json]

Builds an instance with the reference modules, crawls the objects reachable
through getters (components, properties, descriptors, types, ...), and for
each distinct set of interfaces calls every method whose parameters are all
outputs, passing NULL. Expected: OPENDAQ_ERR_ARGUMENT_NULL. This checks the
vtable slot of every getter of every interface the objects answer, finds
getters that do not check their output pointer, and finds codes missing
from the description's `errors` lists.
"""

import argparse
import collections
import ctypes
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TOOL, "output", "bindings", "python"))
import opendaq_ct as daq  # noqa: E402

SKIP_CRAWL = {"getAvailableDevices", "getLog", "getLogFileInfos", "getAvailableOperationModes", "getTicksSinceOrigin",
              "getLastValue", "getLastValueWithTimestamp", "getAvailableCount", "getConnectedClientsInfo"}
ARG_NULL = 0x80000026


def chain(I, name):
    out = []
    x = name
    while x in I:
        out.insert(0, x)
        x = I[x].get("base")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", default=os.path.join(TOOL, "output", "null_probe.json"))
    ap.add_argument("--max-objects", type=int, default=400)
    a = ap.parse_args()
    binp = os.environ["OPENDAQ_BIN"]
    with open(os.path.join(TOOL, "output", "opendaq_api.json"), encoding="utf-8") as f:
        api = json.load(f)
    I = {i["name"]: i for i in api["interfaces"]}
    err_name = {e["value"]: e["name"] for e in api["errors"] if e.get("value") is not None}
    daq.set_binary_dir(binp)
    b = daq.InstanceBuilder()
    b.setModulePath(binp)
    inst = daq.InstanceFromBuilder(b)
    dev = inst.addDevice("daqref://device0")
    fb = inst.addFunctionBlock("RefFBModuleStatistics")
    fb.getInputPorts()[0].connect(dev.getChannels()[0].getSignals()[0])
    probe_order = sorted((n for n, i in I.items() if not i.get("templateParams") and i["name"] not in ("IUnknown",)),
                         key=lambda n: -len(chain(I, n)))

    def interfaces_of(obj):
        return [n for n in probe_order if obj.query_interface(n) is not None]

    # crawl
    seen_sets = {}
    queue = collections.deque([inst, dev, fb])
    visited = 0
    while queue and visited < a.max_objects:
        obj = queue.popleft()
        intfs = tuple(interfaces_of(obj))
        if not intfs or intfs in seen_sets:
            continue
        seen_sets[intfs] = obj
        visited += 1
        for iname in intfs:
            typed = obj.query_interface(iname)
            for m in I[iname].get("methods", []):
                ps = m.get("params", [])
                if m["name"] in SKIP_CRAWL or m.get("abi") != "c" or not ps or not m["name"].startswith("get"):
                    continue
                if not (len(ps) == 1 and ps[0].get("direction") == "out" and ps[0].get("kind") == "interface"):
                    continue
                try:
                    v = getattr(typed, m["name"])(convert=False)
                except Exception:
                    continue
                if isinstance(v, daq._rt.Object):
                    lst = v.query_interface("IList")
                    if lst is not None:
                        for k in range(min(lst.getCount(), 8)):
                            item = lst.getItemAt(k, convert=False)
                            if isinstance(item, daq._rt.Object):
                                queue.append(item)
                    else:
                        queue.append(v)
    # probe
    results = []
    calls = 0
    for intfs, obj in seen_sets.items():
        done = set()
        for iname in intfs:
            ptr = obj.query_interface(iname)
            slot = 0
            for c in chain(I, iname):
                for m in I[c].get("methods", []):
                    ps = m.get("params", [])
                    key = (c, m["name"])
                    if (key not in done and c != "IUnknown" and m.get("abi") == "c" and ps
                            and all(p.get("direction") == "out" and p.get("pointer", 0) >= 1 for p in ps)):
                        done.add(key)
                        calls += 1
                        try:
                            code = daq._rt.vcall(ptr._ptr, slot, ctypes.c_uint32, [ctypes.c_void_p] * len(ps), *([None] * len(ps)))
                            outcome = err_name.get(code, hex(code)) if code else "OPENDAQ_SUCCESS"
                        except OSError as e:
                            outcome = f"crash: {e}"
                        try:
                            daq._rt.lib("coretypes").daqClearErrorInfo()
                        except Exception:
                            pass
                        if outcome != "OPENDAQ_ERR_ARGUMENT_NULL":
                            listed = [x["code"] for x in m.get("errors", [])]
                            results.append({"interface": c, "method": m["name"], "object": list(intfs[:3]), "outcome": outcome,
                                            "listed": outcome in listed})
                    slot += 1
    groups = collections.defaultdict(list)
    for r in results:
        groups[r["outcome"] if not r["outcome"].startswith("crash") else "crash"].append(f"{r['interface']}::{r['method']}")
    report = {"objects": len(seen_sets), "calls": calls, "notArgumentNull": len(results),
              "byOutcome": {k: sorted(set(v)) for k, v in groups.items()}, "details": results}
    with open(a.report, "w", encoding="utf-8", newline="\n") as f:
        json.dump(report, f, indent=1)
    print(f"{len(seen_sets)} distinct objects, {calls} getter calls with NULL outputs, "
          f"{len(results)} did not return OPENDAQ_ERR_ARGUMENT_NULL")
    for k, v in sorted(groups.items(), key=lambda x: -len(x[1])):
        print(f"  {k}: {len(set(v))}  e.g. {', '.join(sorted(set(v))[:6])}")


if __name__ == "__main__":
    main()
