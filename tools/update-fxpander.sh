#!/bin/sh
# update-fxpander.sh: on the MPC, replace the installed FXpander folder with /tmp/r5hfxpander.tar (same name and uid,
# so MPC.settings is not touched) and restart the MPC app, which only reads a skin at startup.
# The old folder is kept as <drive>/fxpander-prev (undo: stop acvs, swap the folders back, start acvs).
# Chain presets live in <drive>/FXpander Presets and are not touched. DRIVE= as for register-fxpander.sh.
set -e
DRIVE="${DRIVE:-/media/MPC_ONE}"
T=/tmp/r5hfxpander.tar
NAME="Ru5h - VST - FXpander"
D="$DRIVE/Synths/$NAME"
PREV="$DRIVE/fxpander-prev"
N="$DRIVE/fxpander-new"
[ -f "$T" ] || { echo "no $T"; exit 1; }
[ -d "$D" ] || { echo "no $D"; exit 1; }
FREE=$(df -k /data | tail -n 1 | awk '{print $4}')
[ "$FREE" -ge 204800 ] || { echo "/data has only $FREE KB free (an app start needs ~180 MB)"; exit 1; }
rm -rf "$N" && mkdir -p "$N" && tar -xf "$T" -C "$N"
[ -f "$N/$NAME/r5hfxpander.so" ] || { echo "the tar has no $NAME/r5hfxpander.so"; exit 1; }
if /lib/ld-linux-armhf.so.3 --list "$N/$NAME/r5hfxpander.so" | grep -q "not found"; then
    echo "missing libraries:"; /lib/ld-linux-armhf.so.3 --list "$N/$NAME/r5hfxpander.so"; exit 1
fi
trap 'systemctl is-active -q acvs || systemctl start acvs' EXIT   # the app comes back even if a step below fails
systemctl stop acvs
rm -rf "$PREV"
mv "$D" "$PREV"
mv "$N/$NAME" "$D"
rmdir "$N"
sync
md5sum "$D/r5hfxpander.so"
echo "skin files: $(ls "$D/Plugin Skins" | wc -l); old folder kept as $PREV"
systemctl start acvs
sleep 8
systemctl is-active acvs
grep -q 'uid="52354658"' /data/Settings/MPC/MPC.settings && echo "entry still there after the app started"
