#!/usr/bin/env bash
# Deploy the built module to a Move over SSH (dev sideload).
#
#   scripts/install.sh [--build]
#
# Env overrides: MOVE_HOST (default move.local), MOVE_USER (default ableton), DEST
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODULE_ID=tinyvoice
MOVE_HOST="${MOVE_HOST:-move.local}"
MOVE_USER="${MOVE_USER:-ableton}"
DEST="${DEST:-/data/UserData/schwung/modules/audio_fx/$MODULE_ID}"
SRC="$ROOT/build/$MODULE_ID"

[ "${1:-}" = "--build" ] && "$ROOT/scripts/build.sh"
[ -f "$SRC/$MODULE_ID.so" ] || { echo "nothing built; run scripts/build.sh (or pass --build)" >&2; exit 1; }

echo "installing to $MOVE_USER@$MOVE_HOST:$DEST"
# tar over ssh: no rsync/scp dependency, works from Git Bash on Windows.
# Never write over the .so in place: if Schwung has it loaded, overwriting its
# pages corrupts the running process (silent or garbage audio until a restart).
# Extract beside it and mv each file in: a rename leaves the old, mapped copy
# intact for the running process, and the new one loads on the next restart.
STAGE="$(dirname "$(dirname "$(dirname "$DEST")")")/.stage-$MODULE_ID"      # same filesystem, outside modules/
# The staged copy is checksummed against the build before anything moves: a dropped connection once
# left the Default presets as 0-byte files with no error (2026-11-24).
SSH="ssh -o ConnectTimeout=8 $MOVE_USER@$MOVE_HOST"
sums() { while IFS= read -r f; do md5sum "$f"; done | sed -E 's/^([0-9a-f]+) [ *]/  /' | sort; }
want=$(cd "$SRC" && find . -type f | sums)
tar -C "$SRC" -cf - . | $SSH "rm -rf '$STAGE' && mkdir -p '$STAGE' '$DEST' && tar -C '$STAGE' -xf -"
got=$($SSH "cd '$STAGE' && find . -type f | while IFS= read -r f; do md5sum \"\$f\"; done" | sed -E 's/^([0-9a-f]+) [ *]/  /' | sort)
[ "$want" = "$got" ] || { echo "staged files differ from the build (connection dropped?): nothing installed; run it again" >&2; exit 1; }
$SSH "chmod 755 '$STAGE'/*.so && rm -f '$DEST/dsp.so' '$DEST/ui.js' '$DEST/ui_chain.js' && rm -rf '$DEST/presets' && for f in '$STAGE'/*; do mv -f \"\$f\" '$DEST'/; done && rm -rf '$STAGE'"

echo "done. Reload the module (or restart Schwung) to pick up the new $MODULE_ID.so."
