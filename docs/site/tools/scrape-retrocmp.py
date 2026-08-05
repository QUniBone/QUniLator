#!/usr/bin/env python3
"""Pull the retrocmp UniBone and QBone articles down for migration.

Writes raw HTML to migration/raw/, downloads every image the articles use to
migration/images/, and converts each article body to GitHub-flavoured Markdown
in migration/md/. The Markdown is a starting point for hand-editing into
src/content/docs/ -- it is not published as-is.

Run from the repository root:  python3 tools/scrape-retrocmp.py
"""

import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request

BASE = "https://retrocmp.com"
OUT = "migration"

ARTICLES = [
    # UniBone
    "/projects/unibone/296-unibone-faq",
    "/projects/unibone/274-unibone-introduction",
    "/projects/unibone/281-unibone-as-memory-emulator-user-view",
    "/projects/unibone/282-unibone-as-memory-emulator-internals",
    "/projects/unibone/278-unibone-as-device-test-console",
    "/projects/unibone/275-unibone-as-disk-emulator",
    "/projects/unibone/325-unibone-host-file-sharing",
    "/projects/unibone/273-unibone-hardware",
    "/projects/unibone/284-unibone-software-a-hello-world-device",
    "/projects/unibone/277-unibone-software-theory-of-operation",
    "/projects/unibone/283-unibone-getting-one",
    "/projects/unibone/329-unibone-auto-start",
    "/projects/unibone/286-unibone-building",
    "/projects/unibone/287-unibone-acceptance-test",
    "/projects/unibone/298-unibone-debugging",
    "/projects/unibone/285-unibone-pdp-11-and-unibus",
    "/projects/unibone/354-unibone-blinkenbone-panels",
    "/projects/unibone/356-unibone-blinkenbone-programmers-reference",
    # QBone
    "/projects/qbone/313-qbone-introduction",
    "/projects/qbone/314-qbone-installation",
    "/projects/qbone/321-qbone-building",
    "/projects/qbone/320-qbone-acceptance-test",
    "/projects/qbone/326-qbone-unibone-alternative-bus-drivers",
]

BODY_CLASS = "com-content-article__body"
TAG = re.compile(r"<\s*(/?)div\b", re.I)


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "qunilator.com-migration/1.0"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def extract_body(html):
    """Return the contents of the article body div, balancing nested divs."""
    start = html.find(BODY_CLASS)
    if start < 0:
        return None
    open_tag_end = html.find(">", start)
    depth = 1
    pos = open_tag_end + 1
    for m in TAG.finditer(html, pos):
        depth += -1 if m.group(1) else 1
        if depth == 0:
            return html[open_tag_end + 1 : m.start()]
    return html[open_tag_end + 1 :]


def collect_images(body, images_dir):
    """Download every <img src> and rewrite the reference to a local file name."""
    def repl(m):
        src = m.group(2)
        if src.startswith("data:"):
            return m.group(0)
        absolute = urllib.parse.urljoin(BASE, src)
        name = os.path.basename(urllib.parse.urlparse(absolute).path)
        if not name:
            return m.group(0)
        target = os.path.join(images_dir, name)
        if not os.path.exists(target):
            try:
                with open(target, "wb") as f:
                    f.write(fetch(absolute))
                print(f"      image {name}")
            except Exception as e:  # a missing image must not stop the run
                print(f"      image {name} FAILED: {e}", file=sys.stderr)
                return m.group(0)
        return f'{m.group(1)}images/{name}{m.group(3)}'

    return re.sub(r'(<img[^>]*\bsrc=")([^"]+)(")', repl, body, flags=re.I)


def main():
    raw_dir = os.path.join(OUT, "raw")
    md_dir = os.path.join(OUT, "md")
    images_dir = os.path.join(OUT, "images")
    for d in (raw_dir, md_dir, images_dir):
        os.makedirs(d, exist_ok=True)

    for path in ARTICLES:
        slug = path.rsplit("/", 1)[-1]
        url = BASE + path
        print(f"  {slug}")
        html = fetch(url).decode("utf-8", "replace")
        with open(os.path.join(raw_dir, slug + ".html"), "w") as f:
            f.write(html)

        body = extract_body(html)
        if body is None:
            print(f"      no article body found", file=sys.stderr)
            continue
        body = collect_images(body, images_dir)

        md = subprocess.run(
            ["pandoc", "-f", "html", "-t", "gfm-raw_html", "--wrap=none"],
            input=body, capture_output=True, text=True, check=True,
        ).stdout

        with open(os.path.join(md_dir, slug + ".md"), "w") as f:
            f.write(f"<!-- migrated from {url} -->\n\n{md}")

    print(f"\n{len(ARTICLES)} articles into {md_dir}/")


if __name__ == "__main__":
    main()
