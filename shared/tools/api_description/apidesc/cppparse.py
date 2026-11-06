"""Declaration-level C++ parser.

Splits a token stream into declarations (namespace scope or class scope)
without understanding expressions: function bodies and initializers are
skipped by brace matching and kept as source slices. Good enough for the
regular style of the openDAQ headers; anything it cannot place is
reported, not guessed.
"""

import re
from dataclasses import dataclass, field

from .lexer import Tok, tokenize

ACCESS = ("public", "protected", "private")
# identifiers that decorate declarations and carry no meaning for us
NOISE = {
    "OPENDAQ_NO_VTABLE", "DAQ_EMPTY_BASES", "PUBLIC_EXPORT", "INTERFACE_FUNC", "OPENDAQ_API",
    "LIBRARY_FACTORY", "INTERNAL_FACTORY", "INLINE_FACTORY", "EXPORT", "final", "OPENDAQ_EXPORTS",
}
SPECIFIERS = {"virtual", "static", "inline", "constexpr", "explicit", "friend", "extern", "mutable",
              "consteval", "constinit", "thread_local", "volatile", "register"}


@dataclass
class Decl:
    kind: str  # function, variable, type, macro, using, other
    head: list  # code tokens of the head
    comments: list = field(default_factory=list)  # comment toks before
    trailing: list = field(default_factory=list)  # comment toks on the same line after
    body: tuple = None  # (start_tok, end_tok) of {...} including braces
    access: str = None
    line: int = 0
    children: list = None  # for types: member decls
    info: dict = field(default_factory=dict)
    src_span: tuple = None  # (start, end) offsets in the file


def match(toks, i):
    """Index of the token closing the bracket at toks[i]."""
    open_ = toks[i].text
    close = {"(": ")", "[": "]", "{": "}", "<": ">"}[open_]
    depth = 0
    for j in range(i, len(toks)):
        t = toks[j]
        if t.kind != "punct":
            continue
        if t.text == open_:
            depth += 1
        elif t.text == close:
            depth -= 1
            if depth == 0:
                return j
    return len(toks) - 1


def match_angle(code, i):
    depth = 0
    for j in range(i, len(code)):
        t = code[j].text
        if t == "<":
            depth += 1
        elif t == ">":
            depth -= 1
            if depth == 0:
                return j
        elif t in "({[":
            pass
    return len(code) - 1


def _next_code(toks, i, end):
    while i < end and toks[i].kind in ("comment", "pp"):
        i += 1
    return i


def _is_macro_name(s):
    return len(s) > 2 and s.upper() == s and re.match(r"^[A-Z_][A-Z0-9_]*$", s) is not None


