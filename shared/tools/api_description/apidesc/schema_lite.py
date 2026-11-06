"""A small JSON Schema validator for the subset the setup schema uses.

Supports $ref (local), allOf, anyOf, oneOf, type, const, enum, required,
properties, additionalProperties, items, pattern, minProperties,
maxProperties. Returns a list of "path: message" strings.
"""

import re

TYPES = {
    "object": lambda v: isinstance(v, dict), "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str), "boolean": lambda v: isinstance(v, bool),
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool), "null": lambda v: v is None,
}


def validate(value, schema, root=None, path="$"):
    root = root or schema
    errs = []
    if "$ref" in schema:
        ref = schema["$ref"]
        target = root
        for part in ref.lstrip("#/").split("/"):
            target = target[part]
        errs += validate(value, target, root, path)
    for sub in schema.get("allOf", []):
        errs += validate(value, sub, root, path)
    if "anyOf" in schema:
        if not any(not validate(value, s, root, path) for s in schema["anyOf"]):
            errs.append(f"{path}: matches none of the allowed forms")
    if "oneOf" in schema:
        n = sum(1 for s in schema["oneOf"] if not validate(value, s, root, path))
        if n != 1:
            errs.append(f"{path}: matches {n} of the oneOf forms, expected 1")
    t = schema.get("type")
    if t:
        ts = t if isinstance(t, list) else [t]
        if not any(TYPES[x](value) for x in ts):
            return errs + [f"{path}: expected {' or '.join(ts)}, got {type(value).__name__}"]
    if "const" in schema and value != schema["const"]:
        errs.append(f"{path}: expected {schema['const']!r}, got {value!r}")
    if "enum" in schema and value not in schema["enum"]:
        errs.append(f"{path}: {value!r} is not one of {schema['enum']}")
    if isinstance(value, str) and "pattern" in schema and not re.search(schema["pattern"], value):
        errs.append(f"{path}: {value!r} does not match {schema['pattern']}")
    if isinstance(value, dict):
        for r in schema.get("required", []):
            if r not in value:
                errs.append(f"{path}: missing required key '{r}'")
        props = schema.get("properties", {})
        for k, v in value.items():
            if k in props:
                errs += validate(v, props[k], root, f"{path}.{k}")
            elif isinstance(schema.get("additionalProperties"), dict):
                errs += validate(v, schema["additionalProperties"], root, f"{path}.{k}")
            elif schema.get("additionalProperties") is False:
                errs.append(f"{path}: unexpected key '{k}'")
        if "minProperties" in schema and len(value) < schema["minProperties"]:
            errs.append(f"{path}: needs at least {schema['minProperties']} keys")
        if "maxProperties" in schema and len(value) > schema["maxProperties"]:
            errs.append(f"{path}: allows at most {schema['maxProperties']} keys")
    if isinstance(value, list) and isinstance(schema.get("items"), dict):
        for i, v in enumerate(value):
            errs += validate(v, schema["items"], root, f"{path}[{i}]")
    return errs
