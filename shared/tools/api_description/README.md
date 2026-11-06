# openDAQ API description

A machine-readable description of the openDAQ API and its implementation objects (spec 0012), generated from the headers, plus the audience tags it reads (spec 0010) and four consumers that prove the description is complete enough to generate from: natvis files, a Python binding, an offline setup builder and a release compatibility report. Everything here is Python 3.9+ with the standard library; `lxml` is used only to validate natvis against Visual Studio's schema.

The parser is independent of RTGen. RTGen still generates the smart pointers and bindings, and the tags in the headers leave its output byte-identical (`checks/rtgen_check.py`).

## Layout

| Path | Content |
| ---- | ------- |
| `generate.py`, `apidesc/` | The C++ tokenizer, the declaration parser and the model that writes `output/opendaq_api.json`. |
| `scan_usage.py`, `config/usage_sources.json` | Usage evidence: which interfaces, methods, factories and implementation members applications (examples, docs, Python GUI) and modules (repositories next to openDAQ) use. |
| `classify.py`, `annotate.py` | Propose an audience for every declaration from rules and evidence, and write or update the `// [api(...)]` tags in the headers. |
| `review/` | The review page: every proposal with its reason and evidence. Decisions made there come back through `annotate.py --apply`. |
| `schemas/` | Hand-written JSON schema of a saved setup, keyed by serialize ID, and the rules the core follows when it loads one. |
| `generators/natvis/` | Visual Studio natvis files from the implementation classes; also checks the hand-written natvis files against the code. |
| `generators/python_ctypes/` | A Python binding that calls the openDAQ libraries through raw vtables, built only from the JSON. |
| `setup/` | The setup builder: a catalog dump of the types a build provides, a Python library and a web page that write setups `loadConfiguration` accepts. |
| `generators/api_diff/` | A compatibility report between two descriptions (ABI, API, setup format). `--audiences` takes audiences from a tagged description when the compared trees are untagged. |
| `generators/c_header/` | A plain C header of the ABI: one vtable struct per interface (`daq`-prefixed names), interface ids, error codes, enums, structs and factory prototypes. |
| `describe_module.py` | A description of one module repository in the same format, with `coreUsage`: the core classes it derives from, the core members and interfaces it uses with their tags, `violations` (uses of `core` API) and `interfaceOverrides` (core interface implementations it replaces). |
| `schemas/api_description.schema.json` | The format of the description itself; `generate.py` validates every file it writes against it. |
| `checks/` | Tests against a real build and the RTGen comparison. |
| `ISSUES.md` | Bugs and inconsistencies found on the way. |

## Commands

```bash
python generate.py                                   # output/opendaq_api.json
python scan_usage.py                                 # output/usage.json
python classify.py && python annotate.py --dry-run   # proposals; nothing to change once the headers are tagged
python review/build_review.py                        # output/review/api-audience-review.html
python generators/natvis/generate.py --xsd "<VS>/Xml/Schemas/1033/natvis.xsd"
python generators/python_ctypes/generate.py          # output/bindings/python/opendaq_ct
OPENDAQ_BIN=<build>/bin python setup/catalog.py      # output/setup/catalog.json
python setup/page/build_page.py                      # output/setup/opendaq-setup-builder.html
python generators/api_diff/diff.py old.json output/opendaq_api.json --md report.md
OPENDAQ_BIN=<build>/bin python checks/test_python_ctypes.py
OPENDAQ_BIN=<build>/bin python checks/test_setup.py
python generators/c_header/generate.py && OPENDAQ_BIN=<build>/bin python checks/test_c_header.py
python describe_module.py ../raspberry-module/rpi_device_module   # output/modules/rpi_device_module.json
python checks/check_tags.py                          # fails on any declaration without an audience tag
python checks/rtgen_check.py <dir-before>            # then again after a header change, and --compare
```

`OPENDAQ_BIN` is the `bin` folder of a build whose source matches the description (the libraries and the module DLLs). The reference modules are needed for the catalog and the setup tests.

## Audience tags

A tag is one comment line directly above the declaration, in RTGen attribute syntax so RTGen's parser accepts it.

| Where | Tag | Meaning |
| ----- | --- | ------- |
| Interface | `// [api(application)]`, `// [api(module)]`, `// [api(core)]` | The default audience of every method of the interface. `application` is the name used for what spec 0010 calls `public`, because RTGen's lexer treats `public` as a keyword. |
| Interface | `// [api(application, coretype)]` | One of the 17 coretypes: openDAQ's wrappers of standard programming types, which bindings map to native values (IBaseObject, IBoolean, IInteger, IFloat, INumber, IString, IRatio, IComplexNumber, IBinaryData, IList, IDict, IIterable, IIterator, IFunction, IProcedure, IEvent, IEventHandler). The list follows what the Python, C# and C generators special-case. |
| Interface method | `// [api(core)]` | Only where the method differs from its interface. |
| Factory macro, C++ factory, smart pointer, enum | `// [api(...)]` | Who may create or use it. |
| Implementation class | `// [api(module)]` | Modules derive from it (or from a class that derives from it). Other installed implementation classes are tagged `core`. |
| Implementation function | `// [api(module)]`, `// [api(module, override)]`, `// [api(core)]` | Modules may call it, override it, or neither. Interface overrides are not tagged; they follow the interface. |
| Member variable | `// [api(module)]`, `// [api(module, readOnly)]`, `// [api(core)]` | Modules may write it, only read it, or neither. |

## The description (`opendaq_api.json`, format version 1)

