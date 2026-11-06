"""Build a setup offline, load it into a real instance, check every value.

    OPENDAQ_BIN=<build>/bin python checks/test_setup.py

Needs output/setup/catalog.json (setup/catalog.py) and the generated
ctypes binding. The setup is written without any device attached; the
instance that loads it starts empty, so everything it ends up with comes
from the file.
"""

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TOOL, "output", "bindings", "python"))
sys.path.insert(0, os.path.join(TOOL, "setup"))
import opendaq_ct as daq  # noqa: E402
from setupkit import Setup, SetupError  # noqa: E402
from apidesc.schema_lite import validate  # noqa: E402

BIN = os.environ.get("OPENDAQ_BIN")


def build_setup():
    s = Setup.from_files()
    root = s.root
    root.name = "Test bench"
    dev = root.add_device("daqref://device0", name="Reference 0")
    ch0 = dev.channel("RefCh0")
    ch0.set("Frequency", 15.0).set("Amplitude", 2.5)
    ch0.description = "Sine source"
    ch1 = dev.channel("RefCh1")
    ch1.active = False
    dev.set("NumberOfChannels", 2)
    stats = root.add_function_block("RefFBModuleStatistics", name="Stats")
    stats.set("BlockSize", 25).set("DomainSignalType", "Explicit")
    stats.input_port("Input").connect(ch0.signal("AI0"))
    stats.signal("rms").public = False
    stats.signal("avg").name = "Average"
    scaling = dev.add_function_block("RefFBModuleScaling", name="Scale on device")
    scaling.input_port("Input").connect(ch0.signal("AI0"))
    scaling.set("scale", 0.5)
    trig = root.add_function_block("RefFBModuleTrigger")
    trig.description = "Edge trigger"
    return s


class Offline(unittest.TestCase):
    def test_schema_and_catalog_checks(self):
        s = build_setup()
        self.assertEqual(s.validate(), [])

    def test_rejects_bad_values(self):
        s = Setup.from_files()
        fb = s.root.add_function_block("RefFBModuleStatistics")
        with self.assertRaises(SetupError):
            fb.set("Overlap", 150)  # max 99
        with self.assertRaises(SetupError):
            fb.set("DomainSignalType", "Sideways")
        with self.assertRaises(SetupError):
            fb.set("NoSuchProperty", 1)
        with self.assertRaises(SetupError):
            s.root.add_function_block("NoSuchType")
        with self.assertRaises(SetupError):
            fb.visible = False  # function blocks lock Visible

    def test_validator_finds_dangling_connection(self):
        s = Setup.from_files()
        fb = s.root.add_function_block("RefFBModuleStatistics")
        fb.input_port("Input").connect("/openDAQDevice/Dev/RefDev9/IO/AI/RefCh0/Sig/AI0")
        self.assertTrue(any("not a signal in this setup" in e for e in s.validate()))


@unittest.skipUnless(BIN, "OPENDAQ_BIN not set")
class LoadIntoInstance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        daq.set_binary_dir(BIN)
        b = daq.InstanceBuilder()
        b.setModulePath(BIN)
        b.setDefaultRootDeviceLocalId("openDAQDevice")
        cls.inst = daq.InstanceFromBuilder(b)
        cls.setup = build_setup()
        cls.text = cls.setup.dumps()
        out = os.path.join(TOOL, "output", "setup", "test_setup.json")
        with open(out, "w", encoding="utf-8") as f:
            f.write(cls.text)
        cls.inst.loadConfiguration(cls.text)

    def find(self, gid, intf=None):
        c = self.inst.findComponent(gid.split("/", 2)[2])
        return c.cast_to(intf) if intf else c

    def test_root(self):
        self.assertEqual(self.inst.getRootDevice().name, "Test bench")

    def test_device_and_channels(self):
        dev = self.inst.getDevices()[0]
        self.assertEqual(dev.getLocalId(), "RefDev0")
        self.assertEqual(dev.name, "Reference 0")
        ch0 = self.find("/openDAQDevice/Dev/RefDev0/IO/AI/RefCh0")
        self.assertEqual(ch0.getPropertyValue("Frequency"), 15.0)
        self.assertEqual(ch0.getPropertyValue("Amplitude"), 2.5)
        self.assertEqual(ch0.description, "Sine source")
        self.assertFalse(self.find("/openDAQDevice/Dev/RefDev0/IO/AI/RefCh1").active)

    def test_function_blocks(self):
        fbs = {fb.getLocalId(): fb for fb in self.inst.getFunctionBlocks()}
        self.assertEqual(set(fbs), {"RefFBModuleStatistics_1", "RefFBModuleTrigger_1"})
        stats = fbs["RefFBModuleStatistics_1"]
        self.assertEqual(stats.name, "Stats")
        self.assertEqual(stats.getPropertyValue("BlockSize"), 25)
        self.assertEqual(stats.getPropertyValue("DomainSignalType"), 1)
        trig = fbs["RefFBModuleTrigger_1"]
        self.assertEqual(trig.description, "Edge trigger")

    def test_signals(self):
        stats = self.find("/openDAQDevice/FB/RefFBModuleStatistics_1", daq.IFunctionBlock)
        sigs = {s.getLocalId(): s for s in stats.getSignals(daq.AnySearchFilter())}
        self.assertFalse(sigs["rms"].getPublic())
        self.assertEqual(sigs["avg"].name, "Average")

    def test_connections(self):
        stats = self.find("/openDAQDevice/FB/RefFBModuleStatistics_1", daq.IFunctionBlock)
        sig = stats.getInputPorts()[0].getSignal()
        self.assertIsNotNone(sig)
        self.assertEqual(sig.getGlobalId(), "/openDAQDevice/Dev/RefDev0/IO/AI/RefCh0/Sig/AI0")

    def test_nested_fb_on_device(self):
        dev = self.inst.getDevices()[0]
        fbs = dev.getFunctionBlocks()
        self.assertEqual([f.getLocalId() for f in fbs], ["RefFBModuleScaling_1"])
        self.assertEqual(fbs[0].name, "Scale on device")
        self.assertEqual(fbs[0].getPropertyValue("scale"), 0.5)
        self.assertIsNotNone(fbs[0].getInputPorts()[0].getSignal())

    def test_saved_setup_matches_schema(self):
        saved = json.loads(self.inst.saveConfiguration())
        self.assertEqual(validate(saved, self.setup.schema), [])
        self.assertEqual(self.setup.validate(saved), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
