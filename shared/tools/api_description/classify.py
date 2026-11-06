"""Propose an audience for every interface, method, factory, enum and
implementation member, with the reason, from rules plus usage evidence.

    python classify.py            -> config/classification.json

The proposal is applied to the headers by annotate.py. Once the headers
carry tags, they are the source of truth: classify.py keeps every
existing tag (reason "tagged in header") and only proposes for untagged
entries. Decisions made on the review page come back through
annotate.py --apply <exported file>.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

CORETYPES = {
    "IBaseObject", "IBoolean", "IInteger", "IFloat", "INumber", "IString", "IRatio", "IComplexNumber", "IBinaryData",
    "IList", "IDict", "IIterable", "IIterator", "IFunction", "IProcedure", "IEvent", "IEventHandler",
}

# Interfaces decided by judgment; everything else follows the rules in interface_audience().
INTERFACE_DECISIONS = {
    # core plumbing nobody outside the core should touch
    "IUnknown": ("application", "COM root; every binding needs queryInterface/addRef/releaseRef."),
    "ITypeManagerPrivate": ("core", "Type-manager bookkeeping for the core."),
    "IContextInternal": ("core", "Context bookkeeping (module manager hand-over) used only by the instance."),
    "IComponentUpdateContext": ("core", "Carries state through loadConfiguration inside the core."),
    "IComponentHolder": ("core", "Serialization helper for component references inside the core."),
    "IUpdatable": ("core", "Core load/update protocol; modules hook in through updateObject overrides, not this interface."),
    "IOwnable": ("core", "Core sets the owner of property objects; neither applications nor modules call it."),
    "IUserLock": ("core", "Device lock state holder inside the core; applications lock through IDevice."),
    "ISignalEvents": ("core", "Callbacks the core invokes on signals when listeners connect."),
    "ISignalPrivate": ("core", "Signal bookkeeping for the core (streaming sources, serialization)."),
    "IInputPortPrivate": ("core", "Input port bookkeeping for the core."),
    "IWrappedDataPacket": ("core", "Packet wrapping used by the core readers."),
    "ITaskInternal": ("core", "Scheduler internals."),
    "IAddressInfoPrivate": ("core", "Address info bookkeeping for the core."),
    "ISynchronizationPrivate": ("core", "Synchronization bookkeeping for the core."),
    "ILoggerSinkBasePrivate": ("core", "Logger sink internals."),
    "ILoggerThreadPoolPrivate": ("core", "Logger thread pool internals."),
    "IPropertyObjectClassInternal": ("core", "Class-manager bookkeeping."),
    "IPermissionManagerInternal": ("core", "Permission inheritance bookkeeping."),
    "IUserInternal": ("core", "Password hash access for the core authentication provider."),
    "IPropertyObjectCore": ("core", "Lock-free property access used by the core while it holds the object lock."),
    "IDataRuleCalcPrivate": ("core", "Rule evaluation used by the core packet code."),
    "IScalingCalcPrivate": ("core", "Scaling evaluation used by the core packet code."),
    "IRulePrivate": ("core", "Rule internals."),
    "IMockChannel": ("core", "Test mock."),
    "IMockDefaultDevice": ("core", "Test mock."),
    "IMockStreaming": ("core", "Test mock."),
    "ILastMessageLoggerSinkPrivate": ("core", "Test helper sink that remembers the last message."),
    # interfaces modules implement or call when they build components
    "IRemovable": ("module", "remove() is called by the core when a component is removed; modules forward it to children."),
    "IErrorGuard": ("module", "Error-info scoping for code that produces error chains (modules and core)."),
    "IAllocator": ("module", "Custom packet memory for modules that own their buffers."),
    "IDeleter": ("module", "Custom packet memory release for modules that own their buffers."),
    "IPacketDestructCallback": ("module", "Notification when an externally-owned packet buffer is released."),
    "IReusableDataPacket": ("module", "Packet reuse for high-rate module producers."),
    "IInputPortConfig": ("module", "Function blocks configure their own input ports; applications use IInputPort."),
    "IInputPortNotifications": ("module", "Implemented by function blocks to get connect/packet notifications."),
    "IConnectionInternal": ("module", "Used by streaming modules to enqueue packets on connections."),
    "IDiscoveryServer": ("module", "Server modules register their services with the discovery server."),
    "IServerCapabilityConfig": ("module", "Client modules fill in the server capabilities they find."),
    "IAddressInfoBuilder": ("module", "Client modules describe the addresses they reached a device on."),
    "ILogFileInfoBuilder": ("module", "Device modules describe their log files."),
    "IIoFolderConfig": ("module", "Device modules build their IO folders."),
    "IMirroredDeviceConfig": ("module", "Client modules attach streaming sources to mirrored devices."),
    "IMirroredInputPortConfig": ("module", "Client modules configure mirrored input ports."),
    "IMirroredSignalPrivate": ("module", "Streaming modules deliver data to mirrored signals."),
    "IMirroredInputPortPrivate": ("module", "Streaming modules send data from mirrored input ports."),
    "IStreamingPrivate": ("module", "Client modules register signals with their streaming objects."),
    "ISubscriptionEventArgs": ("module", "Streaming modules receive subscribe/unsubscribe acknowledgements."),
    "IPacketBuffer": ("module", "Pre-allocated packet memory for module producers."),
    "IPacketBufferBuilder": ("module", "Pre-allocated packet memory for module producers."),
    "ILoggerThreadPool": ("module", "Shared logger threads; modules creating their own loggers use it."),
    "IReaderConfig": ("module", "Reader internals that readers built in modules (recorders) reuse."),
    "IModuleManagerUtils": ("module", "Modules create nested devices and function blocks through it."),
    "IPropertyObjectProtected": ("module", "Owners (device and function block modules) write read-only properties through it."),
    "ILockGuard": ("module", "Lock guards returned by the property-object locking helpers modules call."),
    "IMutex": ("module", "Mutex objects shared between a component and its module."),
    "IComponentDeserializeContext": ("module", "Modules that deserialize their own components read it."),
    "IDeserializeComponent": ("module", "Modules implement custom deserialization of their components."),
    # interfaces applications use even though our application samples do not show them
    "IComponentStatusContainer": ("application", "Applications read component statuses (the Python GUI shows them)."),
    "IDeviceNetworkConfig": ("application", "Applications change device IP configuration (examples/ip_modification)."),
    "IMirroredDevice": ("application", "Applications list a client device's streaming sources."),
    "IVersionInfo": ("application", "Applications read module and SDK versions."),
    "ISyncInterface": ("application", "Applications configure synchronization interfaces (spec 0001)."),
    "ISerializer": ("application", "Applications serialize objects (saveConfiguration, custom JSON)."),
    "ISerializedObject": ("application", "Applications deserialize objects."),
    "ISerializedList": ("application", "Applications deserialize objects."),
    "IDeserializer": ("application", "Applications deserialize objects."),
    "IDeviceDomain": ("application", "Applications read the device domain (origin, tick resolution)."),
    "IDimension": ("application", "Part of data descriptors applications read."),
    "IRange": ("application", "Part of data descriptors applications read."),
    "IBlockReader": ("application", "Reader API."),
    "IPacketReader": ("application", "Reader API."),
    "ILogFileInfo": ("application", "Applications download device log files."),
    "ILoggerComponent": ("application", "Applications and modules log through logger components."),
    "IModuleInfo": ("application", "Applications list loaded modules and their versions."),
    "IWeakRef": ("application", "Weak references are part of the smart-pointer API."),
}

PRIVATE_NAME = re.compile(r"(Private|Internal)$")


def interface_audience(i, ev):
    name = i["name"]
    used_m = "module" in ev.get(name, {})
    used_a = "application" in ev.get(name, {})
    if name in CORETYPES:
        return "application", ["coretype"], "Coretype: openDAQ wrapper of a standard programming type; bindings map it to a native type."
    if name in INTERFACE_DECISIONS:
        a, why = INTERFACE_DECISIONS[name]
        return a, [], why
    hdr = i.get("header", "")
    if PRIVATE_NAME.search(name) or hdr.endswith(("_private.h", "_internal.h")):
        if used_a and used_m:
            return "module", [], "Private/internal name; applications in the repo use it too, but only as plumbing [CHECK]."
        if used_m:
            return "module", [], "Private/internal name, used by modules."
        return "core", [], "Private/internal name, not used by applications or modules."
    if i.get("group") in ("scheduler",) or True:
        if used_a:
            return "application", [], "Used by applications."
        if used_m:
            return "application", [], "Used by modules; nothing restricts it to modules."
        return "application", [], "Default for interfaces that are not private/internal."


# Methods on otherwise public interfaces that only the core or modules should call.
METHOD_DECISIONS = {
    "IComponent.getLockedAttributes": ("application", "Applications check which attributes are locked."),
    "IPropertyObject.getPermissionManager": ("application", "Applications inspect permissions."),
    "IPropertyObject.beginUpdate": ("application", "Batch updates."),
    "IFolderConfig.addItem": ("module", "Only the owning module adds items to a folder it built."),
    "IFolderConfig.removeItem": ("module", "Only the owning module removes items from a folder it built."),
    "IFolderConfig.removeItemWithLocalId": ("module", "Only the owning module removes items from a folder it built."),
    "IFolderConfig.clear": ("module", "Only the owning module clears a folder it built."),
    "ISignalConfig.sendPacket": ("application", "Applications send packets on client-to-device signals."),
    "ISignalConfig.setDomainSignal": ("application", "Applications link domain signals they create."),
    "IDevice.getSyncComponent": ("application", "Applications read synchronization state."),
    "IContext.getOnCoreEvent": ("application", "Applications subscribe to core events."),
    "IContext.moveModuleManager": ("core", "Hand-over of the module manager inside the instance."),
    "IContext.getModuleManager": ("application", "Applications reach the module manager through the context."),
    "IServer.stop": ("core", "The instance stops servers when it removes them; applications call IInstance::removeServer."),
    "IStreaming.addSignals": ("application", "Applications add signals to a streaming source."),
    "IModule.getAvailableDevices": ("application", "Applications discover devices per module."),
    "IModuleManager.loadModules": ("core", "Called once by the instance on start."),
    "IModuleManager.addModule": ("application", "Applications add modules they load themselves."),
    "IModuleManager.completeServerCapability": ("core", "Called by the core while it builds device info."),
    "IModuleManager.createStreaming": ("module", "Client modules create streaming objects for a connection string."),
    "IModuleManager.createDevice": ("module", "Called by the core and by modules that build nested devices."),
    "IModuleManager.createFunctionBlock": ("module", "Called by the core and by modules that build nested function blocks."),
    "IModuleManager.createServer": ("module", "Called by the core when the instance adds a server."),
    "IModuleManager.getAvailableDevices": ("application", "Applications run discovery."),
    "IModuleManager.getAvailableDeviceTypes": ("application", "Applications list device types."),
    "IModuleManager.getAvailableFunctionBlockTypes": ("application", "Applications list function block types."),
    "IModuleManager.getAvailableServerTypes": ("application", "Applications list server types."),
    "IModuleManager.getModules": ("application", "Applications list modules."),
    "IModuleManager.changeIpConfig": ("application", "Applications change device IP configuration."),
    "IModuleManager.requestIpConfig": ("application", "Applications read device IP configuration."),
    "IComponentPrivate.triggerComponentCoreEvent": ("core", "Raises core events on behalf of the component; spec 0010 example."),
    "IComponentPrivate.updateComponentStatusInfo": ("core", "Status bookkeeping inside the core."),
    "IDevicePrivate.setAsRoot": ("core", "Only the instance marks its root device."),
    "IDevicePrivate.getDeviceConfig": ("core", "Configuration-protocol plumbing."),
    "IDevicePrivate.setDeviceConfig": ("core", "Configuration-protocol plumbing."),
    "IDevicePrivate.setRemoteUpdating": ("core", "Configuration-protocol plumbing."),
    "IDevicePrivate.isRemoteUpdating": ("core", "Configuration-protocol plumbing."),
    "IPropertyObjectInternal.setPath": ("core", "The core sets paths when it nests property objects."),
    "IPropertyObjectInternal.setCoreEventTrigger": ("core", "The core installs the core-event trigger."),
    "IPropertyObjectInternal.enableCoreEventTrigger": ("core", "Core-event plumbing."),
    "IPropertyObjectInternal.disableCoreEventTrigger": ("core", "Core-event plumbing."),
    "IPropertyObjectInternal.getCoreEventTrigger": ("core", "Core-event plumbing."),
    "IPropertyObjectInternal.clone": ("module", "Modules clone default configuration objects."),
    "IPropertyObjectInternal.setLockingStrategy": ("core", "Locking plumbing set by the component on its property object."),
}

# Implementation classes modules derive from; their non-private members get module/core tags.
INHERITABLE_EXTRA = ["ImplementationOf", "ImplementationOfWeak", "FolderImpl", "IoFolderImpl", "GenericInputPortImpl",
                     "MirroredDeviceBase", "MirroredInputPortBase", "MirroredSignalBase", "SignalBase",
                     "GenericSyncComponentImpl", "Module", "ModuleAuthenticator"]
PLUMBING_LIBS = ("coretypes",)  # members of coretypes plumbing (ImplementationOf, ObjInstance) stay core


def factory_audience(f, intf_aud, ev):
    name = f["name"]
    ret = f.get("returns")
    used = ev.get(name, {})
    ptypes = [p.get("base") for p in f.get("params", [])]
    if not f.get("exported", True) and f.get("createFunction"):
        return "core", "Declared INTERNAL_FACTORY: not exported from the library, so only the core can call it."
    if name.startswith("Mock") or "Mock" in (ret or ""):
        return "core", "Test mock."
    if name.startswith("CoreEventArgs") or name.startswith("CoreEvent"):
        if "module" in used:
            return "module", "Core event arguments; modules raise their own core events."
        return "core", "Core event arguments are created by the core when it raises core events."
    base_aud = intf_aud.get(ret, "application")
    if "IContext" in ptypes and ("IComponent" in ptypes or "IFolderConfig" in ptypes or "IDevice" in ptypes):
        if "application" in used:
            return "application", "Component constructor (context + parent), used by applications too."
        return "module", "Component constructor taking a context and a parent: modules build components with it."
    if base_aud != "application":
        return base_aud, f"Follows the returned interface {ret} ({base_aud})."
    if ret is None:
        return "application", "Returns a C++ helper type, not an interface; part of the C++ application API."
    if "application" in used:
        return "application", "Used by applications."
    return "application", f"Follows the returned interface {ret} (public)."


def member_audience(cls, item, kind, evidence, chain_libs):
    """kind: member or function. Returns (audience, labels, reason)."""
    name = item["name"]
    ev = evidence.get(cls, {}).get(name)
    hows = set(ev["how"]) if ev else set()
    repos = ", ".join(ev["repos"]) if ev else ""
    if kind == "function":
        if "override" in hows:
            return "module", ["override"], f"Overridden by modules ({repos})."
        if item.get("virtual") and re.match(r"^on[A-Z]", name):
            if hows:
                return "module", ["override"], f"Hook (on*), called by modules ({repos})."
            return "module", ["override"], "Hook (on*) designed for module overrides; no module overrides it yet."
        if hows & {"call", "read"}:
            return "module", [], f"Called by modules ({repos})."
        if item.get("virtual") and name in ("removed", "activeChanged", "visibleChanged", "updateObject", "serializeCustomObjectValues",
                                             "onUpdatableUpdateEnd", "endApplyProperties", "getDiscoveryConfig",
                                             "getDiscoveryConfigs", "allowAddDevicesFromModules",
                                             "allowAddFunctionBlocksFromModules", "clearComponentStatusInternal"):
            return "module", ["override"], "Virtual hook in the component lifecycle that modules may override."
        return "core", [], "Not used by any scanned module."
    # member variable
    if "write" in hows:
        return "module", [], f"Written by modules ({repos})."
    if hows:
        return "module", ["readOnly"], f"Read by modules ({repos})."
    return "core", [], "Not used by any scanned module."


PREFER_RULES = "--prefer-rules" in sys.argv


def settle(tag_aud, tag_labels, rule):
    """Header tag wins (unless --prefer-rules); the reason states the rule and whether the tag agrees."""
    r_aud, r_labels, r_why = rule
    if not tag_aud or PREFER_RULES:
        return r_aud, r_labels, r_why
    if tag_aud == r_aud and sorted(tag_labels or []) == sorted(r_labels or []):
        return tag_aud, tag_labels or [], r_why
    rule_txt = ", ".join([str(r_aud)] + list(r_labels or []))
    return tag_aud, tag_labels or [], f"Tagged differently in the header; the rules propose {rule_txt}: {r_why}"


def main():
    api = json.load(open(os.path.join(HERE, "output", "opendaq_api.json"), encoding="utf-8"))
    usage = json.load(open(os.path.join(HERE, "output", "usage.json"), encoding="utf-8"))
    entries = []

    def add(key, kind, audience, labels, reason, where, tagged=None, extra=None):
        e = {"key": key, "kind": kind, "audience": audience, "labels": labels or [], "reason": reason, "where": where}
        if tagged:
            e["tagged"] = tagged
        if extra:
            e.update(extra)
        entries.append(e)

    iev = usage["interfaces"]
    mev = usage["methods"]
    intf_aud = {}
    for i in api["interfaces"]:
        a, labels, why = settle(i.get("audience"), i.get("labels", []), interface_audience(i, iev))
        intf_aud[i["name"]] = a
        evid = iev.get(i["name"], {})
        add(f"interface:{i['name']}", "interface", a, labels, why, f"{i['file']}:{i['line']}",
            tagged=bool(i.get("audience")),
            extra={"library": i["library"], "group": i["group"], "usedBy": evid,
                   "methods": len(i.get("methods", [])), "brief": (i.get("doc") or {}).get("brief")})
        for m in i.get("methods", []):
            k = f"{i['name']}.{m['name']}"
            rule = (METHOD_DECISIONS[k][0], [], METHOD_DECISIONS[k][1]) if k in METHOD_DECISIONS else (None, [], "Follows the interface.")
            ma, ml, mwhy = settle(m.get("audience"), m.get("labels", []), rule)
            e = mev.get(m["name"], {})
            add(f"method:{k}", "method", ma, ml, mwhy, f"{i['file']}:{m['line']}", tagged=bool(m.get("audience")),
                extra={"interface": i["name"], "interfaceAudience": a, "usedBy": e,
                       "brief": (m.get("doc") or {}).get("brief")})
    fev = usage["factories"]
    for f in api["factories"]:
        if not f.get("createFunction"):
            continue
        ra, rwhy = factory_audience(f, intf_aud, fev)
        a, _, why = settle(f.get("audience"), [], (ra, [], rwhy))
        add(f"factory:{f['createFunction']}", "factory", a, [], why, f"{f['file']}:{f['line']}", tagged=bool(f.get("audience")),
            extra={"name": f["name"], "returns": f.get("returns"), "exported": f.get("exported", False),
                   "usedBy": fev.get(f["name"], {}), "implementation": f.get("implementation")})
    for f in api.get("cppFactories", []):
        key = f"cppfactory:{f['header']}#{f['name']}/{len(f.get('params', []))}"
        if True:
            full = f["returns"].split("::")[-1]
            ret = re.sub(r"<.*", "", full)
            intf = None
            m_obj = re.match(r"^ObjectPtr<(I\w+)>$", full)
            if m_obj:
                intf = m_obj.group(1)
            for i in api["interfaces"]:
                if ret in (i["smartPtr"], (i.get("templated") or {}).get("defaultAliasName"), i["name"][1:] + "Ptr"):
                    intf = i["name"]
            for sp in api.get("smartPointers", []):
                if ret == sp["name"] and sp.get("interface"):
                    intf = intf or sp["interface"]
            if intf is None and ret in ("ListPtr", "ListObjectPtr"):
                intf = "IList"
            if intf is None and ret in ("DictPtr", "DictObjectPtr"):
                intf = "IDict"
            pseudo = {"name": f["name"], "returns": intf, "exported": True,
                      "params": [{"base": re.sub(r"Ptr$", "", p.get("base", "")).join(["I", ""]) if p.get("base", "").endswith("Ptr") else p.get("base")}
                                 for p in f.get("params", [])]}
            ra, rwhy = factory_audience(pseudo, intf_aud, fev)
            a, _, why = settle(f.get("audience"), [], (ra, [], rwhy))
        add(key, "cppfactory", a, [], why, f"{f['file']}:{f['line']}", tagged=bool(f.get("audience")),
            extra={"name": f["name"], "returns": f["returns"], "usedBy": fev.get(f["name"], {})})
    for e in api["enums"]:
        if not e.get("header") or e.get("outer"):
            continue
        a, _, why = settle(e.get("audience"), [], ("application", [], "Enums in installed headers are part of the application API."))
        add(f"enum:{e['name']}", "enum", a, [], why,
            f"{e['file']}:{e['line']}", tagged=bool(e.get("audience")))
    # smart pointer classes: the C++ face of their interface
    for sp in api.get("smartPointers", []):
        ia = intf_aud.get(sp.get("interface"), "application") if sp.get("interface") else "application"
        a, _, why = settle(sp.get("audience"), [], (ia, [], f"Smart pointer of {sp.get('interface') or 'any interface'}; follows its audience."))
        add(f"smartptr:{sp['name']}", "smartptr", a, [], why, f"{sp['file']}:{sp['line']}", tagged=bool(sp.get("audience")),
            extra={"interface": sp.get("interface")})
    # implementation classes
    derived = usage.get("derived", {})
    inheritable = set(derived) | set(INHERITABLE_EXTRA)
    impls = {x["name"]: x for x in api["implementations"]}
    for n in list(inheritable):
        if n in impls:
            inheritable.update(impls[n].get("baseChain", []))
    evidence = usage.get("members", {})
    for name in sorted(impls):
        rec = impls[name]
        if not rec.get("installed"):
            continue
        if name in inheritable:
            rule = ("module", [], f"Modules derive from it ({', '.join(sorted({r for r, _ in derived[name]}))})." if name in derived
                    else "Base of classes modules derive from.")
        else:
            rule = ("core", [], "No module derives from it.")
        a, _, why = settle(rec.get("audience"), rec.get("labels") or [], rule)
        add(f"class:{name}", "class", a, rec.get("labels") or [], why, f"{rec['file']}:{rec['line']}", tagged=bool(rec.get("audience")),
            extra={"library": rec.get("library"), "derivedBy": sorted({f"{r}:{c}" for r, c in derived.get(name, [])})})
        if name not in inheritable:
            continue
        plumbing = rec.get("library") in PLUMBING_LIBS
        seen = set()
        for kind, items in (("member", rec.get("members", [])), ("function", rec.get("functions", []))):
            for it in items:
                if it["access"] == "private" or it.get("implements") or it["name"] in (name, "~" + name):
                    continue
                if it["name"].startswith("operator") or it.get("static") and it["name"] in ("SerializeId", "Deserialize"):
                    continue
                key = f"{kind}:{name}.{it['name']}"
                if key in seen:
                    continue
                seen.add(key)
                rule = member_audience(name, it, kind, evidence, None)
                if plumbing and rule[0] == "core":
                    rule = (rule[0], rule[1], "Object-model plumbing.")
                ma, ml, mwhy = settle(it.get("audience"), it.get("labels") or [], rule)
                evi = evidence.get(name, {}).get(it["name"])
                add(key, kind, ma, ml, mwhy, f"{rec['file']}:{it['line']}", tagged=bool(it.get("audience")),
                    extra={"class": name, "access": it["access"], "virtual": it.get("virtual", False),
                           "type": it.get("type") or it.get("returns"), "evidence": evi})
    out = {"comment": "Proposed audiences. Edit on the review page, export, then annotate.py --apply.",
           "commit": api.get("source", {}).get("commit"), "entries": entries}
    path = os.path.join(HERE, "config", "classification.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    from collections import Counter
    c = Counter((e["kind"], e["audience"]) for e in entries)
    print(path, len(entries), "entries")
    for k in sorted(c, key=lambda x: (x[0], str(x[1]))):
        print(" ", k, c[k])


if __name__ == "__main__":
    main()