| Part | Content |
| ---- | ------- |
| `libraries` | Library names, binary names per platform and source folders. |
| `scalars`, `structs`, `callbacks` | The C ABI types used in signatures: scalar typedefs with size, by-value structs with fields and packing (`ComplexFloat64`, `SourceLocation`), and function-pointer typedefs (`FuncCall`, `ProcCall`). |
| `errors` | Every `OPENDAQ_ERR_*` with its numeric value and the C++ exception that carries it. |
| `enums` | Enumerators with evaluated values, underlying type and audience. |
| `interfaces` | Name, interface ID (UUIDv5 as `DEFINE_INTFID` computes it), base, library, header, audience, `coretype`, smart pointer, RTGen attributes, doc, `abi` (`c` or `cpp`), implementing classes and creating factories. Each method in vtable order with its audience, doc, `abi`, parameters (type, kind, pointer depth, direction, default, element/key/value types) and the error codes its implementations raise. |
| `factories` | C factories: name, create function, returned interface, exported or internal, parameters, implementation class. `cppFactories` holds the inline C++ factories. |
| `functions` | Exported free functions (`daqGetErrorInfoMessage`, `createJsonSerializer`, ...). |
| `implementations` | Implementation classes: template parameters, bases, base chain, all interfaces they answer, factories, serialize ID, members and functions with access, virtual/override flags and audience. `smartPointers` and `typeAliases` hold the smart pointer classes and the aliases (`Device`, `SignalImpl`). |
| `serialization`, `setup` | Per serialize ID: writer, reader, whether the deserializer is registered, the keys the code writes and reads, and the schema definition for setup objects. `setup` holds the setup schema and the load rules. |

## Decisions

| Topic | Decision |
| ----- | -------- |
| Tag name | `application`, not `public`; see `ISSUES.md` #1. |
| Coretype | A modifier on the interface tag (`coretype`), exported as `"coretype": true` (spec 0012 open question). IStruct and IEnumeration are not coretypes: their fields come from runtime types. IErrorInfo maps to exceptions, not values. |
| Parser | Our own declaration-level parser rather than clang: no build dependency, under two seconds for the whole core, and it reads comments where the tags live. |
| Serialization schemas | Hand-written, as spec 0012 recommends, and checked by `checks/test_setup.py` against setups the core saves. The `keysWritten`/`keysRead` lists are extracted automatically as a cross-check. |
| What applications write | Applications connect signals, set property values, add or remove function blocks, devices, servers and streaming, change unlocked attributes (name, description, active, visible, public, operation mode) and set up the instance; otherwise they read. Writing methods on application interfaces outside that list are tagged `module`, with judgment on top: access control (`setPermissions` and the permission builders) and the scheduler calls an application makes are `application`; calls only the core makes are `core`. |
| Factories | Applications get objects from the API. Factories are `module` except those for core types, the instance and readers; factories for arguments of instance-setup and read calls (config providers, logger sinks, authentication providers, users, update parameters, search filters) are `application` and marked `[CHECK]` on the review page. |
| Method tags | Written only where a method differs from its interface (spec 0010); about 1280 methods follow their interface and carry no tag. |
| Untagged implementation members | Members that are private, or classes no module derives from, carry no member tags; they are core by default. |
| Source of truth | The header tags. `classify.py` re-derives a proposal for every entry and flags where a tag disagrees with it; `--prefer-rules` lets new evidence win before a review. |

## Checked against other trees

| Tree | Result |
| ---- | ------ |
| v3.40.3 (`../openDAQ-api-json-v3403`) | Parses without errors. The report against main (`generators/api_diff/diff.py`) lists 30 ABI breaks, mostly methods inserted mid-interface; see `ISSUES.md` #12. |
| `multi-reader-2-v3.1` at 610aab89c (`../openDAQ-cmp-v3`, read only), against its fork point 891d4ae61 | Purely additive: interfaces `IMultiReader2`, `IMultiReader2Params`, `IMultiReader2Status`, `IMultiReader2InputStatus`, `IMultiReader2PortListener` and factories `MultiReader2`, `MultiReader2Params`; no ABI, API or setup-format change. Proposed audiences: `application` for the reader, its params, statuses and factories; `core` for `IMultiReader2PortListener`, an internal marker that `MultiReader2Impl` uses to recognise its own ports and that is declared in the implementation header `multi_reader2_impl.h`. Diffing that branch against main instead mixes in what main gained since 891d4ae61. |
| car-demo `Json_demo_setups` (setups saved with STAR devices) | Both files pass the setup schema. |
| Module descriptions (ref device and FB, native streaming client and server, OPC UA, LT client, raspberry, MQTT, universal adapter, ASAM-CMP) | No module uses `core` API. Interface overrides: ASAM-CMP replaces `IPropertyObject::beginUpdate`/`endUpdate` (`ISSUES.md` #22); the native streaming client implements `IComponentPrivate::setComponentConfig` and `IStreamingPrivate::setOwnerDevice`, so those stay `module`. |
| Generated C header | Compiles with MSVC `/W4 /WX`; `checks/test_c_header.c` creates strings and lists, queries interfaces by the generated ids, builds an instance and adds the reference device through the generated vtables. |

Usage evidence comes from the sources in `config/usage_sources.json`: the openDAQ checkout, the local clones next to it as they are checked out (several are on working branches, for example raspberry-module at 2026-07-21 and iDDS-Module on a 2024 branch), and shallow clones in `../opendaq-org-cache` of the organization repositories without a local clone. Integration tests and documentation are shown on the review page but do not decide proposals.
