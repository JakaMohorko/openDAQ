"""Small C++ tokenizer: enough for openDAQ headers and sources.

Tokens keep their line and source offsets so declarations can be sliced
back out of the text. Comments are kept as tokens (kind 'comment') because
the api tags and RTGen attributes live in them. Preprocessor lines are
kept as one 'pp' token each.
"""

import re
from dataclasses import dataclass


@dataclass
class Tok:
    kind: str  # id, num, str, char, punct, comment, pp
    text: str
    line: int
    start: int
    end: int

    def __repr__(self):
        return f"{self.kind}:{self.text!r}@{self.line}"


_PUNCT3 = ("<<=", ">>=", "...", "->*")
_PUNCT2 = ("::", "->", "<=", ">=", "==", "!=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "%=",
           "&=", "|=", "^=", "<<", ".*")
_ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_NUM_RE = re.compile(r"(0[xX][0-9A-Fa-f']+|0[bB][01']+|\d[\d']*\.?\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?)[uUlLfF]*")


def tokenize(src: str):
    toks = []
    i = 0
    n = len(src)
    line = 1
    at_line_start = True
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            at_line_start = True
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        if c == "\\" and i + 1 < n and src[i + 1] in "\r\n":
            i += 1
            continue
        # preprocessor line (with continuations)
        if c == "#" and at_line_start:
            j = i
            while j < n:
                if src[j] == "\n":
                    if src[j - 1] == "\\" or (src[j - 1] == "\r" and src[j - 2] == "\\"):
                        j += 1
                        continue
                    break
                # comments inside a directive end it for our purposes only if line comment
                j += 1
            text = src[i:j]
            toks.append(Tok("pp", text, line, i, j))
            line += text.count("\n")
            i = j
            continue
        at_line_start = False
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            if j < 0:
                j = n
            toks.append(Tok("comment", src[i:j], line, i, j))
            i = j
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            text = src[i:j]
            toks.append(Tok("comment", text, line, i, j))
            line += text.count("\n")
            i = j
            continue
        # raw string
        if c == "R" and i + 1 < n and src[i + 1] == '"':
            m = re.match(r'R"([^(\s]*)\(', src[i:])
            if m:
                delim = ")" + m.group(1) + '"'
                j = src.find(delim, i + m.end())
                j = n if j < 0 else j + len(delim)
                text = src[i:j]
                toks.append(Tok("str", text, line, i, j))
                line += text.count("\n")
                i = j
                continue
        if c == '"' or c == "'":
            j = i + 1
            while j < n and src[j] != c:
                if src[j] == "\\":
                    j += 1
                if j < n and src[j] == "\n":
                    break
                j += 1
            j += 1
            toks.append(Tok("str" if c == '"' else "char", src[i:j], line, i, j))
            i = j
            continue
        m = _ID_RE.match(src, i)
        if m:
            # string literal prefixes u8"..." L"..."
            if m.group(0) in ("u8", "u", "U", "L") and m.end() < n and src[m.end()] == '"':
                i = m.end()
                continue
            toks.append(Tok("id", m.group(0), line, i, m.end()))
            i = m.end()
            continue
        if c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit()):
            m = _NUM_RE.match(src, i)
            if m and m.end() > i:
                toks.append(Tok("num", m.group(0), line, i, m.end()))
                i = m.end()
                continue
        p3 = src[i:i + 3]
        if p3 in _PUNCT3:
            toks.append(Tok("punct", p3, line, i, i + 3))
            i += 3
            continue
        p2 = src[i:i + 2]
        if p2 in _PUNCT2:
            toks.append(Tok("punct", p2, line, i, i + 2))
            i += 2
            continue
        toks.append(Tok("punct", c, line, i, i + 1))
        i += 1
    return toks
