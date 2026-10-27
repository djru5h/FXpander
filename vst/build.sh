#!/usr/bin/env bash
# Build FXpander: vst/make_skin.py (params.json, layout.conf), then mpc-vst-plugins' gen_vst.py (params.h, the skin
# folder, the plugin-list entry; branch ru5h-local), then the armhf .so from src/fxpander.c alone, plus the on-device
# test host (harness/devhost.c). FXpander is its own VST2 (it hosts plugins), so it doesn't link the framework's
# wrapper and tools/build_port.sh isn't used. Output in vst/build/. The framework is the mpc-vst-plugins submodule, else
# a checkout next to this repo; MPC_VST overrides both.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
if [ -z "${MPC_VST:-}" ]; then
    if [ -f "$ROOT/mpc-vst-plugins/tools/gen_vst.py" ]; then MPC_VST="$ROOT/mpc-vst-plugins"; else MPC_VST="$ROOT/../mpc-vst-plugins"; fi
fi
U="$(id -u):$(id -g)"
python3 "$HERE/make_skin.py"
docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art" >/dev/null
docker run --rm -u "$U" -e HOME=/tmp -v "$ROOT":/w -v "$MPC_VST":/mv:ro -w /w mpc-vst-html-art \
  python3 /mv/tools/gen_vst.py vst/vst.json
# glibc 2.31 (bullseye), so it loads on MPC OS 2.x as well as 3.x
docker run --rm --platform linux/arm/v7 -u "$U" -v "$ROOT":/b -w /b arm32v7/gcc:11-bullseye bash -euc '
  CC="gcc -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation"
  $CC -fPIC -shared -fvisibility=hidden -Isrc -Ivst/build src/fxpander.c -ldl -lpthread -lm -Wl,--no-undefined -o vst/build/r5hfxpander.so
  strip vst/build/r5hfxpander.so
  echo "exported: $(readelf --dyn-syms -W vst/build/r5hfxpander.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND | awk "{print \$8}" | tr "\n" " ")"
  echo "highest glibc: $(readelf -V vst/build/r5hfxpander.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1)"
  $CC -Ivst/build harness/devhost.c -ldl -lpthread -lm -o vst/build/devhost-armhf
'
md5sum "$ROOT/vst/build/r5hfxpander.so"
