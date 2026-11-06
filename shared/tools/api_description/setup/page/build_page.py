"""Build the setup builder page from the catalog and the API description.

    python setup/page/build_page.py  -> output/setup/opendaq-setup-builder.html
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.normpath(os.path.join(HERE, "..", ".."))


def main():
    with open(os.path.join(TOOL, "output", "setup", "catalog.json"), encoding="utf-8") as f:
        catalog = json.load(f)
    with open(os.path.join(TOOL, "output", "opendaq_api.json"), encoding="utf-8") as f:
        api = json.load(f)
    catalog.pop("source", None)
    data = {
        "catalog": catalog,
        "guids": {i["name"]: i["guid"] for i in api["interfaces"] if i.get("coretype") or i["name"] in ("IStruct", "IEnumeration")},
        "schema": api["setup"]["schema"],
        "rules": api["setup"]["rules"],
        "version": api.get("openDaqVersion"),
        "commit": api.get("source", {}).get("commit"),
    }
    blob = json.dumps(data, separators=(",", ":"), ensure_ascii=False).replace("</", "<\\/")
    with open(os.path.join(HERE, "template.html"), encoding="utf-8") as f:
        html = f.read().replace("/*__DATA__*/null", blob)
    out = os.path.join(TOOL, "output", "setup", "opendaq-setup-builder.html")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    print(out, f"{len(html) / 1024:.0f} KiB")


if __name__ == "__main__":
    main()
