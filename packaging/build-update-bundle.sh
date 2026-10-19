#!/bin/sh
# Build the update file for a package the apt repository already carries.
#
#   packaging/build-update-bundle.sh <repo-url> <pkg>_<version>_armhf.deb...
#
# An update file is what a board with no way to the repository installs from:
# <pkg>_<version>_armhf.update.tar, holding the repository's signed InRelease,
# its Packages index and the package. The board checks the signature against the
# keyring it already carries, the index against the signature and the package
# against the index (qunilator-update --stage-local), so the file needs no
# signature of its own. It is written beside each package.
#
# The index is fetched from the repository after the release has been published
# there, and has to list the package with the hash of the file given here - a
# repository that has not rebuilt its index yet is waited for, up to a few
# minutes.

set -eu
# no AppleDouble members when this runs on a Mac
export COPYFILE_DISABLE=1

[ $# -ge 2 ] || { sed -n '4p' "$0" | sed 's/^# *//' >&2; exit 2; }
REPO=${1%/}
shift

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

for deb in "$@"; do
    name=$(basename "$deb")
    pkg=${name%%_*}
    ver=${name#"${pkg}"_}
    ver=${ver%_armhf.deb}
    want=$(sha256 "$deb")

    tries=0
    while :; do
        curl -fsS -o "$work/InRelease" "$REPO/dists/trixie/InRelease"
        curl -fsS -o "$work/Packages" "$REPO/dists/trixie/main/binary-armhf/Packages"
        listed=$(awk -v p="$pkg" -v v="$ver" '
            /^$/ {if (pk == p && vr == v) {print sh; exit} pk = vr = sh = ""}
            /^Package: / {pk = $2} /^Version: / {vr = $2} /^SHA256: / {sh = $2}
            END {if (pk == p && vr == v) print sh}' "$work/Packages" | head -1)
        index=$(sha256 "$work/Packages")
        signed=$(awk '/^SHA256:/ {f = 1; next} /^[A-Za-z]/ {f = 0}
            f && $3 == "main/binary-armhf/Packages" {print $1}' "$work/InRelease")
        [ "$listed" = "$want" ] && [ "$index" = "$signed" ] && break
        tries=$((tries + 1))
        if [ "$tries" -ge 30 ]; then
            echo "build-update-bundle: $REPO does not list $name with its hash" >&2
            exit 1
        fi
        sleep 10
    done

    cp "$deb" "$work/$name"
    out="$(dirname "$deb")/${name%.deb}.update.tar"
    # plain files at the top of the archive, owned by nobody in particular
    tar -C "$work" -cf "$out" --owner=0 --group=0 InRelease Packages "$name" 2>/dev/null \
        || tar -C "$work" -cf "$out" InRelease Packages "$name"
    rm -f "$work/$name"
    echo "$out"
done
