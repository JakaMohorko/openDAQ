"""Build the classification review page from config/classification.json.

    python review/build_review.py  -> output/review/api-audience-review.html

The page embeds the entries and the tagged headers, lets a reviewer go
through the headers file by file or filter the declarations, change
audiences and labels, and mark files as reviewed. Decisions and reviewed
files are kept in the artifact's db (collections `decisions` and `files`)
when published, or in browser storage when opened as a local file.
Decisions come back through annotate.py --apply <export.json>.
"""

import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
REPO = os.path.normpath(os.path.join(TOOL, "..", "..", ".."))
TAG = re.compile(r"^\s*//\s*\[api\(([^)]*)\)\]\s*$")
SNIPPET_MAX = 4

KEEP = ("key", "kind", "audience", "labels", "reason", "where", "usedBy", "evidence", "interface", "interfaceAudience",
        "brief", "class", "virtual", "type", "returns", "name", "exported", "implementation", "derivedBy", "library",
        "group", "access", "methods", "tagged")


def rules_text():
    """The rules classify.py applies, for the page's rules panel."""
    src = open(os.path.join(TOOL, "classify.py"), encoding="utf-8").read()
    m = re.search(r"CORETYPE_INTERFACES = \{(.*?)\}", src, re.S)
    return {"coretypes": re.findall(r'"ct\w+":\s*"(\w+)"', m.group(1))}


def declaration_end(lines, i):
    """Index past the last line of the declaration starting at line index i, at most SNIPPET_MAX lines."""
    for j in range(i, min(i + SNIPPET_MAX, len(lines))):
        s = lines[j].strip()
        if ";" in s or "{" in s or s.endswith(")"):
            return j + 1
    return min(i + SNIPPET_MAX, len(lines))


def snippet(lines, start, end):
    """Source lines start..end-1 with their common indentation removed."""
    chunk = [lines[j].rstrip() for j in range(start, end)]
    indent = min((len(x) - len(x.lstrip()) for x in chunk if x.strip()), default=0)
    return "\n".join(x[indent:] for x in chunk)


def file_items(lines, entry_lines):
    """Tag sites and listed declarations of one header in source order.

    An item is a tag with the declaration below it, or a listed declaration
    without a tag; `e` holds the indices of the entries it covers.
    """
    items, covered = [], set()
    for t, text in enumerate(lines):
        m = TAG.match(text)
        if not m:
            continue
        d = t + 1
        while d < len(lines) and (not lines[d].strip() or lines[d].strip().startswith("//")):
            d += 1
        end = declaration_end(lines, d)
        span = range(d + 1, end + 1)
        idx = [i for ln in span for i in entry_lines.get(ln, [])]
        covered.update(ln for ln in span if ln in entry_lines)
        tag = ", ".join(x.strip() for x in m.group(1).split(","))
        items.append({"l": t + 1, "t": tag, "c": snippet(lines, t, end), "e": idx})
    for ln, idx in entry_lines.items():
        if ln in covered or ln > len(lines):
            continue
        items.append({"l": ln, "t": None, "c": snippet(lines, ln - 1, declaration_end(lines, ln - 1)), "e": idx})
    items.sort(key=lambda x: x["l"])
    return items


def files_view(cls_entries):
    """Every header that carries a tag or a listed declaration, with its items."""
    by_file = {}
    for i, e in enumerate(cls_entries):
        path, line = e["where"].rsplit(":", 1)
        by_file.setdefault(path, {}).setdefault(int(line), []).append(i)
    tagged = subprocess.run(["git", "-C", REPO, "grep", "-l", "-E", r"^\s*//\s*\[api\(", "--", "*.h"],
                            capture_output=True, text=True, encoding="utf-8").stdout.split()
    files = []
    for path in sorted(set(by_file) | set(tagged)):
        with open(os.path.join(REPO, path), encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        files.append({"p": path, "items": file_items(lines, by_file.get(path, {}))})
    return files


def repo_url():
    """The GitHub page of the remote the branch is pushed to (upstream remote, else origin), for file links."""
    git = lambda *a: subprocess.run(["git", "-C", REPO, *a], capture_output=True, text=True).stdout.strip()
    remote = git("rev-parse", "--abbrev-ref", "@{u}").split("/")[0] or "origin"
    url = git("remote", "get-url", remote)
    url = re.sub(r"^git@github\.com:", "https://github.com/", url)
    return re.sub(r"\.git$", "", url) if url.startswith("https://github.com/") else ""


def main():
    cls = json.load(open(os.path.join(TOOL, "config", "classification.json"), encoding="utf-8"))
    api = json.load(open(os.path.join(TOOL, "output", "opendaq_api.json"), encoding="utf-8"))
    entries = []
    for e in cls["entries"]:
        x = {k: e[k] for k in KEEP if k in e and e[k] not in (None, [], {}, "")}
        if "where" in x:
            x["where"] = x["where"].replace("core/", "", 1)
        entries.append(x)
    head = subprocess.run(["git", "-C", REPO, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    data = {
        "commit": cls.get("commit"),
        "head": head,
        "repoUrl": repo_url(),
        "openDaqVersion": api.get("openDaqVersion"),
        "rules": rules_text(),
        "entries": entries,
        "files": files_view(cls["entries"]),
    }
    blob = json.dumps(data, separators=(",", ":"), ensure_ascii=False).replace("</", "<\\/")
    tpl = open(os.path.join(HERE, "template.html"), encoding="utf-8").read()
    html = tpl.replace("/*__DATA__*/null", blob)
    out = os.path.join(TOOL, "output", "review", "api-audience-review.html")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    files = data["files"]
    tags = sum(1 for f in files for it in f["items"] if it["t"])
    loose = sum(1 for f in files for it in f["items"] if it["t"] and not it["e"])
    print(out, f"{len(html) / 1e6:.2f} MB, {len(entries)} entries, {len(files)} files, {tags} tags ({loose} without a listed declaration)")


if __name__ == "__main__":
    sys.exit(main())
