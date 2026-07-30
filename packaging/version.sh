#!/bin/sh
# The version of this tree, from the top stanza of packaging/debian/changelog.
#
# The changelog is authoritative, and three consumers derive from it: the
# package (build-deb.sh), the binary (the makefiles' -DQUNILATOR_VERSION) and
# the web bundle (vite.config.ts). One implementation here keeps them from
# disagreeing - a service that reports a version other than the one the
# installed package carries cannot verify its own update.

set -e
cd "$(dirname "$0")/.."
sed -n 's/^[a-z]* (\([^)]*\)).*/\1/p' packaging/debian/changelog | head -1
