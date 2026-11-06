"""Dump a type catalog for the setup tool from the modules in a build.

    OPENDAQ_BIN=<build>/bin python setup/catalog.py [--out output/setup/catalog.json]
        [--device daqref://device0 ...]

A loaded setup never creates channels, signals, input ports or IO folders:
they come from the device or function block type. To let a setup be
written without a device attached, this tool instantiates each function
block type and each simulated device once, through the ctypes binding
generated from the API description, and records the component tree they
produce: local IDs, serialize IDs, attributes, signals, input ports,
nested function block types, and every property with its metadata.
"""

import argparse
import json
import os
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TOOL, "output", "bindings", "python"))
import opendaq_ct as daq  # noqa: E402

CT = {int(v): k for k, v in daq.CoreType.__members__.items()}


def value_desc(v):
    """A JSON-able description of a property value."""
    if v is None or isinstance(v, (bool, int, float, str)):
        return v
    if isinstance(v, Fraction):
        return {"__type": "Ratio", "num": v.numerator, "den": v.denominator}
    if isinstance(v, complex):
        return {"__type": "ComplexNumber", "real": v.real, "imaginary": v.imag}
    if isinstance(v, list):
        return [value_desc(x) for x in v]
    if isinstance(v, dict):
        return {"__type": "Dict", "values": [{"key": value_desc(k), "value": value_desc(x)} for k, x in v.items()]}
    if isinstance(v, daq._rt.Object):
        e = v.query(daq.IEnumeration)
        if e is not None:
            return {"__type": "Enumeration", "typeName": e.getEnumerationType().getName(), "value": e.getValue()}
        s = v.query(daq.IStruct)
        if s is not None:
            names = s.getFieldNames()
            vals = s.getFieldValues()
            return {"__type": "Struct", "typeName": s.getStructType().getName(),
                    "fields": {n: value_desc(x) for n, x in zip(names, vals)}}
        po = v.query(daq.IPropertyObject)
        if po is not None:
            sid = safe(lambda: v.cast(daq.ISerializable).getSerializeId(), "PropertyObject")
            return {"__type": sid, "className": safe(po.getClassName) or None, "properties": properties(po)}
        u = v.query(daq.IUnit)
        if u is not None:
            return {"__type": "Unit", "symbol": u.getSymbol(), "name": u.getName(), "quantity": u.getQuantity(), "id": u.getId()}
        return {"__object": v._name}
    return repr(v)


def safe(fn, default=None):
    try:
        return fn()
    except Exception:
        return default


def properties(po):
    out = []
    for p in po.getAllProperties():
        name = p.getName()
        vt = int(p.getValueType())
        rec = {"name": name, "valueType": CT.get(vt, vt)}
        for key, getter in (("description", p.getDescription), ("readOnly", p.getReadOnly), ("visible", p.getVisible)):
            v = safe(getter)
            if v not in (None, "", False) or key == "visible":
                rec[key] = v
        ref = safe(p.getIsReferenced)
        if ref:
            rec["isReferenced"] = True
        rp = safe(p.getReferencedProperty, None)
        if rp is not None:
            rec["referencedProperty"] = True
        if vt in (4, 5):
            it = safe(p.getItemType)
            if it is not None:
                rec["itemType"] = CT.get(int(it), int(it))
            if vt == 5:
                kt = safe(p.getKeyType)
                if kt is not None:
                    rec["keyType"] = CT.get(int(kt), int(kt))
        for key, getter in (("default", p.getDefaultValue), ("min", p.getMinValue), ("max", p.getMaxValue),
                            ("selectionValues", p.getSelectionValues), ("suggestedValues", p.getSuggestedValues)):
            v = safe(getter)
            if v is not None:
                rec[key] = value_desc(v)
        unit = safe(p.getUnit)
        if unit is not None:
            rec["unit"] = safe(lambda: unit.getSymbol())
        st = safe(p.getStructType)
        if st is not None:
            rec["structType"] = safe(lambda: st.getName())
        if vt not in (7, 10):  # procedures and functions have no value to save
            cur = safe(lambda: po.getPropertyValue(name))
            if cur is not None:
                cv = value_desc(cur)
                if cv != rec.get("default"):
                    rec["value"] = cv
        out.append(rec)
    return out


