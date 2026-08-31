#!/usr/bin/env python3
"""Build a catalogue bundle (.qcfg.zip) and its index entry.

A bundle is what the web interface's configuration export writes — one
configuration document at the root plus images/<subpath> for every image it
names — but deflated, since a bundle is published and downloaded rather than
kept. The images come from an images root laid out like the board's
($QUNILATOR_DIR/images): the subpaths in the document name the files.

    tools/build-catalog-zip.py 211bsd.qcfg.json --images-root ./images \
        --out 211bsd.qcfg.zip

Prints the index entry for the catalogue's configurations list, with the
bundle's size and sha256 filled in; edit the description and version by hand.
"""

import argparse
import hashlib
import json
import os
import sys
import zipfile


def image_subpath(value):
    """The images-root-relative subpath a config document's image value names
    (the Python twin of webstorage_image_subpath)."""
    if value.startswith("./"):
        value = value[2:]
    if value.startswith("images/"):
        return value[len("images/"):]
    return value


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("document", help="the configuration document (.qcfg.json)")
    ap.add_argument("--images-root", required=True,
                    help="directory laid out like the board's images tree")
    ap.add_argument("--out", help="bundle path (default: <name>.qcfg.zip)")
    ap.add_argument("--name", help="entry name (default: the document's stem)")
    ap.add_argument("--bus", default="qbus", choices=["qbus", "unibus", "any"])
    args = ap.parse_args()

    doc = json.load(open(args.document))
    stem = os.path.basename(args.document)
    for suffix in (".qcfg.json", ".json"):
        if stem.endswith(suffix):
            stem = stem[: -len(suffix)]
            break
    name = args.name or stem
    out = args.out or name + ".qcfg.zip"

    subpaths = []
    for dev in doc.get("devices", []):
        image = dev.get("params", {}).get("image", "")
        if image:
            subpaths.append(image_subpath(image))

    images = []
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(args.document, name + ".qcfg.json")
        for sub in subpaths:
            path = os.path.join(args.images_root, sub)
            if not os.path.exists(path):
                sys.exit("image not found: " + path)
            z.write(path, "images/" + sub)
            images.append({"path": sub, "bytes": os.path.getsize(path)})

    sha = hashlib.sha256()
    with open(out, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            sha.update(chunk)

    # the qunilator-catalog/1 entry shape; the project's own catalogue keeps
    # these as YAML files under docs/site/src/content/configurations/
    entry = {
        "id": name,
        "title": doc.get("title", name),
        "summary": "",
        "bus": args.bus,
        "download": {
            "url": os.path.basename(out),
            "bytes": os.path.getsize(out),
            "sha256": sha.hexdigest(),
        },
        "images": images,
    }
    print(json.dumps(entry, indent=1))


if __name__ == "__main__":
    main()