class Parser:
    def __init__(self, src, path=""):
        self.src = src
        self.path = path
        self.toks = tokenize(src)
        self.problems = []

    def text(self, a, b):
        """Source text from token a to token b inclusive."""
        return self.src[self.toks[a].start:self.toks[b].end]

    # ------------------------------------------------------------------
    def parse(self):
        return self.split(0, len(self.toks), in_class=None)

    def split(self, i, end, in_class=None, namespace=()):
        """in_class: None at namespace scope, else 'class' or 'struct'."""
        toks = self.toks
        decls = []
        pending = []
        access = None if in_class is None else ("private" if in_class == "class" else "public")
        last_decl_line = -1
        while i < end:
            t = toks[i]
            if t.kind == "comment":
                if decls and t.line == last_decl_line and t.text.startswith("//"):
                    decls[-1].trailing.append(t)
                else:
                    pending.append(t)
                i += 1
                continue
            if t.kind == "pp":
                i += 1
                continue
            if t.text in (";", "}") and t.kind == "punct":
                i += 1
                continue
            if in_class and t.kind == "id" and t.text in ACCESS:
                j = _next_code(toks, i + 1, end)
                if j < end and toks[j].text == ":":
                    access = t.text
                    i = j + 1
                    pending = []
                    continue
            # macro statements: ALL_CAPS(…) without trailing ; or with one
            if t.kind == "id" and _is_macro_name(t.text) and t.text not in NOISE and not t.text.startswith("DECLARE_") \
                    and not t.text.startswith("DECLARE_TEMPLATED"):
                j = _next_code(toks, i + 1, end)
                if j < end and toks[j].text == "(":
                    k = match(toks, j)
                    after = _next_code(toks, k + 1, end)
                    if after < end and toks[after].text in ("{",):
                        pass  # a function-like macro heading a body; fall through
                    else:
                        d = Decl("macro", [x for x in toks[i:k + 1] if x.kind not in ("comment", "pp")],
                                 comments=pending, access=access, line=t.line)
                        d.info["name"] = t.text
                        d.info["args"] = self.macro_args(j, k)
                        d.src_span = (t.start, toks[k].end)
                        decls.append(d)
                        last_decl_line = toks[k].line
                        pending = []
                        # resume right after the macro so comments before the next declaration are kept
                        i = after + 1 if after < end and toks[after].text == ";" else k + 1
                        continue
                else:
                    # bare macro (BEGIN_NAMESPACE_OPENDAQ) unless it starts a declaration
                    nxt = toks[j] if j < end else None
                    if nxt is None or nxt.kind != "id" or t.text.startswith(("BEGIN_", "END_")):
                        i += 1
                        continue
            d, i = self.read_decl(i, end, pending, access, in_class, namespace)
            pending = []
            if d is None:
                continue
            if d.kind == "namespace":
                b0, b1 = d.body
                inner_ns = namespace + ((d.info.get("name"),) if d.info.get("name") else ())
                decls.extend(self.split(b0 + 1, b1, None, inner_ns))
                continue
            decls.append(d)
            last_decl_line = toks[i - 1].line if i - 1 < len(toks) else -1
        return decls

    def macro_args(self, j, k):
        """Split macro arguments at top-level commas, returning source strings."""
        toks = self.toks
        args = []
        start = j + 1
        depth = 0
        for x in range(j + 1, k):
            t = toks[x]
            if t.kind != "punct":
                continue
            if t.text in "([{":
                depth += 1
            elif t.text in ")]}":
                depth -= 1
            elif t.text == "," and depth == 0:
                args.append(self.src[toks[start].start:toks[x - 1].end].strip() if x > start else "")
                start = x + 1
        if start < k:
            args.append(self.src[toks[start].start:toks[k - 1].end].strip())
        return [re.sub(r"\s+", " ", a) for a in args]

    def read_decl(self, i, end, pending, access, in_class, namespace):
        toks = self.toks
        start = i
        code = []
        j = i
        saw_paren_group = False
        saw_eq = False
        init_list = False
        last_code = None
        in_tmpl = False
        tmpl_depth = 0
        while j < end:
            t = toks[j]
            if t.kind in ("comment", "pp"):
                j += 1
                continue
            if not code and t.text == "template":
                in_tmpl = True
            if t.kind == "punct" and t.text in ("(", "["):
                k = match(toks, j)
                code.extend(x for x in toks[j:k + 1] if x.kind not in ("comment", "pp"))
                if t.text == "(" and not saw_eq and not in_tmpl:
                    saw_paren_group = True
                last_code = toks[k]
                j = k + 1
                continue
            if t.kind == "punct" and t.text == ";":
                code_head = code
                d = self.make_decl(code_head, None, pending, access, start, j)
                return d, j + 1
            if t.kind == "punct" and t.text == ":" and saw_paren_group and not saw_eq and not is_type_head(code):
                init_list = True
            if t.kind == "punct" and t.text in ("<", ">") and code and code[0].text == "template" and in_tmpl:
                tmpl_depth += 1 if t.text == "<" else -1
                if tmpl_depth == 0:
                    in_tmpl = False
            if t.kind == "punct" and t.text == "=" and not in_tmpl and not (last_code is not None and last_code.text == "operator"):
                saw_eq = True
            if t.kind == "punct" and t.text == "{":
                k = match(toks, j)
                head_ids = [c.text for c in code]
                type_kw = is_type_head(code)
                is_decl_iface = bool(code) and code[0].text.startswith(("DECLARE_OPENDAQ", "DECLARE_TEMPLATED_OPENDAQ"))
                if "namespace" in head_ids[:2] and not saw_paren_group:
                    d = Decl("namespace", code, body=(j, k), line=toks[start].line)
                    d.info["name"] = head_ids[1] if len(head_ids) > 1 and head_ids[0] == "namespace" else None
                    return d, k + 1
                if head_ids[:1] == ["extern"] and len(code) == 2 and code[1].kind == "str":
                    d = Decl("namespace", code, body=(j, k), line=toks[start].line)
                    return d, k + 1
                if is_decl_iface or (type_kw and not saw_eq):
                    # type body, then declarators until ;
                    e = k + 1
                    while e < end and not (toks[e].kind == "punct" and toks[e].text == ";"):
                        if toks[e].kind == "punct" and toks[e].text == "{":
                            e = match(toks, e)
                        e += 1
                    d = self.make_decl(code, (j, k), pending, access, start, k, is_type=True)
                    return d, e + 1
                if saw_eq or (init_list and last_code is not None and (last_code.kind == "id" or last_code.text == ">")):
                    # initializer braces
                    code.extend(x for x in toks[j:k + 1] if x.kind not in ("comment", "pp"))
                    last_code = toks[k]
                    j = k + 1
                    continue
                if saw_paren_group:
                    d = self.make_decl(code, (j, k), pending, access, start, k)
                    return d, k + 1
                # brace-initialized variable  int x{5};
                code.extend(x for x in toks[j:k + 1] if x.kind not in ("comment", "pp"))
                last_code = toks[k]
                j = k + 1
                continue
            if t.kind == "punct" and t.text == "}":
                # stray close: unbalanced (preprocessor branches)
                self.problems.append(f"{self.path}:{t.line}: unbalanced '}}'")
                return None, j + 1
            code.append(t)
            last_code = t
            j += 1
        if code:
            self.problems.append(f"{self.path}:{toks[start].line}: unterminated declaration")
        return None, end

    # ------------------------------------------------------------------
    def make_decl(self, code, body, pending, access, start_idx, end_idx, is_type=False):
        toks = self.toks
        line = code[0].line if code else toks[start_idx].line
        d = Decl("other", code, comments=list(pending), body=body, access=access, line=line)
        d.src_span = (toks[start_idx].start, toks[end_idx].end)
        if not code:
            return None
        c = list(code)
        tmpl = None
        if c[0].text == "template" and len(c) > 1 and c[1].text == "<":
            k = match_angle(c, 1)
            tmpl = c[2:k]
            c = c[k + 1:]
            if not c:
                return None
        d.info["template"] = " ".join(x.text for x in tmpl) if tmpl is not None else None
        d.info["template_params"] = parse_template_params(tmpl) if tmpl is not None else None
        first = c[0].text
        if first.startswith(("DECLARE_OPENDAQ", "DECLARE_TEMPLATED_OPENDAQ")):
            d.kind = "interface"
            d.info["macro"] = first
            d.info["args"] = [x.strip() for x in tokens_text(c[2:-1]).split(",")] if len(c) > 3 else []
            if body:
                d.children = self.split(body[0] + 1, body[1], in_class="struct")
            return d
        if first in ("using",):
            d.kind = "using"
            ids = [x.text for x in c]
            if "=" in ids:
                d.info["name"] = ids[ids.index("=") - 1]
                d.info["value"] = tokens_text(c[ids.index("=") + 1:])
            else:
                d.info["name"] = ids[-1]
            return d
        if first in ("typedef", "friend", "static_assert"):
            d.kind = first
            return d
        cc = [x for x in c if x.text not in NOISE and x.text not in ("[[", "]]")]
        # strip attributes [[...]]
        while cc and cc[0].text == "[" and len(cc) > 1 and cc[1].text == "[":
            k = match(cc, 0) if False else None
            break
        if cc and cc[0].text in ("class", "struct", "union", "enum") and (is_type or cc[0].text == "enum" or True):
            if is_type or "{" not in [x.text for x in cc]:
                return self._type_decl(d, cc, body, is_type)
        # function?
        angle = 0
        paren_idx = None
        for idx, x in enumerate(cc):
            if x.text == "<" and idx > 0 and (cc[idx - 1].kind == "id" and cc[idx - 1].text != "operator"):
                angle += 1
            elif x.text == ">" and angle > 0:
                angle -= 1
            elif x.text == "=" and angle == 0 and not (idx > 0 and cc[idx - 1].text == "operator"):
                break
            elif x.text == "(" and angle == 0:
                paren_idx = idx
                break
        if paren_idx is not None and paren_idx > 0:
            return self._func_decl(d, cc, paren_idx, body)
        if is_type:
            d.kind = "type?"
            return d
        return self._var_decl(d, cc)

    def _type_decl(self, d, cc, body, is_type):
        d.kind = "type"
        kw = cc[0].text
        rest = cc[1:]
        if kw == "enum" and rest and rest[0].text in ("class", "struct"):
            kw = "enum class"
            rest = rest[1:]
        rest = [x for x in rest if x.text not in NOISE]
        d.info["keyword"] = kw
        if not rest:
            d.info["name"] = None
            return d
        # name possibly with template specialization
        name = rest[0].text
        idx = 1
        if idx < len(rest) and rest[idx].text == "<":
            k = match_angle(rest, idx)
            d.info["specialization"] = tokens_text(rest[idx + 1:k])
            idx = k + 1
        d.info["name"] = name
        bases = []
        underlying = None
        if idx < len(rest) and rest[idx].text == ":":
            btoks = rest[idx + 1:]
            if kw.startswith("enum"):
                underlying = tokens_text(btoks)
            else:
                for part in split_top(btoks, ","):
                    words = [x for x in part if x.text not in ("public", "protected", "private", "virtual")]
                    if words:
                        bases.append(tokens_text(words))
        d.info["bases"] = bases
        d.info["underlying"] = underlying
        d.info["forward"] = body is None
        if body is not None:
            if kw.startswith("enum"):
                d.info["enumerators"] = self._enumerators(body)
            else:
                d.children = self.split(body[0] + 1, body[1], in_class="class" if kw == "class" else "struct")
        return d

    def _enumerators(self, body):
        toks = self.toks
        items = []
        start = body[0] + 1
        pending = []
        depth = 0
        cur = []
        for x in range(body[0] + 1, body[1] + 1):
            t = toks[x]
            if t.kind == "pp":
                continue
            if t.kind == "comment":
                if cur:
                    # trailing comment belongs to current enumerator
                    pending.append(t)
                else:
                    pending.append(t)
                continue
            if t.kind == "punct" and t.text in "([{" and x != body[0]:
                depth += 1
            if (t.kind == "punct" and t.text == "," and depth == 0) or x == body[1]:
                if cur:
                    name = cur[0].text
                    value = tokens_text(cur[2:]) if len(cur) > 2 and cur[1].text == "=" else None
                    items.append({"name": name, "value": value, "comments": [c.text for c in pending]})
                cur = []
                pending = []
                continue
            if t.kind == "punct" and t.text in ")]}":
                depth -= 1
            cur.append(t)
        return items

    def _func_decl(self, d, cc, paren_idx, body):
        d.kind = "function"
        # name
        k = paren_idx - 1
        name_toks = []
        if cc[k].kind == "punct" and any(x.text == "operator" for x in cc[:paren_idx]):
            op = [x.text for x in cc].index("operator")
            name_toks = cc[op:paren_idx]
            # operator() : the first () is the name
            if paren_idx == op + 1 and cc[paren_idx].text == "(" and cc[paren_idx + 1].text == ")":
                name_toks = cc[op:paren_idx + 2]
                paren_idx = paren_idx + 2
            k = op - 1
        else:
            name_toks = [cc[k]]
            k -= 1
            if k >= 0 and cc[k].text == "~":
                name_toks.insert(0, cc[k])
                k -= 1
        # qualification  A<B>::C::name
        qual = []
        while k >= 1 and cc[k].text == "::":
            k -= 1
            if cc[k].text == ">":
                depth = 0
                m = k
                while m >= 0:
                    if cc[m].text == ">":
                        depth += 1
                    elif cc[m].text == "<":
                        depth -= 1
                        if depth == 0:
                            break
                    m -= 1
                qual.insert(0, tokens_text(cc[m - 1:k + 1]))
                k = m - 2
            else:
                qual.insert(0, cc[k].text)
                k -= 1
        ret = cc[:k + 1]
        specs = [x.text for x in ret if x.text in SPECIFIERS]
        ret = [x for x in ret if x.text not in SPECIFIERS]
        close = match(cc, paren_idx)
        params = parse_params(cc[paren_idx + 1:close])
        tail = cc[close + 1:]
        tail_text = [x.text for x in tail]
        d.info.update({
            "name": "".join(x.text for x in name_toks) if name_toks and name_toks[0].text == "operator" else tokens_text(name_toks),
            "qualifier": "::".join(qual) if qual else None,
            "return": tokens_text(ret),
            "specifiers": specs,
            "params": params,
            "const": "const" in tail_text[:3],
            "override": "override" in tail_text,
            "final": "final" in tail_text,
            "pure": tail_text[-2:] == ["=", "0"],
            "deleted": tail_text[-2:] == ["=", "delete"],
            "defaulted": tail_text[-2:] == ["=", "default"],
            "noexcept": "noexcept" in tail_text,
        })
        if body is not None:
            d.info["body"] = self.text(body[0], body[1])
        return d

    def _var_decl(self, d, cc):
        d.kind = "variable"
        ids = [x.text for x in cc]
        # cut at initializer
        cut = len(cc)
        angle = 0
        for idx, x in enumerate(cc):
            if x.text == "<" and idx > 0 and cc[idx - 1].kind == "id":
                angle += 1
            elif x.text == ">" and angle > 0:
                angle -= 1
            elif angle == 0 and x.text in ("=", "{", "[", ":") and idx > 0:
                if x.text == ":" and idx + 1 < len(cc) and cc[idx + 1].text == ":":
                    continue
                cut = idx
                break
        decl = cc[:cut]
        init = cc[cut:]
        if not decl:
            d.kind = "other"
            return d
        name = decl[-1].text
        typ = decl[:-1]
        specs = [x.text for x in typ if x.text in SPECIFIERS]
        typ = [x for x in typ if x.text not in SPECIFIERS]
        d.info.update({
            "name": name,
            "type": tokens_text(typ),
            "specifiers": specs,
            "init": tokens_text(init[1:]) if init and init[0].text == "=" else (tokens_text(init) if init else None),
            "array": bool(init) and init[0].text == "[",
        })
        if not typ:
            d.kind = "other"
        return d


