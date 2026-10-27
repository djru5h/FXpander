#!/usr/bin/env bash
# Package the plugin folder (skin + .so) as the MPC expects it:  tools/pack.sh [dest dir]  -> r5hfxpander.tar (default: tools/)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$HERE/tools}"
NAME="Ru5h - VST - FXpander"
STAGE="$(mktemp -d)"
cp -a "$HERE/vst/build/skin/$NAME" "$STAGE/"
cp "$HERE/vst/build/r5hfxpander.so" "$STAGE/$NAME/"
tar -cf "$DEST/r5hfxpander.tar" -C "$STAGE" "$NAME"
rm -rf "$STAGE"
echo "$(tar -tf "$DEST/r5hfxpander.tar" | wc -l) entries; $(du -h "$DEST/r5hfxpander.tar" | cut -f1)"
md5sum "$HERE/vst/build/r5hfxpander.so" "$DEST/r5hfxpander.tar"
