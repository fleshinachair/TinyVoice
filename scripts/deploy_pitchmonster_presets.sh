#!/usr/bin/env bash
# Install the presets scripts/import_pitchmonster.py made (build/pitchmonster_presets/*.json) into
# Schwung's preset store on the Move, where TinyVoice's My Presets page lists them (one flat folder):
#   /data/UserData/schwung/presets/tinyvoice/
#
#   bash scripts/deploy_pitchmonster_presets.sh [host]        (default move.local; MOVE_USER ableton)
#
# Order: upload to a staging folder, checksum it against the local files, then copy into place
# (a rollback copy of what was there is kept first). Files of your own there are left
# alone. No restart: Schwung reads a preset when it loads. The presets are converted from your
# own PitchMonster installation: keep them on your machine and your Move.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${1:-${MOVE_HOST:-move.local}}"
USER_="${MOVE_USER:-ableton}"
SRC="$ROOT/build/pitchmonster_presets"
DST=/data/UserData/schwung/presets/tinyvoice
STAGE=/data/UserData/tinyvoice-pm-presets-staging
RB=/data/UserData/tinyvoice-pm-presets-rollback
SSH="ssh -o ConnectTimeout=8 $USER_@$HOST"

[ -d "$SRC" ] || { echo "no $SRC: run scripts/import_pitchmonster.py first"; exit 1; }
norm() { sed -E 's/^([0-9a-f]+) [ *]/\1  /' | sort; }
want=$(cd "$SRC" && md5sum *.json | norm)
echo "$(echo "$want" | wc -l) presets to install on $HOST"

$SSH "mkdir -p '$DST' '$RB' && cp -rp '$DST'/. '$RB'/ 2>/dev/null || true; rm -rf '$STAGE' && mkdir -p '$STAGE'"
tar -C "$SRC" -cf - . | $SSH "tar -C '$STAGE' -xf -"
got=$($SSH "cd '$STAGE' && md5sum *.json" | norm)     # BusyBox xargs has no -0; the folder is flat
[ "$want" = "$got" ] || { echo "staging checksums differ: nothing installed"; exit 1; }
$SSH "cp -rf '$STAGE'/. '$DST'/ && rm -rf '$STAGE'"
got=$($SSH "cd '$DST' && md5sum *.json" | norm)
missing=$(comm -23 <(echo "$want") <(echo "$got") | wc -l)
[ "$missing" -eq 0 ] || { echo "$missing installed file(s) differ: the previous files are in $RB"; exit 1; }
echo "installed. On the Move: TinyVoice (Mangler) > Presets, Imported bank (and My Presets)."