# ----------------------------------------------------------------------
def is_type_head(code):
    c = list(code)
    if c and c[0].text == "typedef":
        c = c[1:]
    if len(c) > 1 and c[0].text == "template" and c[1].text == "<":
        c = c[match_angle(c, 1) + 1:]
    c = [x for x in c if x.text not in NOISE]
    return bool(c) and c[0].text in ("class", "struct", "union", "enum")


def tokens_text(ts):
    out = ""
    prev = None
    for t in ts:
        s = t.text
        if prev is not None:
            glue = (prev.text in ("::", "<", "(", "[", "~", "!", ".", "->") or s in ("::", ">", ")", "]", ",", "(", "[", ".", "->", ";")
                    or (s == "<" and prev.kind == "id") or (s in ("*", "&", "&&") and prev.kind in ("id",) and False))
            if s in ("*", "&", "&&"):
                glue = prev.text in ("*", "&", "<", "(")
            if prev.text in ("*", "&") and s in ("*", "&", ">", ")", ","):
                glue = True
            if s == "(" and prev.kind == "id" and prev.text in ("const", "return"):
                glue = False
            out += s if glue else " " + s
        else:
            out = s
        prev = t
    return out


def split_top(ts, sep):
    parts = []
    cur = []
    depth = 0
    for t in ts:
        if t.text in ("(", "[", "{", "<"):
            depth += 1
        elif t.text in (")", "]", "}", ">"):
            depth -= 1
        if t.text == sep and depth == 0:
            parts.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        parts.append(cur)
    return parts


