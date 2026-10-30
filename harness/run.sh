#!/usr/bin/env bash
# Offline checks, in the gcc:12 Docker image (nothing to install): builds FXpander, the test children (testfx.c) and
# x86 builds of P-Valley PNG and YES-SL 9000 from their repos next to this one (mpc-noise-gen, mpc-channel-strip; gain
# stand-ins if they aren't there), all under ASan/UBSan, and runs harness/host.c against them. Exit status is the checks'.
# With `aarch64` the same checks run in an arm64 container (the Gen2 CPU; QEMU on an x86 host), under UBSan only:
# ASan doesn't run under QEMU's user-mode emulation. That build reads the Gen2 plugin list (PLUGIN_LIST).
#   harness/run.sh [aarch64]
set -euo pipefail
PLAT=() B=harness/build SAN="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
if [ "${1:-}" = aarch64 ]; then
    PLAT=(--platform linux/arm64) B=harness/build-aarch64 SAN="-O1 -g -fsanitize=undefined -fno-omit-frame-pointer"
fi
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEV="$(cd "$HERE/.." && pwd)"
if [ -z "${MPC_VST:-}" ]; then   # the framework: the submodule, else a checkout next to this one
    if [ -f "$HERE/mpc-vst-plugins/tools/gen_vst.py" ]; then MPC_VST="$HERE/mpc-vst-plugins"; else MPC_VST="$DEV/mpc-vst-plugins"; fi
fi
REAL=1
[ -d "$DEV/mpc-noise-gen/src" ] && [ -d "$DEV/mpc-channel-strip/src" ] || REAL=0
python3 "$HERE/vst/make_skin.py" >/dev/null
python3 "$MPC_VST/tools/gen_vst.py" "$HERE/vst/vst.json" --params-h >/dev/null
docker run --rm ${PLAT[@]+"${PLAT[@]}"} -e REAL=$REAL -e B="$B" -e SAN="$SAN" -v "$HERE":/w -v "$DEV":/d:ro -v "$MPC_VST":/mv:ro -w /w gcc:12 bash -euc '
  mkdir -p $B; rm -rf $B/presets
  echo "--- building for $(uname -m)"
  CC="gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fPIC"
  W=/mv/wrapper
  $CC $SAN -shared -Isrc -Ivst/build src/fxpander.c -ldl -lpthread -lm -o $B/r5hfxpander.so
  $CC $SAN -shared -DTFX_UID=0x54476e31 harness/testfx.c -o $B/testfx_gain.so
  $CC $SAN -shared -DTFX_UID=0x54476e32 -DUSE_CHUNK harness/testfx.c -o $B/testfx_chunk.so
  $CC $SAN -shared -DTFX_UID=0x54476e33 -DMONO harness/testfx.c -o $B/testfx_mono.so
  $CC $SAN -shared -DTFX_UID=0x54476e34 -DSYNTH harness/testfx.c -o $B/testfx_synth.so
  $CC $SAN -shared -DTFX_UID=0x54476e37 -DEXTRA=100 harness/testfx.c -o $B/testfx_many.so
  echo "int not_a_plugin(void) { return 1; }" > $B/notvst.c && $CC -shared $B/notvst.c -o $B/notvst.so
  if [ "$REAL" = 1 ]; then
    N=/d/mpc-noise-gen; S=/d/mpc-channel-strip
    $CC $SAN -shared -I$N/vst/build -I$W -I$N/src $N/src/engine.c $N/src/noise.c $W/vst2_wrap.c -lm -lpthread -o $B/pvalley.so
    $CC $SAN -shared -I$S/vst/build -I$W -I$S/src $S/src/engine.c $S/src/strip.c $S/src/presets.c $S/src/trace.c $W/vst2_wrap.c -lm -lpthread -o $B/yessl.so
  else
    echo "mpc-noise-gen / mpc-channel-strip not next to this repo: gain stand-ins, their checks skipped"
    $CC $SAN -shared -DTFX_UID=0x52354e5a harness/testfx.c -o $B/pvalley.so
    $CC $SAN -shared -DTFX_UID=0x5235394a harness/testfx.c -o $B/yessl.so
    export FXPANDER_NO_REAL=1
  fi
  $CC $SAN -Ivst/build harness/host.c -ldl -lpthread -lm -o $B/host
  echo "--- checks ($SAN)"
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ASAN_OPTIONS=detect_leaks=1 $B/host /w/$B/r5hfxpander.so /w/$B
'
