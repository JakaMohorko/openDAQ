"""Build the classification review page from config/classification.json.

    python review/build_review.py  -> output/review/api-audience-review.html

The page embeds the entries, lets a reviewer change audiences and labels,
and keeps the decisions in the artifact's db (collection `decisions`) when
published, or in browser storage when opened as a local file. Decisions
come back through annotate.py --apply <export.json>.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)

KEEP = ("key", "kind", "audience", "labels", "reason", "where", "usedBy", "evidence", "interface", "interfaceAudience",
        "brief", "class", "virtual", "type", "returns", "name", "exported", "implementation", "derivedBy", "library",
        "group", "access", "methods", "tagged")


def rules_text():
    """The rules classify.py applies, for the page's rules panel."""
    src = open(os.path.join(TOOL, "classify.py"), encoding="utf-8").read()
    m = re.search(r"CORETYPES = \{(.*?)\}", src, re.S)
    coretypes = sorted(x.strip().strip('"') for x in m.group(1).replace("\n", " ").split(",") if x.strip())
    return {"coretypes": coretypes}


def main():
    cls = json.load(open(os.path.join(TOOL, "config", "classification.json"), encoding="utf-8"))
    api = json.load(open(os.path.join(TOOL, "output", "opendaq_api.json"), encoding="utf-8"))
    entries = []
    for e in cls["entries"]:
        x = {k: e[k] for k in KEEP if k in e and e[k] not in (None, [], {}, "")}
        if "where" in x:
            x["where"] = x["where"].replace("core/", "", 1)
        entries.append(x)
    data = {
        "commit": cls.get("commit"),
        "openDaqVersion": api.get("openDaqVersion"),
        "rules": rules_text(),
        "entries": entries,
    }
    blob = json.dumps(data, separators=(",", ":"), ensure_ascii=False).replace("</", "<\\/")
    tpl = open(os.path.join(HERE, "template.html"), encoding="utf-8").read()
    html = tpl.replace("/*__DATA__*/null", blob)
    out = os.path.join(TOOL, "output", "review", "api-audience-review.html")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    print(out, f"{len(html) / 1e6:.2f} MB, {len(entries)} entries")


if __name__ == "__main__":
    sys.exit(main())