def parse_template_params(ts):
    out = []
    for part in split_top(ts, ","):
        txt = [x.text for x in part]
        default = None
        if "=" in txt:
            e = txt.index("=")
            default = tokens_text(part[e + 1:])
            part = part[:e]
            txt = txt[:e]
        variadic = "..." in txt
        name = txt[-1] if txt and txt[-1] not in ("...", "typename", "class") else None
        out.append({"name": name, "kind": tokens_text(part[:-1]) if name else tokens_text(part),
                    "default": default, "variadic": variadic})
    return out


TYPE_WORDS = {"const", "volatile", "unsigned", "signed", "struct", "class", "typename", "enum"}


def parse_params(ts):
    params = []
    if len(ts) == 1 and ts[0].text == "void":
        return params
    for part in split_top(ts, ","):
        if not part:
            continue
        txt = [x.text for x in part]
        default = None
        if "=" in txt:
            e = txt.index("=")
            default = tokens_text(part[e + 1:])
            part = part[:e]
            txt = txt[:e]
        name = None
        typ = part
        if len(part) >= 2 and part[-1].kind == "id" and part[-1].text not in TYPE_WORDS \
                and (part[-2].kind == "id" or part[-2].text in ("*", "&", "&&", ">")):
            name = part[-1].text
            typ = part[:-1]
        # array param  T name[]
        params.append(describe_type(typ, name, default))
    return params


def describe_type(typ, name=None, default=None):
    txt = [x.text for x in typ]
    ptr = txt.count("*")
    ref = "&" in txt or "&&" in txt
    is_const = "const" in txt
    # const that applies to the pointee is the one before the first *
    const_pointee = "const" in txt[: txt.index("*")] if "*" in txt else is_const
    base_toks = []
    depth = 0
    for x in typ:
        if x.text == "<":
            depth += 1
        elif x.text == ">":
            depth -= 1
        if depth == 0 and x.text in ("*", "&", "&&", "const", "volatile", "struct", "class", "typename", "enum"):
            continue
        base_toks.append(x)
    base = tokens_text(base_toks)
    out = {"name": name, "cppType": tokens_text(typ), "base": base, "pointer": ptr}
    if ref:
        out["reference"] = True
    if is_const:
        out["const"] = True if const_pointee else "pointer"
    if default is not None:
        out["default"] = default
    return out


def parse_file(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        src = f.read()
    p = Parser(src, path)
    decls = p.parse()
    return p, decls