def component(c):
    rec = {"localId": c.getLocalId(), "serializeId": safe(lambda: c.cast(daq.ISerializable).getSerializeId()),
           "name": c.getName(), "description": safe(c.getDescription, ""), "active": c.getActive(),
           "visible": c.getVisible()}
    locked = safe(c.getLockedAttributes, [])
    if locked:
        rec["lockedAttributes"] = locked
    sig = c.query(daq.ISignal)
    if sig is not None:
        rec["public"] = sig.getPublic()
        dd = safe(sig.getDescriptor)
        if dd is not None:
            rec["sampleType"] = safe(lambda: daq.SampleType(int(dd.getSampleType())).name)
            rec["unit"] = safe(lambda: dd.getUnit().getSymbol())
        rec["globalIdSuffix"] = None
    fb = c.query(daq.IFunctionBlock)
    if fb is not None:
        rec["typeId"] = safe(lambda: fb.getFunctionBlockType().getId())
        rec["nestedFunctionBlockTypes"] = sorted(safe(lambda: list(fb.getAvailableFunctionBlockTypes().keys()), []))
    dev = c.query(daq.IDevice)
    if dev is not None:
        rec["functionBlockTypes"] = sorted(safe(lambda: list(dev.getAvailableFunctionBlockTypes().keys()), []))
        info = safe(dev.getInfo)
        if info is not None:
            rec["info"] = {k: safe(getattr(info, g)) for k, g in (("manufacturer", "getManufacturer"),
                                                                  ("serialNumber", "getSerialNumber"),
                                                                  ("model", "getModel"),
                                                                  ("connectionString", "getConnectionString"))}
    po = c.query(daq.IPropertyObject)
    if po is not None:
        rec["properties"] = properties(po)
    folder = c.query(daq.IFolder)
    if folder is not None:
        rec["children"] = [component(x) for x in folder.getItems(daq.AnySearchFilter())]
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(TOOL, "output", "setup", "catalog.json"))
    ap.add_argument("--device", nargs="*", default=["daqref://device0"],
                    help="connection strings of devices that exist without hardware (simulators)")
    a = ap.parse_args()
    binp = os.environ.get("OPENDAQ_BIN")
    if not binp:
        sys.exit("set OPENDAQ_BIN to the directory with the openDAQ libraries and modules")
    daq.set_binary_dir(binp)
    b = daq.InstanceBuilder()
    b.setModulePath(binp)
    b.setDefaultRootDeviceLocalId("openDAQDevice")
    inst = daq.InstanceFromBuilder(b)
    root = inst.getRootDevice()
    cat = {"formatVersion": 1, "source": binp, "rootLocalId": root.getLocalId(), "root": component(root),
           "functionBlockTypes": {}, "deviceTypes": {}, "devices": {}}
    for tid, t in inst.getAvailableFunctionBlockTypes().items():
        entry = {"id": tid, "name": t.getName(), "description": t.getDescription()}
        try:
            fb = inst.addFunctionBlock(tid)
            entry["template"] = component(fb)
            cfg = safe(t.createDefaultConfig)
            if cfg is not None:
                entry["defaultConfig"] = properties(cfg)
            inst.removeFunctionBlock(fb)
        except daq.OpenDaqError as e:
            entry["error"] = str(e)
        cat["functionBlockTypes"][tid] = entry
    for tid, t in inst.getAvailableDeviceTypes().items():
        cat["deviceTypes"][tid] = {"id": tid, "name": t.getName(), "description": t.getDescription(),
                                   "connectionStringPrefix": safe(t.getConnectionStringPrefix)}
    for cs in a.device:
        try:
            dev = inst.addDevice(cs)
            cat["devices"][cs] = {"connectionString": cs, "localId": dev.getLocalId(), "template": component(dev)}
            inst.removeDevice(dev)
        except daq.OpenDaqError as e:
            cat["devices"][cs] = {"connectionString": cs, "error": str(e)}
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(cat, f, indent=1)
    n_err = sum(1 for x in cat["functionBlockTypes"].values() if "error" in x)
    print(f"{a.out}: {len(cat['functionBlockTypes'])} function block types ({n_err} failed), {len(cat['devices'])} devices")


if __name__ == "__main__":
    main()
