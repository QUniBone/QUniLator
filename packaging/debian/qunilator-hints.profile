# QUniLator: what a freshly flashed board can still be given - the source tree
# and the disk images, neither of which the image carries. qunilator-hints
# decides from what the board holds and prints nothing once both are there.
#
# This is /etc/profile.d rather than a motd: the image's /etc/update-motd.d is
# inert (nothing regenerates /run/motd.dynamic and the scripts are not
# executable), and a file in /etc/motd.d is static, so it could not go quiet by
# itself. A login shell sources this; "ssh <board> <command>" does not, so
# nothing is prepended to the output of a deploy or a script.
#
# Interactive shells only, so "bash -lc" in a pipeline stays clean. Delete this
# file to be rid of the hints; dpkg will not put it back.
case $- in
    *i*)
        if [ -x /usr/sbin/qunilator-hints ]; then
            /usr/sbin/qunilator-hints
        fi
        ;;
esac
