#!/usr/bin/env python3
"""Fetch the QUniBone sample disk images, and give them proper names.

We cannot ship these images ourselves, but they are published at
http://files.retrocmp.com/qunibone/10.03_app_demo/ in a tree of
per-application directories.  This walks that tree, picks up every
compressed image (*.gz), and files each one under its medium in the media
tree - never the directory structure it was published in.

A machine carries one bus, so it gets one set of images.  The server keeps
the examples in three trees - 5_applications for what runs on either bus,
5_applications_u for UNIBUS and 5_applications_q for QBUS - and a board
merges the one that fits it into 5_applications (qunibone-platform.sh).
This fetches the images of exactly that pair, the bus being read from
qunibone-platform.env or build.env unless --bus says otherwise.  It matters
beyond saving a download: both bus trees hold an "rsx11m_4_8_bl70" that is a
different disk, and only one of them belongs on this machine.

The names on the server are a mess: the same kind of image is variously
called .dsk.gz, .img.gz, .rk.gz, .RL2.gz or nothing at all, and a few
images appear in two directories.  Everything that can be recognised is
written out as

    <software>.<disktype>.dsk.gz

against the catalogue at the bottom of this file; anything unrecognised
keeps its name but is at least given the .dsk.gz ending.

Images go where the rest of the system keeps them, one folder per medium:
/var/lib/qunilator/images/dl/rt11v5.5.rl02.dsk.gz. That is what the web
interface serves and what the example command files mount.

Usage: qunilator-fetch-images [options] [<media-tree>]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE_URL = "http://files.retrocmp.com/qunibone/10.03_app_demo/"
MAX_DEPTH = 6
SUFFIX = ".dsk.gz"

COMMON_TREE = "5_applications"           # runs on either bus
BUS_TREES = {"unibus": "_u", "qbus": "_q"}
BUS_NAMES = {"u": "unibus", "unibus": "unibus", "unibone": "unibus",
             "q": "qbus", "qbus": "qbus", "qbone": "qbus",
             "both": "both"}

# Disk/medium types, and the spellings the server uses for them.
TYPE_ALIASES = {
    "rk": "rk05", "rk05": "rk05", "rk06": "rk06", "rk07": "rk07",
    "rl": "rl02", "rl1": "rl01", "rl2": "rl02", "rl01": "rl01", "rl02": "rl02",
    "rx": "rx01", "rx1": "rx01", "rx2": "rx02", "rx01": "rx01", "rx02": "rx02",
    "r54": "rd54", "rd51": "rd51", "rd53": "rd53", "rd54": "rd54",
    "ra70": "ra70", "ra80": "ra80", "ra81": "ra81", "ra82": "ra82",
    "ra92": "ra92", "rs11": "rs11", "rp06": "rp06", "rm03": "rm03",
    "bin": "bin", "tap": "tap", "ptap": "ptap",
}

# Where a medium's images live in the media tree: one folder per medium, named
# by DEC device mnemonic, which is the layout the API documents and the package
# seeds. An image whose medium is not one of these lands at the root, where the
# web interface still lists it.
MEDIA_FOLDERS = {
    "rk05": "dk", "rk06": "dk", "rk07": "dk",
    "rl01": "dl", "rl02": "dl",
    "ra70": "du", "ra80": "du", "ra81": "du", "ra82": "du", "ra92": "du",
    "rd51": "du", "rd53": "du", "rd54": "du",
    "rx01": "rx", "rx02": "rx",
    "rs11": "rf",                       # RF11 controller, RS11 fixed-head disk
    "tap": "mu", "bin": "mu", "ptap": "mu",
}

IMAGES_DIR = "/var/lib/qunilator/images"

# Endings that say "this is a disk image" and nothing about the medium.
GENERIC_EXTS = {"dsk", "img", "image", "disk"}


def read_setting(path: str, key: str) -> str | None:
    """<key>=<value> out of a shell-style settings file."""
    try:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if line.startswith(f"{key}=") and not line.startswith("#"):
                    return line.split("=", 1)[1].strip().strip('"\'')
    except OSError:
        pass
    return None


def detect_bus() -> tuple[str, str]:
    """Which bus this machine is, and what said so.

    A board flashed from the release image has qunibone-platform.env, which
    is what qunibone-platform.sh reads to decide which example tree to merge
    in; a workstation checkout has build.env naming the board it builds for.
    Failing both, the installed binary is named after the bus it drives.
    """
    tree = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for directory in (tree, os.getcwd(), "/root", os.path.expanduser("~")):
        path = os.path.join(directory, "qunibone-platform.env")
        value = (read_setting(path, "QUNILATOR_PLATFORM")
                 or read_setting(path, "QUNILATOR_PLATFORM_SUFFIX"))
        if value:
            bus = "qbus" if value.lower().lstrip("_").startswith("q") else "unibus"
            return bus, path
    for directory in (tree, os.getcwd()):
        path = os.path.join(directory, "build.env")
        value = read_setting(path, "QUNILATOR_BUS")
        if value and value.lower() in BUS_NAMES:
            return BUS_NAMES[value.lower()], path
    for binary, bus in (("/usr/bin/unibone", "unibus"), ("/usr/bin/qbone", "qbus")):
        if os.path.exists(binary):
            return bus, binary
    raise SystemExit(
        "cannot tell which bus this machine is: no qunibone-platform.env, no\n"
        "build.env with QUNILATOR_BUS, and neither /usr/bin/unibone nor\n"
        "/usr/bin/qbone is installed. Say so with --bus unibus|qbus.")


def trees_for(bus: str) -> list[str]:
    """The server directories whose images belong on this machine."""
    if bus == "both":
        return [COMMON_TREE] + [COMMON_TREE + s for s in BUS_TREES.values()]
    return [COMMON_TREE, COMMON_TREE + BUS_TREES[bus]]


class Source:
    """One *.gz found on the server."""

    def __init__(self, url: str, relpath: str):
        self.url = url
        self.relpath = relpath           # path below the base URL
        self.filename = relpath.rsplit("/", 1)[-1]
        self.dirname = relpath.rsplit("/", 1)[0] if "/" in relpath else ""
        self.leafdir = self.dirname.rsplit("/", 1)[-1]
        self.target = self.filename      # filled in by plan()
        self._size = None
        self._fingerprint = None

    @property
    def subpath(self) -> str:
        """Its place in the media tree: <medium folder>/<name>."""
        return subpath_of(self.target)

    def __repr__(self):
        return f"<Source {self.relpath}>"


# ---------------------------------------------------------------- HTTP

class Fetcher:
    def __init__(self, retries: int = 3, timeout: int = 60):
        self.retries = retries
        self.timeout = timeout

    def _open(self, url: str, method: str = "GET", headers: dict | None = None):
        last = None
        for attempt in range(self.retries):
            try:
                req = urllib.request.Request(url, method=method,
                                             headers=headers or {})
                return urllib.request.urlopen(req, timeout=self.timeout)
            except (urllib.error.URLError, OSError) as exc:
                last = exc
                if isinstance(exc, urllib.error.HTTPError) and exc.code < 500:
                    break                       # 404 and friends will not heal
                time.sleep(2 * (attempt + 1))
        raise last

    def text(self, url: str) -> str:
        with self._open(url) as r:
            return r.read().decode("utf-8", "replace")

    def size(self, url: str) -> int:
        """Content length, or -1 when the server will not say."""
        try:
            with self._open(url, method="HEAD") as r:
                return int(r.headers.get("Content-Length", -1))
        except (urllib.error.URLError, OSError, ValueError):
            return -1

    def head_bytes(self, url: str, count: int = 65536) -> bytes:
        with self._open(url, headers={"Range": f"bytes=0-{count - 1}"}) as r:
            return r.read(count)

    def download(self, url: str, dest: str, progress) -> None:
        tmp = dest + ".part"
        with self._open(url) as r:
            total = int(r.headers.get("Content-Length", -1))
            done = 0
            with open(tmp, "wb") as fh:
                while True:
                    chunk = r.read(256 * 1024)
                    if not chunk:
                        break
                    fh.write(chunk)
                    done += len(chunk)
                    progress(done, total)
        os.replace(tmp, dest)


# ------------------------------------------------------------- scanning

HREF_RE = re.compile(r'href="([^"]+)"', re.I)


def scan(fetcher: Fetcher, base: str, pattern: re.Pattern, max_depth: int,
         url: str | None = None, relpath: str = "", depth: int = 1) -> list[Source]:
    """Walk the Apache index below base, collecting matching files."""
    url = url or base
    if depth > max_depth:
        print(f"depth limit reached, not descending into {url}", file=sys.stderr)
        return []
    found = []
    for href in HREF_RE.findall(fetcher.text(url)):
        if (href.startswith(("?", "/", "#")) or href.startswith("../")
                or re.match(r"[a-z][a-z0-9+.-]*:", href, re.I)):
            continue                            # sort links, parent, absolutes
        # A href is URL-encoded; '+' is a literal plus in a path, not a space.
        name = urllib.parse.unquote(href.rstrip("/"))
        if href.endswith("/"):
            found += scan(fetcher, base, pattern, max_depth,
                          url + href, f"{relpath}{name}/", depth + 1)
        elif pattern.match(name):
            found.append(Source(url + href, relpath + name))
    return found


# ------------------------------------------------------------- naming

def parse_catalogue(text: str) -> dict[str, dict[str, str]]:
    """<software> -> {<disktype>: <full name>} for every catalogue entry."""
    catalogue: dict[str, dict[str, str]] = {}
    for line in text.splitlines():
        name = line.strip()
        if not name or name.startswith("#"):
            continue
        stem = name[: -len(SUFFIX)] if name.endswith(SUFFIX) else name
        software, _, disktype = stem.rpartition(".")
        if not software:
            software, disktype = stem, ""
        catalogue.setdefault(software.lower(), {})[disktype.lower()] = name
    return catalogue


def split_name(filename: str) -> tuple[str, str | None]:
    """Split a server name into (software, disktype-hint)."""
    stem = filename[:-3] if filename.lower().endswith(".gz") else filename
    head, _, last = stem.rpartition(".")
    if head and last.lower() in GENERIC_EXTS:
        # ".dsk" says nothing; look at what is in front of it
        stem = head
        head, _, last = stem.rpartition(".")
    if head and last.lower() in TYPE_ALIASES:
        return head, TYPE_ALIASES[last.lower()]
    return stem, None


def subpath_of(target: str) -> str:
    """Where <target> belongs in the media tree, as <folder>/<name>.

    Takes the compressed name and the expanded one, since an example names the
    image it mounts and the fetcher names the ".gz" it brings down.
    """
    stem = target[:-3] if target.lower().endswith(".gz") else target
    stem = stem[:-4] if stem.lower().endswith(".dsk") else stem
    _, _, disktype = stem.rpartition(".")
    folder = MEDIA_FOLDERS.get(disktype.lower(), "")
    return f"{folder}/{target}" if folder else target


def ensure_suffix(filename: str) -> str:
    """Whatever else it is, it ends in .dsk.gz."""
    stem = filename[:-3] if filename.lower().endswith(".gz") else filename
    if stem.lower().endswith(".dsk"):
        stem = stem[:-4]
    return stem + SUFFIX


def plan(sources: list[Source], catalogue: dict[str, dict[str, str]],
         overrides: dict[str, str]) -> None:
    """Give every source its final name.

    Two passes, so that the outcome does not depend on the order the
    directories were walked in: a file naming its disk type takes that
    catalogue entry first, and one that does not gets what is left over
    (root.rd54.gz claims root.rd54.dsk.gz, so root.dsk.gz is the RX01).
    """
    claimed: dict[str, str] = {}                # catalogue name -> filename
    deferred: list[tuple[Source, dict[str, str]]] = []

    for src in sources:
        if src.relpath in overrides:            # decided by hand, see below
            src.target = overrides[src.relpath]
            claimed[src.target] = src.filename
            continue
        software, hint = split_name(src.filename)
        entries = catalogue.get(software.lower(), {})
        if hint and hint in entries:
            src.target = entries[hint]
            claimed[src.target] = src.filename
        elif entries:
            deferred.append((src, entries))
        else:
            src.target = ensure_suffix(src.filename)

    for src, entries in deferred:
        # Nothing free and one candidate: two copies of one image, which is
        # what the duplicate check downstream is for.
        free = {dt: n for dt, n in entries.items()
                if claimed.get(n, src.filename) == src.filename} or entries
        # An example directory is named after its medium - lsx.rx01, rt11.rl02
        # - which is the only thing left saying that the "root.dsk.gz" in
        # lsx.rx01/ is the RX01 root floppy and not the RD54 of another set.
        dirhint = TYPE_ALIASES.get(src.leafdir.rsplit(".", 1)[-1].lower(), "")
        src.target = free.get(dirhint) or free[sorted(free)[0]]
        claimed[src.target] = src.filename


def resolve_collisions(sources: list[Source], fetcher: Fetcher, log) -> list[Source]:
    """Drop duplicate copies; keep files that only look like duplicates apart."""
    kept: dict[str, Source] = {}
    out = []
    for src in sources:
        first = kept.get(src.target)
        if first is None:
            kept[src.target] = src
            out.append(src)
            continue
        if fingerprint(fetcher, src) == fingerprint(fetcher, first):
            log(f"  = {src.target} (same file, also in {first.dirname})")
            continue
        # Different images under one name and no override to tell them apart.
        software, _, rest = src.target.partition(".")
        src.target = f"{software}_{src.leafdir}.{rest}"
        print(f"warning: {src.relpath} and {first.relpath} both want "
              f"{first.target}; keeping this one as {src.target}", file=sys.stderr)
        kept[src.target] = src
        out.append(src)
    return out


def fingerprint(fetcher: Fetcher, src: Source) -> tuple[int, str]:
    """Length plus a hash of the first 64K - enough to tell two images apart."""
    if src._fingerprint is None:
        head = fetcher.head_bytes(src.url)
        src._fingerprint = (size_of(fetcher, src), hashlib.sha256(head).hexdigest())
    return src._fingerprint


def size_of(fetcher: Fetcher, src: Source) -> int:
    if src._size is None:
        src._size = fetcher.size(src.url)
    return src._size


# ---------------------------------------------------------------- main

def find_legacy(target_dir: str, src: Source, want: int) -> str | None:
    """A file already in the target that is this image under an older name.

    An earlier run may have left it at the root of the tree rather than in its
    medium's folder, or under the server's own name, or under that name with
    the source directory glued in front, which is how clashes used to be kept
    apart.  Same length means the same image; moving beats downloading it.
    """
    if want <= 0:
        return None
    for old in (src.target, src.filename, f"{src.leafdir}_{src.filename}",
                f"{subpath_of(src.filename)}"):
        path = os.path.join(target_dir, old)
        if (old != src.subpath and os.path.isfile(path)
                and os.path.getsize(path) == want):
            return old
    return None


def human(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Download the QUniBone sample disk images into the media "
                    "tree, renamed to <software>.<disktype>.dsk.gz and filed "
                    "under the folder for their medium.")
    ap.add_argument("target", nargs="?", default=IMAGES_DIR,
                    help="media tree to write into (default: %(default)s)")
    ap.add_argument("--bus", choices=sorted(set(BUS_NAMES)), metavar="BUS",
                    help="which machine these images are for: unibus (u) or "
                         "qbus (q), fetching 5_applications and the tree for "
                         "that bus. Detected from qunibone-platform.env or "
                         "build.env when not given. \"both\" takes all three "
                         "trees, which no single machine wants.")
    ap.add_argument("-b", "--base-url", default=BASE_URL, help="URL to scan")
    ap.add_argument("-p", "--pattern", default=r".*\.gz$",
                    help="regex a file name must match (default: %(default)s)")
    ap.add_argument("-d", "--max-depth", type=int, default=MAX_DEPTH,
                    help="how deep to descend (default: %(default)s)")
    ap.add_argument("-n", "--dry-run", action="store_true",
                    help="list what would be fetched, write nothing")
    ap.add_argument("-f", "--force", action="store_true",
                    help="re-download files that are already there")
    ap.add_argument("-q", "--quiet", action="store_true", help="no per-file output")
    ap.add_argument("--names", metavar="FILE",
                    help="read the name catalogue from FILE instead of the "
                         "built-in one (one <software>.<disktype>.dsk.gz per line)")
    ap.add_argument("--keep-server-names", action="store_true",
                    help="do not rename; only enforce the .dsk.gz ending")
    args = ap.parse_args()

    # The media tree is a board's, and creating one somewhere else is how an
    # operator ends up with images the service cannot see.
    if args.target == IMAGES_DIR and not os.path.isdir(IMAGES_DIR):
        print(f"{IMAGES_DIR} is not here - this is where a board keeps its\n"
              "images, and the package creates it. Name a directory to write "
              "somewhere else.", file=sys.stderr)
        return 1

    base = args.base_url if args.base_url.endswith("/") else args.base_url + "/"
    pattern = re.compile(args.pattern, re.I)
    log = (lambda *a: None) if args.quiet else print
    fetcher = Fetcher()

    catalogue = {}
    if not args.keep_server_names:
        text = open(args.names).read() if args.names else CATALOGUE
        catalogue = parse_catalogue(text)

    if args.bus:
        bus, said_so = BUS_NAMES[args.bus], "--bus"
    else:
        bus, said_so = detect_bus()
    trees = trees_for(bus)
    print(f"Fetching the {bus.upper()} images ({', '.join(trees)}) "
          f"from {base}\n  bus from {said_so}")

    sources, missing = [], []
    for tree in trees:
        try:
            sources += scan(fetcher, base, pattern, args.max_depth,
                            url=f"{base}{tree}/", relpath=f"{tree}/")
        except (urllib.error.URLError, OSError) as exc:
            missing.append(f"{tree} ({exc})")
    if missing and not sources:
        # not the QUniBone layout at all: take the URL as given
        print(f"no example trees below {base}, scanning it whole", file=sys.stderr)
        try:
            sources = scan(fetcher, base, pattern, args.max_depth)
        except (urllib.error.URLError, OSError) as exc:
            print(f"cannot read {base}: {exc}", file=sys.stderr)
            return 1
    elif missing:
        print(f"no such tree, skipped: {', '.join(missing)}", file=sys.stderr)
    if not sources:
        print(f"nothing matching {args.pattern!r} below {base}", file=sys.stderr)
        return 1
    print(f"Found {len(sources)} file(s).")

    overrides = dict(OVERRIDES)
    if bus == "both":
        overrides.update(OVERRIDES_BOTH)
    sources.sort(key=lambda s: s.relpath)
    plan(sources, catalogue, {} if args.keep_server_names else overrides)
    sources = resolve_collisions(sources, fetcher, log)

    if args.dry_run:
        for src in sources:
            mark = " " if src.target == src.filename else "*"
            print(f"  {mark} {src.relpath}\n      -> {src.subpath}")
        print(f"Dry run: {len(sources)} file(s), nothing written to {args.target}.")
        return 0

    os.makedirs(args.target, exist_ok=True)
    counts = dict(new=0, kept=0, renamed=0, failed=0)

    for src in sources:
        dest = os.path.join(args.target, src.subpath)
        want = size_of(fetcher, src)
        have = os.path.getsize(dest) if os.path.isfile(dest) else -1

        if not args.force and want > 0 and have == want:
            log(f"  = {src.subpath} (already here)")
            counts["kept"] += 1
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)

        old = None if args.force or have >= 0 else find_legacy(args.target, src, want)
        if old:
            os.replace(os.path.join(args.target, old), dest)
            log(f"  > {old} -> {src.subpath} (already here, moved)")
            counts["renamed"] += 1
            continue

        log(f"  + {src.subpath} ({human(want)}) <- {src.relpath}")
        show = sys.stdout.isatty() and not args.quiet

        def progress(done, total, name=src.subpath):
            if show and total > 0:
                sys.stdout.write(f"\r    {name}: {100 * done // total:3d}%")
                sys.stdout.flush()

        try:
            fetcher.download(src.url, dest, progress)
            counts["new"] += 1
        except (urllib.error.URLError, OSError) as exc:
            print(f"\nfailed to download {src.url}: {exc}", file=sys.stderr)
            counts["failed"] += 1
            if os.path.exists(dest + ".part"):
                os.unlink(dest + ".part")
        finally:
            if show:
                sys.stdout.write("\r\033[K")

    print(f"Done: {counts['new']} downloaded, {counts['kept']} already present, "
          f"{counts['renamed']} moved into place, {counts['failed']} failed "
          f"-> {args.target}")

    # Whatever else is in the tree is the operator's: their own images, the
    # sample pack, the overlays a drive writes. Said once, so a name that was
    # expected to be replaced and was not is visible.
    here = set()
    for root, _, files in os.walk(args.target):
        rel = os.path.relpath(root, args.target)
        here |= {f if rel == "." else f"{rel}/{f}" for f in files}
    strays = sorted(here - {s.subpath for s in sources})
    if strays and not args.quiet:
        print(f"{len(strays)} other file(s) under {args.target}, left alone: "
              + ", ".join(strays[:5]) + (", ..." if len(strays) > 5 else ""))
    return 1 if counts["failed"] else 0


# --------------------------------------------------------------- the names

# Images the catalogue cannot place, keyed by their path below the base URL.
OVERRIDES = {
    # One tree, two builds of one image: the RK05 in mini-unix.rk05/ belongs
    # to mini-unix_dk0_05.sh, the one in cpu/ to cpu20_mini-unix_dk0.sh. The
    # suffix is the server's own <OS>_<device>_<pdp11-version> scheme.
    "5_applications_u/mini-unix.rk05/mini-unix-tape1.rk05.gz":
        "mini-unix-tape1_05.rk05.dsk.gz",
    "5_applications_u/cpu/mini-unix-tape1.rk05.gz":
        "mini-unix-tape1_20.rk05.dsk.gz",
    # the Q tree's rsxm.dsk.gz is the file its own .cmd calls rsxm70.dsk
    "5_applications_q/rsx11.mscp/rsxm.dsk.gz": "rsxm70.ra70.dsk.gz",
}

# Only for --bus both, where one directory has to hold both bus trees at once.
# Each of these names a disk that exists in both, with different content, so
# the two cannot share the catalogue name a single-bus run gives them: the
# suffix is the CPU of the example that mounts it (rsx11m4.8_du0+rl_23_73.sh
# in the Q tree, rsx11m4.8_du0+rl_34.sh in the U tree).
OVERRIDES_BOTH = {
    "5_applications_q/rsx11.mscp/rsx11m_4_8_bl70.dsk.gz":
        "rsx11m_4_8_bl70_73.ra70.dsk.gz",
    "5_applications_u/rsx11.mscp/rsx11m_4_8_bl70.dsk.gz":
        "rsx11m_4_8_bl70_34.ra70.dsk.gz",
    "5_applications_q/rsx11.mscp/rsxm.dsk.gz": "rsxm70_73.ra70.dsk.gz",
    "5_applications_u/rsx11.mscp/rsxm70.dsk.gz": "rsxm70_34.ra70.dsk.gz",
}

# Every name we know, as <software>.<disktype>.dsk.gz.  A downloaded file is
# matched to one of these on its software part, and on its disk type where the
# server bothered to give one.  Adding a line here is enough to name a file
# the server publishes later.
CATALOGUE = """
2.11BSD_44.ra92.dsk.gz
cc.rx01.dsk.gz
dl0.rl02.dsk.gz
JH_DU0.ra70.dsk.gz
JH_DU1.ra70.dsk.gz
mini-unix-tape0.bin.dsk.gz
mini-unix-tape1_05.rk05.dsk.gz
mini-unix-tape1_20.rk05.dsk.gz
mini-unix-tape2.rk05.dsk.gz
mini-unix-tape3.rk05.dsk.gz
NanoVMS044-1M.rl02.dsk.gz
quas.ra82.dsk.gz
root.rd54.dsk.gz
root.rx01.dsk.gz
rsx11m4.1_excprv.rl02.dsk.gz
rsx11m4.1_hlpdcl.rl02.dsk.gz
rsx11m4.1_sys_34.rl02.dsk.gz
rsx11m4.1_user.rl02.dsk.gz
rsx11m_4_8_bl70.ra70.dsk.gz
rsx11mp46-rl02pg.rl02.dsk.gz
rsx11mpbl87.ra70.dsk.gz
rsx11mpv4.6_du0_84.ra80.dsk.gz
rsx11mpv4.6_du1_84.ra80.dsk.gz
rsxdl1.rl02.dsk.gz
rsxdl2.rl02.dsk.gz
rsxdl3.rl02.dsk.gz
rsxm70.ra70.dsk.gz
rsxm70.rl02.dsk.gz
RT11.rx02.dsk.gz
rt11v03-1.rx01.dsk.gz
rt11v03-2.rx01.dsk.gz
rt11v03-3.rx01.dsk.gz
rt11v03-4.rx01.dsk.gz
rt11v03-5.rx01.dsk.gz
rt11v03-6.rx01.dsk.gz
rt11v03-7.rx01.dsk.gz
rt11v03-8.rx01.dsk.gz
rt11v03-9.rx01.dsk.gz
rt11v03b-1.rx01.dsk.gz
rt11v03b-1-c.rx01.dsk.gz
rt11v03b-2.rx01.dsk.gz
rt11v03b-2-c.rx01.dsk.gz
rt11v03b-3.rx01.dsk.gz
rt11v03b-3-c.rx01.dsk.gz
rt11v03b-4.rx01.dsk.gz
rt11v03b-4-c.rx01.dsk.gz
rt11v03b-5.rx01.dsk.gz
rt11v03b-5-c.rx01.dsk.gz
rt11v03b-6.rx01.dsk.gz
rt11v03b-6-c.rx01.dsk.gz
rt11v03b-7-c.rx01.dsk.gz
rt11v03b-8.rx01.dsk.gz
rt11v03b-8-c.rx01.dsk.gz
rt11v5.5.rl02.dsk.gz
rt11v5.5_34.ra80.dsk.gz
rt11v5.5_games.rl02.dsk.gz
rt11v5.5_games_34.rl02.dsk.gz
RTRKV4.00.rk05.dsk.gz
si400.rx01.dsk.gz
si400-bak.rx01.dsk.gz
srcam.rx01.dsk.gz
srcnz.rx01.dsk.gz
ssp-2.rx01.dsk.gz
ssp-ex.rx01.dsk.gz
unix_v6.rl02.dsk.gz
unixv1_rk0.rk05.dsk.gz
unixv1_rs0.rs11.dsk.gz
unixv6.rl02.dsk.gz
unixv6-with-sources.rl02.dsk.gz
usr.rx01.dsk.gz
v6bin.rk05.dsk.gz
v6doc.rk05.dsk.gz
v6src.rk05.dsk.gz
vms073.rd54.dsk.gz
xxdp+.rl02.dsk.gz
xxdp22.rl02.dsk.gz
xxdp25.rl02.dsk.gz
"""

if __name__ == "__main__":
    sys.exit(main())
