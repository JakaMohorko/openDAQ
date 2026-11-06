"""RTGen-style attribute tags and Doxygen comments.

Tags are `[name(arg, arg)]` items in `// [...]` line comments or `/*# ... */`
blocks. The `api` tag (spec 0010) carries the audience and modifiers:
`[api(public)]`, `[api(module, override)]`, `[api(public, coretype)]`.
"""

import re

AUDIENCES = ("application", "module", "core")
MODIFIERS = ("coretype", "override", "readOnly", "deprecated", "callback")


def _split_args(s):
    out, cur, depth, q = [], "", 0, None
    for ch in s:
        if q:
            cur += ch
            if ch == q:
                q = None
            continue
        if ch in "\"'":
            q = ch
            cur += ch
            continue
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def _parse_items(inner):
    """'a(x, y), b' -> [{'name': 'a', 'args': ['x', 'y']}, {'name': 'b', 'args': []}]"""
    items = []
    for part in _split_args(inner):
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(?:\((.*)\))?$", part.strip(), re.S)
        if not m:
            continue
        args = _split_args(m.group(2)) if m.group(2) is not None else []
        items.append({"name": m.group(1), "args": [a.strip() for a in args]})
    return items


def is_tag_comment(text):
    return text.startswith("/*#") or bool(re.match(r"^//\s*\[", text))


def tags_from_comment(text):
    """All attribute items in one comment token."""
    if text.startswith("/*#"):
        body = text[3:-2]
        items = []
        for m in re.finditer(r"\[((?:[^\[\]]|\[[^\]]*\])*)\]", body):
            inner = re.sub(r"\n\s*\*", " ", m.group(1))
            items.extend(_parse_items(inner))
        return items
    m = re.match(r"^//\s*\[(.*)\]\s*$", text)
    if m:
        return _parse_items(m.group(1))
    return []


def tags_from_comments(comments):
    items = []
    for c in comments:
        items.extend(tags_from_comment(c.text))
    return items


def api_tag(items):
    """Audience and modifiers from the last api tag, or (None, [])."""
    found = None
    for it in items:
        if it["name"] == "api":
            found = it
    if not found or not found["args"]:
        return None, []
    aud = found["args"][0]
    mods = found["args"][1:]
    return aud, mods


def rt_attrs(items):
    """RTGen attributes other than api, grouped by name."""
    out = {}
    for it in items:
        if it["name"] == "api":
            continue
        out.setdefault(it["name"], []).append(it["args"])
    return out


# ----------------------------------------------------------------------
def doc_from_comments(comments):
    docs = [c.text for c in comments if c.text.startswith("/*!") or c.text.startswith("///") or c.text.startswith("//!")]
    if not docs:
        return None
    return parse_doc(docs[-1])


def _clean(text):
    if text.startswith("/*"):
        text = text[3:-2] if text.startswith(("/*!", "/**")) else text[2:-2]
    lines = []
    for ln in text.splitlines():
        ln = re.sub(r"^\s*\*\s?", "", ln)
        ln = re.sub(r"^\s*//[/!]\s?", "", ln)
        lines.append(ln.rstrip())
    return "\n".join(lines).strip()


def parse_doc(text):
    t = _clean(text)
    doc = {"brief": None, "details": None, "params": {}, "retvals": [], "returns": None}
    blocks = re.split(r"\n(?=\s*@)", "\n" + t)
    details = []
    for b in blocks:
        b = b.strip()
        if not b:
            continue
        m = re.match(r"@brief\s+(.*)", b, re.S)
        if m:
            parts = re.split(r"\n\s*\n", m.group(1), maxsplit=1)
            doc["brief"] = " ".join(parts[0].split())
            if len(parts) > 1:
                details.append(parts[1].strip())
            continue
        m = re.match(r"@param(?:\[([a-z, ]+)\])?\s+(\w+)\s*(.*)", b, re.S)
        if m:
            parts = re.split(r"\n\s*\n", m.group(3), maxsplit=1)
            doc["params"][m.group(2)] = {"dir": (m.group(1) or "").replace(" ", "") or None,
                                         "text": " ".join(parts[0].split())}
            if len(parts) > 1:
                details.append(parts[1].strip())
            continue
        m = re.match(r"@retval\s+(\w+)\s*(.*)", b, re.S)
        if m:
            doc["retvals"].append({"code": m.group(1), "text": " ".join(m.group(2).split())})
            continue
        m = re.match(r"@returns?\s+(.*)", b, re.S)
        if m:
            doc["returns"] = " ".join(m.group(1).split())
            continue
        if re.match(r"@(ingroup|addtogroup|defgroup|\{|\})", b):
            continue
        if b.startswith("@"):
            details.append(b)
            continue
        if doc["brief"] is None and not details:
            parts = re.split(r"\n\s*\n", b, maxsplit=1)
            doc["brief"] = " ".join(parts[0].split())
            if len(parts) > 1:
                details.append(parts[1].strip())
        else:
            details.append(b)
    if details:
        doc["details"] = "\n\n".join(d for d in details if d)
    if not doc["params"]:
        del doc["params"]
    if not doc["retvals"]:
        del doc["retvals"]
    return {k: v for k, v in doc.items() if v}
