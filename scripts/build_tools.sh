#!/usr/bin/env bash
# Native render tools for the calibration workbench (build/*.exe on Windows):
#   hm_render        the bare Resynth engine from a preset's params (calibration/tv_host.py)
#   tv_state_render  the whole module from a state / Schwung preset file (make_default_bank.py, level sweeps)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC=(zig cc)
command -v zig >/dev/null || CC=(cc)
EXT=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXT=".exe" ;; esac
D="$ROOT/src/dsp"
mkdir -p "$ROOT/build"
FL=(-O2 -std=c11 -Wall -Wextra -Werror -pedantic -I"$D")
"${CC[@]}" "${FL[@]}" "$D/hm.c" "$D/hmfx.c" "$D/fft.c" "$D/cher.c" "$ROOT/tools/hm_render.c" -o "$ROOT/build/hm_render$EXT" -lm
"${CC[@]}" "${FL[@]}" "$ROOT/tools/tv_state_render.c" "$D/dsp.c" "$D/cher.c" "$D/daft.c" "$D/imogen.c" "$D/psola.c" "$D/modes.c" \
    "$D/freeze.c" "$D/demo.c" "$D/voice.c" "$D/verb.c" "$D/vfx.c" "$D/presets.c" "$D/groove.c" "$D/tour.c" \
    "$D/hm.c" "$D/hmfx.c" "$D/fft.c" "$D/rsbank.c" -o "$ROOT/build/tv_state_render$EXT" -lm
echo "built build/hm_render$EXT build/tv_state_render$EXT"
