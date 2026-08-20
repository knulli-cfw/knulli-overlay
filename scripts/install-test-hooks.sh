#!/bin/sh
# Wires the overlay into the device's volume/brightness keys by adding a call
# to /usr/bin/volume-button, which is what triggerhappy runs for them.  This
# edits a system file, so the original is kept next to it and --uninstall puts
# it back.  Event driven: nothing runs between key presses.
#
#     install-test-hooks.sh              # patch
#     install-test-hooks.sh --uninstall  # restore
#
# On knulli the root filesystem is a writable overlay, so the change survives a
# reboot but not a firmware upgrade.

TARGET=/usr/bin/volume-button
BACKUP=/usr/bin/volume-button.pre-knulli-overlay

if [ "$1" = "--uninstall" ]; then
    [ -f "$BACKUP" ] || { echo "no backup at $BACKUP"; exit 1; }
    mv "$BACKUP" "$TARGET" && echo "restored $TARGET"
    exit $?
fi

[ -f "$TARGET" ] || { echo "$TARGET not found"; exit 1; }
if grep -q knulli-overlay "$TARGET"; then
    echo "$TARGET already patched"
    exit 0
fi
command -v knulli-overlay >/dev/null || { echo "knulli-overlay is not in PATH"; exit 1; }

cp -a "$TARGET" "$BACKUP" || exit 1

# Show the value the key press just produced, right after each adjustment.
sed -e 's#^\( *\)\(knulli-audio setSystemVolume .*\)$#\1\2\n\1knulli-overlay volume "$(knulli-audio getSystemVolume)"#' \
    -e 's#^\( *\)\(knulli-brightness [+-] .*\)$#\1\2\n\1knulli-overlay brightness "$(knulli-brightness)"#' \
    "$BACKUP" > "$TARGET" || { cp -a "$BACKUP" "$TARGET"; exit 1; }
chmod --reference="$BACKUP" "$TARGET" 2>/dev/null || chmod 0755 "$TARGET"

echo "patched $TARGET ($(grep -c knulli-overlay "$TARGET") calls added), backup at $BACKUP"
