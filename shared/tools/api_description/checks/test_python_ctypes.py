"""Exercise the generated ctypes binding against real openDAQ libraries.

    OPENDAQ_BIN=<build>/bin python checks/test_python_ctypes.py

Covers interface ids and vtable slots (every call goes through them),
coretype conversion both ways, typed errors, callbacks, and a real
instance with the reference device and function block modules.
"""

import os
import sys
import unittest
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "output", "bindings", "python"))
import opendaq_ct as daq  # noqa: E402

BIN = os.environ.get("OPENDAQ_BIN")


@unittest.skipUnless(BIN, "OPENDAQ_BIN not set")
class CoreTypes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        daq.set_binary_dir(BIN)

    def test_string(self):
        s = daq.String("héllo")
        self.assertEqual(s.getCharPtr(), "héllo")
        self.assertEqual(s.getLength(), 6)  # bytes, not characters
        self.assertTrue(s.supports(daq.IConvertible))
        self.assertIsNone(s.query(daq.IList))

    def test_values_round_trip(self):
        for v in [True, 42, -7, 2.5, "x", [1, "a", 2.5], {"a": 1, "b": [True, False]}, Fraction(1, 3), 1 + 2j]:
            self.assertEqual(daq._rt.to_python(daq._rt.from_python(v)), v, repr(v))

    def test_list_methods(self):
        lst = daq.List()
        lst.pushBack(1)
        lst.pushBack("two")
        self.assertEqual(lst.getCount(), 2)
        self.assertEqual(lst.getItemAt(1), "two")
        self.assertEqual(daq._rt.to_python(lst, "IList"), [1, "two"])

    def test_typed_error(self):
        lst = daq.List()
        with self.assertRaises(daq.OpenDaqError) as cm:
            lst.getItemAt(5)
        self.assertEqual(cm.exception.name, "OPENDAQ_ERR_OUTOFRANGE")

    def test_function_callback(self):
        f = daq.Function(lambda x: x * 2)
        self.assertEqual(f.call(21), 42)

    def test_enum(self):
        self.assertEqual(daq.String("a").cast(daq.ICoreType).getCoreType(), daq.CoreType.ctString)


@unittest.skipUnless(BIN, "OPENDAQ_BIN not set")
class Instance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        daq.set_binary_dir(BIN)
        b = daq.InstanceBuilder()
        b.setModulePath(BIN)
        b.setDefaultRootDeviceLocalId("openDAQDevice")
        cls.inst = daq.InstanceFromBuilder(b)

    def test_ref_device_and_fb(self):
        inst = self.inst
        types = inst.getAvailableFunctionBlockTypes()
        self.assertIn("RefFBModuleStatistics", types)
        dev = inst.addDevice("daqref://device0")
        self.assertIsInstance(dev, daq.IDevice)
        self.assertTrue(dev.name)
        chans = dev.getChannels()
        self.assertGreater(len(chans), 0)
        self.assertIsInstance(chans[0], daq.IChannel)
        fb = inst.addFunctionBlock("RefFBModuleStatistics")
        fb.name = "Stats"
        self.assertEqual(fb.name, "Stats")
        sig = chans[0].getSignals()[0]
        fb.getInputPorts()[0].connect(sig)
        self.assertEqual(fb.getInputPorts()[0].getSignal().getGlobalId(), sig.getGlobalId())
        names = fb.getVisibleProperties()
        self.assertTrue(all(isinstance(p, daq.IProperty) for p in names))
        setup = inst.saveConfiguration()
        self.assertIn('"__type": "Instance"', setup.replace('":"', '": "').replace('" :', '":'))


if __name__ == "__main__":
    unittest.main(verbosity=2)
