#!/bin/sh
# register-fxpander.sh: on the MPC, the first install of FXpander.
#   sh register-fxpander.sh            install /tmp/r5hfxpander.tar into Synths and register it in MPC.settings
#   sh register-fxpander.sh dry FILE   only add the entry to FILE (a copy of MPC.settings), to check the edit
# Checks the .so's libraries resolve and /data has room, stops the MPC app (acvs), backs up MPC.settings to the MPC_ONE
# drive, adds the PLUGIN entry before </KNOWNPLUGINS>, unpacks the plugin folder and starts acvs again.
# Undo: stop acvs, copy the backup back over MPC.settings, delete the plugin folder, start acvs.
# DRIVE= picks the drive the plugin goes on (default /media/MPC_ONE):  DRIVE=/media/MYDRIVE sh register-fxpander.sh
set -e
DRIVE="${DRIVE:-/media/MPC_ONE}"
T=/tmp/r5hfxpander.tar
S="$DRIVE/Synths"
NAME="Ru5h - VST - FXpander"
D="$S/$NAME"
SET=/data/Settings/MPC/MPC.settings
UID_HEX=52354658
ENTRY="      <PLUGIN name=\"FXpander\" descriptiveName=\"FXpander\" format=\"VST\" category=\"Effect\" manufacturer=\"Ru5h\" version=\"1.0\" file=\"$D/r5hfxpander.so\" uid=\"52354658\" isInstrument=\"0\" fileTime=\"0\" infoUpdateTime=\"0\" numInputs=\"2\" numOutputs=\"2\" isShell=\"0\"/>"

add_entry() {   # $1: the settings file; exactly one entry afterwards, or fail
    grep -q "uid=\"$UID_HEX\"" "$1" && { echo "already registered in $1"; return 1; }
    grep -q "</KNOWNPLUGINS>" "$1" || { echo "no </KNOWNPLUGINS> in $1"; return 1; }
    awk -v e="$ENTRY" '/<\/KNOWNPLUGINS>/ && !done { print e; done = 1 } { print }' "$1" > "$1.new"
    [ "$(grep -c "uid=\"$UID_HEX\"" "$1.new")" = 1 ] || { echo "entry not added"; rm -f "$1.new"; return 1; }
    mv "$1.new" "$1"
}

if [ "$1" = dry ]; then add_entry "$2"; echo "dry run: entry added to $2"; exit 0; fi

[ -f "$T" ] || { echo "no $T"; exit 1; }
[ -d "$DRIVE" ] || { echo "no $DRIVE (set DRIVE= to your drive's folder under /media)"; exit 1; }
[ ! -e "$D" ] || { echo "$D already exists"; exit 1; }
grep -q "uid=\"$UID_HEX\"" "$SET" && { echo "already registered"; exit 1; }
FREE=$(df -k /data | tail -n 1 | awk '{print $4}')
[ "$FREE" -ge 204800 ] || { echo "/data has only $FREE KB free (an app start needs ~180 MB)"; exit 1; }
N="$DRIVE/fxpander-new"
rm -rf "$N" && mkdir -p "$N" && tar -xf "$T" -C "$N"
[ -f "$N/$NAME/r5hfxpander.so" ] || { echo "the tar has no $NAME/r5hfxpander.so"; exit 1; }
if /lib/ld-linux-armhf.so.3 --list "$N/$NAME/r5hfxpander.so" | grep -q "not found"; then
    echo "missing libraries:"; /lib/ld-linux-armhf.so.3 --list "$N/$NAME/r5hfxpander.so"; exit 1
fi
STAMP=$(date +%Y%m%d-%H%M%S)
BAK="$DRIVE/settings-backups/MPC.settings.bak-fxpander-$STAMP"
mkdir -p "$DRIVE/settings-backups"
trap 'systemctl is-active -q acvs || systemctl start acvs' EXIT   # the app comes back even if a step below fails
systemctl stop acvs
cp "$SET" "$BAK"
cmp -s "$SET" "$BAK" || { echo "backup differs"; exit 1; }
echo "backup: $BAK"
add_entry "$SET"
echo "registered: $(grep -c "uid=\"$UID_HEX\"" "$SET") entry"
mv "$N/$NAME" "$D"
rmdir "$N"
sync
md5sum "$D/r5hfxpander.so"
echo "skin files: $(ls "$D/Plugin Skins" | wc -l)"
systemctl start acvs
sleep 8
systemctl is-active acvs
grep -q "uid=\"$UID_HEX\"" "$SET" && echo "entry still there after the app started"
