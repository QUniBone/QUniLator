#!/bin/bash
# Start the emulator's menu program - "demo" as it is built, /usr/bin/<name>-cli
# as the package installs it, named for the board's bus. It drives the PRUs and
# the bus, so it wants root: "sudo ./demo.sh".
for cli in /usr/bin/qbone-cli /usr/bin/unibone-cli ; do
    if [ -x "$cli" ] ; then
        exec "$cli" --verbose "$@"
    fi
done
echo "$(basename "$0"): no qbone-cli or unibone-cli in /usr/bin - is the" >&2
echo "emulator installed here? A build in this tree installs it: ./compile.sh" >&2
exit 1
