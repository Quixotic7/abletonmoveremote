#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# The built module onto the Move (ableton@move.local, or $MOVE): a running Move Remote is stopped first (Move comes
# back), every file goes up under a temporary name and is moved into place (never written over a file in use).
#   scripts/deploy.sh                    copy only
#   scripts/deploy.sh --launch           .. and start it (Move's own software stops until you leave it)
#   scripts/deploy.sh --test [PERCENT]   .. started with the test hook on (/dev/shm/mr-cmd, /dev/shm/mr-state).
#                                        A test is SILENT unless PERCENT (1..100) of the output is asked for: the
#                                        speaker is loud; keep it at 10 or less
set -euo pipefail
DIR="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${MOVE:-ableton@move.local}"
DEST=/data/UserData/schwung/modules/tools/moveremote
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR"
[[ -x "$DIR/dist/moveremote/moveremote-host" ]] || { echo "ERROR: run make package first" >&2; exit 1; }
$SSH "$HOST" 'P=$(pidof moveremote-host); if [ -n "$P" ]; then kill -TERM $P; n=0; while [ $n -lt 40 ] && pidof moveremote-host >/dev/null; do sleep 0.25; n=$((n+1)); done; sleep 8; echo "deploy: stopped the running one"; fi; mkdir -p '"$DEST"
for f in moveremote-host moveremote-heal standalone module.json LICENSE THIRD_PARTY_LICENSES.md; do
    scp -q -o BatchMode=yes -o LogLevel=ERROR "$DIR/dist/moveremote/$f" "$HOST:$DEST/.$f.new"
    $SSH "$HOST" "mv '$DEST/.$f.new' '$DEST/$f'"
done
$SSH "$HOST" "chmod 755 $DEST/moveremote-host $DEST/moveremote-heal $DEST/standalone; rm -f /dev/shm/mr-test"
echo "deploy: copied to $HOST:$DEST"
MODE="${1:-}"
VOL="${2:-0}"
[[ "$VOL" =~ ^[0-9]+$ ]] || { echo "ERROR: the test volume is a percent, 0..100" >&2; exit 1; }
[[ "$MODE" == --test ]] && $SSH "$HOST" "echo $VOL >/dev/shm/mr-test"
if [[ "$MODE" == --test || "$MODE" == --launch ]]; then
    $SSH "$HOST" "/data/UserData/schwung/launch-standalone.sh $DEST/standalone; sleep 9; cat /data/UserData/schwung/moveremote-host.log; pidof moveremote-host >/dev/null && echo 'deploy: running' || echo 'deploy: NOT running'"
fi
