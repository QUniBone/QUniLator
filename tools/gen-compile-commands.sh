#!/bin/bash
#
# gen-compile-commands.sh: write compile_commands.json for the editor
#
# clangd needs the include paths and defines each source is really compiled
# with. Without a compilation database it falls back to whatever .clangd says,
# and resolves the relative -I paths there against the directory of the file it
# is parsing rather than against the repository - so a header two directories up
# is reported missing, and every type in it as unknown.
#
# The flags come from the makefiles themselves, by asking make what it would do
# and reading the compile lines back, so nothing here has to be kept in step
# with them by hand.
#
#	./tools/gen-compile-commands.sh
#
# Two things are rewritten on the way through. The compiler becomes the host's,
# because clangd parses with it and the ARM cross toolchain is not installed on
# a workstation; and the container's /qunibone becomes this checkout, because
# the makefiles are asked their questions inside the crossbuild image, which is
# where the cross toolchain lives.
#
# compile_commands.json is generated and is not committed. Run this after
# changing an include path or adding a directory of sources.

set -e
cd "$(dirname "$0")/.."
root=$PWD

image=$(docker images --format '{{.Repository}}:{{.Tag}}' | grep '^qunibone-crossbuild:' | head -1)
if [ -z "$image" ]; then
    echo "$0: the crossbuild image is not built - run ./crossbuild.sh once" >&2
    exit 2
fi

# -B so make prints every compile and not only what is out of date, and -o for
# the PRU firmware so it is taken as already made: it is built by its own
# toolchain in another container, and asking for it here would only fail.
ask_make () {                                   # ask_make <suffix> <bus> <platform>
    local deploy=/qunibone/10.01_base/4_deploy$1

    docker run --rm --user "$(id -u):$(id -g)" -v "$root:/qunibone" \
        -w /qunibone/10.03_app_demo/2_src "$image" \
        make -f "makefile$1" -B -n \
            QUNILATOR_DIR=/qunibone \
            MAKE_CONFIGURATION=RELEASE \
            MAKE_TARGET_ARCH=BBB \
            BBB_CC=arm-linux-gnueabihf-gcc \
            QUNILATOR_PLATFORM="$3" \
            -o "$deploy/pru0_code_all_array.c" \
            -o "$deploy/pru1_code_$2_array.c" \
            -o "$deploy/pru1_code_test_array.c" \
            -o "$deploy/pru1_code_${2}int_array.c" 2>/dev/null || true
}

# The generator reads make's output on stdin, so it cannot also be fed to
# python on stdin: it is written out first and run from there.
generator=$(mktemp)
trap 'rm -f "$generator"' EXIT
cat >"$generator" <<'GENERATOR'
import json, shlex, sys, os

root = sys.argv[1]
entries = {}

for line in sys.stdin:
    line = line.strip()
    if ' -c ' not in line and not line.endswith(' -c'):
        continue
    if not line.split(' ', 1)[0].endswith(('gcc', 'g++', 'cc', 'c++', 'clang', 'clang++')):
        continue
    try:
        words = shlex.split(line)
    except ValueError:
        continue

    sources = [w for w in words
               if w.endswith(('.c', '.cpp')) and not w.startswith('-')]
    if len(sources) != 1:
        continue                                # a link, or a multi-file compile
    source = sources[0]

    # The host's compiler parses these, so clangd finds a standard library
    # whatever the build itself compiles with.
    words[0] = 'c++' if source.endswith('.cpp') else 'cc'

    # A cross build's own flags describe a toolchain clangd does not have.
    drop_exact = {'-fmax-errors=3', '-no-pie'}
    words = [w for w in words if w not in drop_exact]

    words = [w.replace('/qunibone', root) for w in words]
    source = source.replace('/qunibone', root)

    # A relative source name is relative to the makefile that named it, and
    # only the application's makefiles do that.
    directory = os.path.join(root, '10.03_app_demo/2_src')
    if not os.path.isabs(source):
        source = os.path.normpath(os.path.join(directory, source))

    # A source compiled for both platforms is listed once; the first wins,
    # which is UNIBUS, the configuration with the most devices in it.
    entries.setdefault(source, {
        'directory': directory,
        'file': source,
        'arguments': words,
    })

json.dump(list(entries.values()), sys.stdout, indent=1)
sys.stdout.write('\n')
print("%d sources" % len(entries), file=sys.stderr)
GENERATOR

# The host builds - the CPU core tests and the VAX simulators - need no
# container and no cross toolchain, and name their compiler in HOST_CC.
ask_host_make () {                              # ask_host_make <directory>
    make -C "$1" -B -n QUNILATOR_DIR="$root" QUNIBONE_DIR="$root" 2>/dev/null || true
}

{
    ask_make _u unibus UNIBUS
    ask_make _q qbus QBUS
    ask_host_make 10.06_cputest/2_src
    ask_host_make 10.07_vax/2_src
} | python3 "$generator" "$root" >compile_commands.json
