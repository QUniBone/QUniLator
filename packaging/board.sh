# board.sh: the board's identity and the branding it puts on what is installed.
#
# Sourced, not run. The caller sets SUFFIX to _q or _u before sourcing; this
# defines NAME, DISPLAY and BUS for that board, the rebrand filter that carries
# them into a text file, and stage_frontend, which writes the web root.
#
# The emulator is compiled for one bus, so the binary, the unit that runs it and
# the package carry the board's name: QBUS is qbone/QBone, UNIBUS is
# unibone/UniBone. The web interface shows the software's own product name,
# QUniLator, which is the same on both buses. The appliance's own files - the
# provisioning tools, their units, and the paths under /etc, /var/lib and
# /usr/share - are named "qunilator" whichever bus the cape bridges.

if [ "$SUFFIX" = _u ]; then
    NAME=unibone; DISPLAY=UniBone; OTHER=qbone; BUS=Unibus
else
    NAME=qbone;   DISPLAY=QBone;   OTHER=unibone; BUS=Qbus
fi

# Carry the board brand and its bus into a text file that names the other.
rebrand() {
    sed -e "s/qbone/$NAME/g" -e "s/QBone/$DISPLAY/g" -e "s/Qbus/$BUS/g"
}

# Write the web root - the Vite build output - into $1. index.html, the hashed
# JS/CSS bundles and the manifest carry the display brand, so rebrand those text
# assets; the favicons and other binaries copy as-is. The bundle hash is not
# recomputed, but index.html references the assets by their built names, so this
# stays coherent.
stage_frontend() {
    local dest=$1 dist=10.05_web/3_frontend/dist f b
    [ -d "$dist" ] || {
        echo "no $dist - run 'npm ci && npm run build' in 10.05_web/3_frontend" >&2
        return 1
    }
    mkdir -p "$dest/assets"
    for f in "$dist"/*; do
        [ -f "$f" ] || continue
        b=$(basename "$f")
        case "$b" in
            index.html|site.webmanifest)
                rebrand < "$f" > "$dest/$b"; chmod 644 "$dest/$b" ;;
            *)
                install -m 644 "$f" "$dest/$b" ;;
        esac
    done
    for f in "$dist"/assets/*; do
        [ -f "$f" ] || continue
        b=$(basename "$f")
        case "$b" in
            *.js|*.css)
                rebrand < "$f" > "$dest/assets/$b"; chmod 644 "$dest/assets/$b" ;;
            *)
                install -m 644 "$f" "$dest/assets/$b" ;;
        esac
    done
}
