"""Runtime support for the generated ctypes binding (copied verbatim into the package).

Holds what does not depend on the API description: library loading,
interface ids, reference counting, error translation and the conversion
between coretypes and Python values. Everything API-specific (interfaces,
method slots, enums, factories, errors) is generated from the JSON.
"""

import ctypes
import os
import sys
import uuid
from fractions import Fraction

_registry = {}  # interface name -> generated class
_libs = {}
_lib_names = {}  # library -> file name, filled by the generated module
_bin_dirs = []
_error_classes = {}  # code -> exception class


# ---------------------------------------------------------------- libraries
def set_binary_dir(path):
    """Directory holding the openDAQ libraries (and modules)."""
    path = os.path.abspath(path)
    if path not in _bin_dirs:
        _bin_dirs.insert(0, path)
        if hasattr(os, "add_dll_directory"):
            os.add_dll_directory(path)


def binary_dir():
    if not _bin_dirs and os.environ.get("OPENDAQ_BIN"):
        set_binary_dir(os.environ["OPENDAQ_BIN"])
    return _bin_dirs[0] if _bin_dirs else None


def lib(name):
    if name not in _libs:
        files = _lib_names[name]
        fname = files["windows"] if sys.platform == "win32" else files["macos"] if sys.platform == "darwin" else files["linux"]
        d = binary_dir()
        path = os.path.join(d, fname) if d else fname
        _libs[name] = ctypes.CDLL(path)
    return _libs[name]


# ---------------------------------------------------------------- ids
class GUID(ctypes.Structure):
    _fields_ = [("Data1", ctypes.c_uint32), ("Data2", ctypes.c_uint16), ("Data3", ctypes.c_uint16),
                ("Data4", ctypes.c_ubyte * 8)]

    @classmethod
    def parse(cls, text):
        u = uuid.UUID(text)
        g = cls()
        g.Data1, g.Data2, g.Data3 = u.fields[0], u.fields[1], u.fields[2]
        for i, b in enumerate(u.bytes[8:]):
            g.Data4[i] = b
        return g

    def __str__(self):
        return str(uuid.UUID(fields=(self.Data1, self.Data2, self.Data3, self.Data4[0], self.Data4[1],
                                     int.from_bytes(bytes(self.Data4[2:]), "big")))).upper()


_FUNCTYPE = ctypes.WINFUNCTYPE if sys.platform == "win32" else ctypes.CFUNCTYPE
_proto_cache = {}


def vcall(ptr, slot, restype, argtypes, *args):
    """Call vtable slot `slot` of the interface pointer `ptr`."""
    key = (restype, tuple(argtypes))
    proto = _proto_cache.get(key)
    if proto is None:
        proto = _proto_cache[key] = _FUNCTYPE(restype, ctypes.c_void_p, *argtypes)
    vtbl = ctypes.cast(ptr, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    return proto(vtbl[slot])(ptr, *args)


# ---------------------------------------------------------------- errors
class OpenDaqError(Exception):
    """A failed openDAQ call: `code` is the ErrCode, `name` its OPENDAQ_ERR_* name."""

    code = None
    name = None

    def __init__(self, code, message=None, call=None):
        self.code = code
        self.call = call
        self.message = message
        text = self.name or hex(code)
        if message:
            text += f": {message}"
        super().__init__(f"{text} ({call})" if call else text)


INFO_CODES = {}  # success codes that mean "nothing happened", filled by the generated module


def check(code, call=None):
    code &= 0xFFFFFFFF
    if code & 0x80000000 == 0:
        if code in INFO_CODES:
            import warnings
            warnings.warn(f"{INFO_CODES[code]} from {call}", stacklevel=3)
        return code
    msg = None
    try:
        s = ctypes.c_void_p()
        f = lib("coretypes").daqGetErrorInfoMessage
        f.restype = ctypes.c_uint32
        if f(ctypes.byref(s)) & 0x80000000 == 0 and s.value:
            msg = Object(s.value, _registry["IString"], owned=True).cast_to(_registry["IString"]).getCharPtr()
        lib("coretypes").daqClearErrorInfo()
    except Exception:
        pass
    raise _error_classes.get(code, OpenDaqError)(code, msg, call)


# ---------------------------------------------------------------- objects
class Object:
    """An interface pointer. Owns one reference and releases it when collected."""

    _iid = None
    _name = "IUnknown"

    def __init__(self, ptr, cls=None, owned=True):
        if isinstance(ptr, ctypes.c_void_p):
            ptr = ptr.value
        self._ptr = ptr
        if not owned and ptr:
            vcall(ptr, 1, ctypes.c_int, [])

    def __del__(self):
        p = getattr(self, "_ptr", None)
        if p:
            self._ptr = None
            try:
                vcall(p, 2, ctypes.c_int, [])
            except Exception:
                pass

    @property
    def ptr(self):
        return self._ptr

    def query_interface(self, cls):
        """queryInterface: the object as `cls`, or None if it does not implement it.

        The helpers use snake_case names so generated interface methods (camelCase) never shadow them.
        """
        if isinstance(cls, str):
            cls = _registry[cls]
        out = ctypes.c_void_p()
        code = vcall(self._ptr, 0, ctypes.c_uint32, [ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p)],
                     ctypes.byref(GUID.parse(cls._iid)), ctypes.byref(out)) & 0xFFFFFFFF
        if code & 0x80000000:
            return None
        return cls(out.value, owned=True)

    def cast_to(self, cls):
        r = self.query_interface(cls)
        if r is None:
            raise TypeError(f"object does not implement {cls if isinstance(cls, str) else cls._name}")
        return r

    def supports_interface(self, cls):
        return self.query_interface(cls) is not None

    def __eq__(self, other):
        if not isinstance(other, Object):
            return NotImplemented
        a, b = self.query_interface("IBaseObject"), other.query_interface("IBaseObject")
        return a is not None and b is not None and a._ptr == b._ptr

    def __hash__(self):
        b = self.query_interface("IBaseObject")
        return hash(b._ptr if b else self._ptr)

    def __repr__(self):
        try:
            s = self.query_interface("IBaseObject").toString()
        except Exception:
            s = hex(self._ptr or 0)
        return f"<{self._name} {s}>"


