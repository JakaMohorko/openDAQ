"""Build openDAQ setups offline.

    from setupkit import Setup
    s = Setup.from_files()                       # output/setup/catalog.json + output/opendaq_api.json
    dev = s.root.add_device("daqref://device0")
    dev.channel("RefCh0").set("Frequency", 15.0)
    fb = s.root.add_function_block("RefFBModuleStatistics", name="Stats")
    fb.input_port("Input").connect(dev.channel("RefCh0").signal("AI0"))
    fb.signal("rms").public = False
    open("setup.json", "w").write(s.dumps())

    python setup/setupkit.py validate <setup.json>

A setup describes the whole tree (spec rule "full-description"): every
component the catalog says the topology has is written with its name,
non-default attributes and non-default property values, plus whatever
you change. Function blocks and sub-devices you add are written with the
data the loader needs to create them (typeId, connectionString).
Everything else follows the rules in the API description's `setup` part.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL)
from apidesc.schema_lite import validate as schema_validate  # noqa: E402

CORE_TO_INTF = {"ctBool": "IBoolean", "ctInt": "IInteger", "ctFloat": "IFloat", "ctString": "IString",
                "ctList": "IList", "ctDict": "IDict", "ctRatio": "IRatio", "ctObject": "IBaseObject",
                "ctStruct": "IStruct", "ctEnumeration": "IEnumeration", "ctComplexNumber": "IComplexNumber"}
FOLDER_KIND = {"Sig": "Signal", "IP": "InputPort", "FB": "FunctionBlock", "Dev": "Device"}


class SetupError(ValueError):
    pass


class Node:
    """One component of the setup tree, backed by a catalog template."""

    def __init__(self, setup, template, parent, local_id=None, added=False, connection_string=None):
        self.setup = setup
        self.t = template or {"localId": local_id, "serializeId": "Device", "name": local_id, "properties": [], "children": []}
        self.parent = parent
        self.local_id = local_id or self.t["localId"]
        self.kind = self.t.get("serializeId")
        self.added = added
        self.connection_string = connection_string
        self.attrs = {}
        self.props = {}
        self.config = {}
        self.signal_id = None
        self.children = {}
        for c in self.t.get("children", []):
            self.children[c["localId"]] = Node(setup, c, self)
        self._added_order = []

    # ---------------------------------------------------------------- identity
    @property
    def global_id(self):
        return "/" + self.local_id if self.parent is None else self.parent.global_id + "/" + self.local_id

    def __repr__(self):
        return f"<{self.kind} {self.global_id}>"

    def child(self, path):
        node = self
        for part in path.strip("/").split("/"):
            if part not in node.children:
                raise SetupError(f"{node.global_id} has no child '{part}' (has {', '.join(node.children) or 'none'})")
            node = node.children[part]
        return node

    def find(self, local_id, kind=None):
        """The first descendant with this local ID (and serialize ID)."""
        for c in self.children.values():
            if c.local_id == local_id and (kind is None or c.kind == kind):
                return c
            r = c.find(local_id, kind)
            if r is not None:
                return r
        return None

    def _find_required(self, local_id, kind, what):
        r = self.find(local_id, kind)
        if r is None:
            raise SetupError(f"{self.global_id} has no {what} '{local_id}'")
        return r

    def signal(self, local_id):
        return self._find_required(local_id, "Signal", "signal")

    def input_port(self, local_id):
        return self._find_required(local_id, "InputPort", "input port")

    def channel(self, local_id):
        return self._find_required(local_id, "Channel", "channel")

    def walk(self):
        yield self
        for c in self.children.values():
            yield from c.walk()

    # ---------------------------------------------------------------- attributes
    def _attr(self, key, default):
        return self.attrs.get(key, self.t.get(key, default))

    name = property(lambda s: s._attr("name", s.local_id), lambda s, v: s._set_attr("name", str(v)))
    description = property(lambda s: s._attr("description", ""), lambda s, v: s._set_attr("description", str(v)))
    active = property(lambda s: s._attr("active", True), lambda s, v: s._set_attr("active", bool(v)))
    visible = property(lambda s: s._attr("visible", True), lambda s, v: s._set_attr("visible", bool(v)))

    @property
    def public(self):
        return self._attr("public", True)

    @public.setter
    def public(self, v):
        if self.kind != "Signal":
            raise SetupError(f"{self.global_id}: only signals have 'public'")
        self._set_attr("public", bool(v))

    def _set_attr(self, k, v):
        if k[0].upper() + k[1:] in self.t.get("lockedAttributes", []):
            raise SetupError(f"{self.global_id}: '{k}' is locked by the component's type; the loader ignores it")
        self.attrs[k] = v

    # ---------------------------------------------------------------- properties
    def property_info(self, name):
        for p in self.t.get("properties", []):
            if p["name"] == name:
                return p
        return None

    def properties(self):
        return [p["name"] for p in self.t.get("properties", [])]

    def set(self, name, value, config=False):
        """Set a property value (config=True: the function block's creation config)."""
        props = self.setup.catalog["functionBlockTypes"].get(self.t.get("typeId"), {}).get("defaultConfig", []) if config \
            else self.t.get("properties", [])
        info = next((p for p in props if p["name"] == name), None)
        if info is None and self.t.get("properties"):
            raise SetupError(f"{self.global_id} has no {'config ' if config else ''}property '{name}' "
                             f"(has {', '.join(p['name'] for p in props)})")
        value = self.setup.check_value(info, value, f"{self.global_id}.{name}")
        (self.config if config else self.props)[name] = value
        return self

    def get(self, name):
        if name in self.props:
            return self.props[name]
        info = self.property_info(name) or {}
        return info.get("value", info.get("default"))

    # ---------------------------------------------------------------- topology
    def add_function_block(self, type_id, local_id=None, name=None):
        if self.kind not in ("Device", "FunctionBlock", "Channel"):
            raise SetupError(f"{self.global_id}: function blocks can only be added to devices and function blocks")
        cat = self.setup.catalog["functionBlockTypes"].get(type_id)
        if not cat or "template" not in cat:
            raise SetupError(f"function block type '{type_id}' is not in the catalog")
        allowed = self.t.get("functionBlockTypes") if self.kind == "Device" else self.t.get("nestedFunctionBlockTypes")
        if allowed is not None and type_id not in allowed:
            raise SetupError(f"{self.global_id} does not accept '{type_id}' (accepts {', '.join(allowed) or 'none'})")
        folder = self.children.get("FB")
        if folder is None:
            folder = self.children["FB"] = Node(self.setup, {"localId": "FB", "serializeId": "Folder", "name": "FB", "children": []}, self)
        if local_id is None:
            n = 1
            while f"{type_id}_{n}" in folder.children:
                n += 1
            local_id = f"{type_id}_{n}"
        if local_id in folder.children:
            raise SetupError(f"{folder.global_id} already has '{local_id}'")
        node = Node(self.setup, cat["template"], folder, local_id=local_id, added=True)
        node.attrs["name"] = name or local_id
        folder.children[local_id] = node
        return node

    def add_device(self, connection_string, local_id=None, name=None):
        if self.kind != "Device":
            raise SetupError(f"{self.global_id}: devices can only be added to devices")
        cat = self.setup.catalog["devices"].get(connection_string)
        template = cat.get("template") if cat else None
        if template is None:
            # same device type, another instance: daqref://device1 -> RefDev1
            prefix = connection_string.split("://")[0]
            for cs, d in self.setup.catalog["devices"].items():
                if cs.split("://")[0] == prefix and d.get("template"):
                    template = d["template"]
                    m = re.search(r"(\d+)$", connection_string)
                    if local_id is None and m and re.search(r"\d+$", d["localId"]):
                        local_id = re.sub(r"\d+$", m.group(1), d["localId"])
        if local_id is None:
            local_id = cat["localId"] if cat else None
        if local_id is None:
            raise SetupError(f"no catalog entry for '{connection_string}': pass the local ID the device's module assigns")
        folder = self.children.get("Dev")
        if folder is None:
            folder = self.children["Dev"] = Node(self.setup, {"localId": "Dev", "serializeId": "Folder", "name": "Dev", "children": []}, self)
        node = Node(self.setup, template, folder, local_id=local_id, added=True, connection_string=connection_string)
        node.attrs["name"] = name or (template or {}).get("name", local_id)
        folder.children[local_id] = node
        return node

    def remove(self):
        if not self.added:
            raise SetupError(f"{self.global_id} comes from its type and cannot be removed")
        del self.parent.children[self.local_id]

    def connect(self, signal):
        if self.kind != "InputPort":
            raise SetupError(f"{self.global_id} is not an input port")
        if isinstance(signal, Node):
            if signal.kind != "Signal":
                raise SetupError(f"{signal.global_id} is not a signal")
            signal = signal.global_id
        self.signal_id = signal
        return self

    # ---------------------------------------------------------------- serialization
    def to_json(self):
        k = self.kind
        if k in ("Folder", "IoFolder"):
            items = {cid: c.to_json() for cid, c in self.children.items()}
            out = {"__type": k, "name": self.name}
            if items:
                out["items"] = items
            return out
        out = {"__type": k, "name": self.name}
        if self.description:
            out["description"] = self.description
        if not self.active:
            out["active"] = False
        if not self.visible:
            out["visible"] = False
        if k == "Signal" and not self.public:
            out["public"] = False
        if k in ("FunctionBlock", "Channel") and self.t.get("typeId"):
            out["typeId"] = self.t["typeId"]
        if k == "Device" and self.connection_string:
            out["connectionString"] = self.connection_string
        if k == "InputPort" and self.signal_id:
            out["signalId"] = self.signal_id
        pv = self.setup.prop_values(self.t.get("properties", []), self.props)
        if k == "Device" and "DaqDeviceInfo" not in pv:
            pv = {"DaqDeviceInfo": {"__type": "DeviceInfo", "propValues": {}}, **pv}  # rule "device-info"
        if pv:
            out["propValues"] = pv
        if k == "FunctionBlock" and self.config:
            cfg = self.setup.catalog["functionBlockTypes"].get(self.t.get("typeId"), {}).get("defaultConfig", [])
            out["ComponentConfig"] = {"__type": "PropertyObject", "propValues": self.setup.prop_values(cfg, self.config)}
        for cid, c in self.children.items():
            if cid in ("Srv", "Synchronization"):
                continue  # ignored by the loader
            if c.kind in ("Folder", "IoFolder"):
                j = c.to_json()
                if j.get("items") or cid in ("FB", "Dev") and self.kind == "Device":
                    out[cid] = j
            else:
                out[cid] = c.to_json()
        return out


class Setup:
    def __init__(self, catalog, api):
        self.catalog = catalog
        self.api = api
        self.guid = {i["name"]: i["guid"] for i in api["interfaces"]}
        self.schema = (api.get("setup") or {}).get("schema")
        self.root = Node(self, catalog["root"], None, local_id=catalog.get("rootLocalId", "openDAQDevice"))

    @classmethod
    def from_files(cls, catalog=None, api=None):
        catalog = catalog or os.path.join(TOOL, "output", "setup", "catalog.json")
        api = api or os.path.join(TOOL, "output", "opendaq_api.json")
        with open(catalog, encoding="utf-8") as c, open(api, encoding="utf-8") as a:
            return cls(json.load(c), json.load(a))

    # ---------------------------------------------------------------- values
    def check_value(self, info, value, where):
        if info is None:
            return value
        vt = info.get("valueType")
        if info.get("readOnly"):
            pass  # the loader writes read-only values too (rule "order")
        if vt == "ctInt" and info.get("selectionValues") is not None:
            sel = info["selectionValues"]
            options = sel if isinstance(sel, list) else [x["value"] for x in sel.get("values", [])]
            keys = list(range(len(options))) if isinstance(sel, list) else [x["key"] for x in sel.get("values", [])]
            if isinstance(value, str):
                if value not in options:
                    raise SetupError(f"{where}: '{value}' is not one of {options}")
                return keys[options.index(value)]
            if value not in keys:
                raise SetupError(f"{where}: selection index {value} is out of range {keys}")
            return int(value)
        if vt == "ctBool":
            if not isinstance(value, bool):
                raise SetupError(f"{where}: expected a bool, got {value!r}")
        elif vt == "ctInt":
            if isinstance(value, bool) or not isinstance(value, int):
                raise SetupError(f"{where}: expected an integer, got {value!r}")
        elif vt == "ctFloat":
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise SetupError(f"{where}: expected a number, got {value!r}")
            value = float(value)
        elif vt == "ctString":
            if not isinstance(value, str):
                raise SetupError(f"{where}: expected a string, got {value!r}")
        elif vt == "ctList":
            if not isinstance(value, list):
                raise SetupError(f"{where}: expected a list, got {value!r}")
        elif vt in ("ctFunc", "ctProc"):
            raise SetupError(f"{where}: functions and procedures have no value a setup can store")
        if vt in ("ctInt", "ctFloat"):
            lo, hi = info.get("min"), info.get("max")
            if isinstance(lo, (int, float)) and value < lo or isinstance(hi, (int, float)) and value > hi:
                raise SetupError(f"{where}: {value} is outside [{lo}, {hi}]")
        return value

    def encode(self, info, v):
        """A property value in setup encoding (rule 'values')."""
        vt = (info or {}).get("valueType")
        if isinstance(v, dict) and v.get("__type") and "properties" in v:
            return self.object_value(v, {})
        if vt == "ctList" and isinstance(v, list):
            out = {"__type": "List", "values": [self.encode(None, x) for x in v]}
            intf = CORE_TO_INTF.get(info.get("itemType")) if info else None
            if intf and intf in self.guid:
                out["itemIntfID"] = "{" + self.guid[intf] + "}"
            return out
        if vt == "ctFloat" and isinstance(v, int) and not isinstance(v, bool):
            return float(v)
        return v

    def object_value(self, desc, overrides):
        out = {"__type": desc.get("__type", "PropertyObject")}
        pv = self.prop_values(desc.get("properties", []), overrides)
        if pv:
            out["propValues"] = pv
        return out

    def prop_values(self, props, overrides):
        pv = {}
        for p in props:
            n = p["name"]
            if p.get("valueType") in ("ctFunc", "ctProc") or p.get("referencedProperty"):
                continue
            if n in overrides:
                pv[n] = self.encode(p, overrides[n])
            elif p.get("valueType") == "ctObject":
                val = p.get("value", p.get("default"))
                if isinstance(val, dict) and val.get("__type"):
                    pv[n] = self.object_value(val, {})
            elif "value" in p:
                pv[n] = self.encode(p, p["value"])
        for n, v in overrides.items():
            if n not in pv:
                pv[n] = self.encode(None, v)
        return pv

    # ---------------------------------------------------------------- output
    def to_json(self):
        return {"__type": "Instance", "rootDevice": {self.root.local_id: self.root.to_json()}}

    def dumps(self, indent=1):
        return json.dumps(self.to_json(), indent=indent)

    def validate(self, doc=None):
        """Schema errors plus catalog checks (unknown types, unknown IDs, dangling connections)."""
        doc = doc if doc is not None else self.to_json()
        errs = schema_validate(doc, self.schema) if self.schema else []
        signals = set()
        ports = []

        def walk(node, gid, kind):
            if not isinstance(node, dict):
                return
            t = node.get("__type")
            if t == "Signal":
                signals.add(gid)
            if t == "InputPort" and node.get("signalId"):
                ports.append((gid, node["signalId"]))
            if t == "FunctionBlock" and node.get("typeId") and node["typeId"] not in self.catalog["functionBlockTypes"]:
                errs.append(f"{gid}: function block type '{node['typeId']}' is not in the catalog; the loader skips it if no module provides it")
            for k, v in node.items():
                if k in FOLDER_KIND or k == "IO":
                    for cid, c in (v.get("items") or {}).items():
                        walk_folder_item(c, f"{gid}/{k}/{cid}")

        def walk_folder_item(c, gid):
            if c.get("__type") == "IoFolder":
                for cid, x in (c.get("items") or {}).items():
                    walk_folder_item(x, f"{gid}/{cid}")
            else:
                walk(c, gid, c.get("__type"))
        for rid, r in (doc.get("rootDevice") or {}).items():
            walk(r, "/" + rid, "Device")
        for gid, sid in ports:
            if sid not in signals:
                errs.append(f"{gid}: signalId {sid} is not a signal in this setup (the device that owns it must be part of the setup)")
        return errs


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "validate":
        s = Setup.from_files()
        doc = json.load(open(sys.argv[2], encoding="utf-8"))
        errs = s.validate(doc)
        print("\n".join(errs) if errs else "valid")
        return 1 if errs else 0
    print(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
