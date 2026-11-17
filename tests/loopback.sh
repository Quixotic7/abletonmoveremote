#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# The simulator and the host library end to end: mrctl selftest (every message type, injected presses, a second host
# taking over), the page the device serves, and the WebSocket handshake (RFC 6455's own example key).
set -uo pipefail
cd "$(dirname "$0")/.."
PORT=17720
DIR="$(mktemp -d)"
build/sim/moveremote-sim --port $PORT --dir "$DIR" 2>"$DIR/sim.log" &
SIM=$!
trap 'kill $SIM 2>/dev/null; wait $SIM 2>/dev/null; rm -rf "$DIR"' EXIT
for _ in $(seq 50); do [[ -p "$DIR/mr-cmd" ]] && break; sleep 0.1; done
fail=0
build/host/mrctl --host 127.0.0.1 --port $PORT selftest --inject "$DIR/mr-cmd" || fail=1

if curl -s "http://127.0.0.1:$PORT/" | grep -q "<title>Move Remote</title>"; then echo "  ok    the page"
else echo "  FAIL  the page"; fail=1; fi

# (curl, not nc: the BSD and Linux nc part differently from a connection left open, and a WebSocket stays open)
ACCEPT=$(curl -s -i -N --max-time 1 -H "Connection: Upgrade" -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" -H "Sec-WebSocket-Version: 13" "http://127.0.0.1:$PORT/ws" \
    | tr -d '\r' | sed -n 's/^Sec-WebSocket-Accept: //p')
if [[ "$ACCEPT" == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" ]]; then echo "  ok    WebSocket handshake"
else echo "  FAIL  WebSocket handshake (got '$ACCEPT')"; fail=1; fi

echo "shot" > "$DIR/mr-cmd"; sleep 0.3
if [[ -s "$DIR/mr-screen.pbm" ]] && head -1 "$DIR/mr-screen.pbm" | grep -q P1; then echo "  ok    screen shot"
else echo "  FAIL  screen shot"; fail=1; fi
grep -q "host=" "$DIR/mr-state" && echo "  ok    state line: $(cat "$DIR/mr-state")" || { echo "  FAIL  state line"; fail=1; }

echo "quit" > "$DIR/mr-cmd"
for _ in $(seq 30); do kill -0 $SIM 2>/dev/null || break; sleep 0.1; done
if kill -0 $SIM 2>/dev/null; then echo "  FAIL  the simulator ignored quit"; fail=1; else echo "  ok    quit"; fi
[[ $fail == 0 ]] && echo "loopback: passed" || { echo "loopback: FAILED"; cat "$DIR/sim.log"; }
exit $fail