def wrap(ptr, iface="IBaseObject", convert=True, element=None, key=None, value=None):
    """Turn an owned interface pointer into a Python value or wrapper object."""
    if not ptr:
        return None
    cls = _registry.get(iface) or _registry["IBaseObject"]
    obj = cls(ptr, owned=True)
    if not convert:
        return obj
    return to_python(obj, iface, element, key, value)


CT = {"ctBool": 0, "ctInt": 1, "ctFloat": 2, "ctString": 3, "ctList": 4, "ctDict": 5, "ctRatio": 6, "ctProc": 7,
      "ctObject": 8, "ctBinaryData": 9, "ctFunc": 10, "ctComplexNumber": 11, "ctStruct": 12, "ctEnumeration": 13}


def to_python(obj, iface="IBaseObject", element=None, key=None, value=None):
    """Coretypes become Python values; anything else stays a wrapper."""
    if obj is None:
        return None
    if iface == "IString":
        return obj.getCharPtr()
    if iface in ("IInteger", "IFloat", "IBoolean"):
        v = obj.getValue()
        return bool(v) if iface == "IBoolean" else v
    if iface == "IList":
        return [_item(x, element) for x in _raw_items(obj)]
    if iface == "IDict":
        keys = obj.getKeyList(convert=False)
        vals = obj.getValueList(convert=False)
        kl = [_item(x, key) for x in _raw_items(keys)]
        vl = [_item(x, value) for x in _raw_items(vals)]
        return dict(zip(kl, vl))
    if iface == "IRatio":
        return Fraction(obj.getNumerator(), obj.getDenominator())
    if iface == "IComplexNumber":
        return complex(obj.getReal(), obj.getImaginary())
    if iface in ("IBaseObject", "INumber"):
        ct = obj.query_interface("ICoreType")
        if ct is None:
            return obj
        t = ct.getCoreType()
        target = {0: "IBoolean", 1: "IInteger", 2: "IFloat", 3: "IString", 4: "IList", 5: "IDict", 6: "IRatio",
                  11: "IComplexNumber"}.get(int(t))
        if target:
            return to_python(obj.cast_to(target), target, element, key, value)
        other = {7: "IProcedure", 9: "IBinaryData", 10: "IFunction", 12: "IStruct", 13: "IEnumeration"}.get(int(t))
        if other and other in _registry:
            return obj.cast_to(other)
        return obj
    if element is not None and iface not in _registry:
        return obj
    return obj


def _item(raw, element):
    """A container element, typed by the RTGen elementType when there is one."""
    if raw is None:
        return None
    if element and element != "IBaseObject" and element in _registry:
        typed = raw.query_interface(element)
        if typed is None:
            return to_python(raw, "IBaseObject")
        return to_python(typed, element)
    return to_python(raw, "IBaseObject")


def _raw_items(lst):
    n = lst.getCount()
    return [lst.getItemAt(i, convert=False) for i in range(n)]


# ---------------------------------------------------------------- python -> openDAQ
def from_python(v, iface="IBaseObject"):
    """An owned wrapper for passing `v` as an `iface` argument (None -> NULL)."""
    if v is None:
        return None
    if isinstance(v, Object):
        if iface not in ("IBaseObject",) and iface in _registry and not isinstance(v, _registry[iface]):
            q = v.query_interface(iface)
            if q is not None:
                return q
        return v
    F = sys.modules[__package__]
    if isinstance(v, bool):
        return F.Boolean(v)
    if isinstance(v, int):
        return F.Integer(v)
    if isinstance(v, float):
        return F.Float(v)
    if isinstance(v, str):
        return F.String(v)
    if isinstance(v, Fraction):
        return F.Ratio(v.numerator, v.denominator)
    if isinstance(v, complex):
        return F.ComplexNumber(v.real, v.imag)
    if isinstance(v, (list, tuple)):
        lst = F.List()
        for x in v:
            lst.pushBack(x)
        return lst
    if isinstance(v, dict):
        d = F.Dict()
        for k, x in v.items():
            d.set(k, x)
        return d
    raise TypeError(f"cannot pass {type(v).__name__} as {iface}")


def arg_ptr(v, iface="IBaseObject", keep=None):
    """Raw pointer for an in-argument; `keep` collects temporaries until the call returns."""
    o = from_python(v, iface)
    if o is None:
        return None
    if keep is not None:
        keep.append(o)
    if iface in _registry and iface not in ("IBaseObject",) and not isinstance(o, _registry[iface]):
        o = o.cast_to(iface)
        if keep is not None:
            keep.append(o)
    return o._ptr


def cstr(v):
    return v.encode("utf-8") if isinstance(v, str) else v


def pystr(v):
    return v.decode("utf-8", "replace") if isinstance(v, (bytes, bytearray)) else v
