#!/bin/bash
# Runs one scenario against a fresh fake Dispatcharr.
#
#   [BIN=pvr_harness] [VARIANT=asan] [T=90] [TMO=30] [CTL="knob=value&knob=value"] tests/glue/run.sh <scenario> [args...]
#
# BIN is stream_harness (default; scenarios live, live_fault, rec, ip, ... see stream_driver.cpp's main()) or
# pvr_harness (ctor_dtor, functional, surface, ...). CTL sets fault knobs on the fake server before the run
# (see fake_dispatcharr.py's /__ctl handler), TMO is the client's request timeout in seconds. Under tsan the
# sanitizer runtime refuses the address-space layout on roughly half of all starts; use run_tsan.sh.
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
BIN=${BIN:-stream_harness}; V=${VARIANT:-asan}
# A port the OS has just confirmed is free, and a retry if the fake still dies at start-up. A fixed random range overlapped
# the ephemeral client ports of the earlier scenarios' connections, so the fake sometimes failed to bind ("Address already
# in use") and the scenario then ran against nothing, which one scenario (connection_state) reports as an unreachable server.
for attempt in 1 2 3 4 5 6; do
  PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
  python3 "$HERE/fake_dispatcharr.py" $PORT & SPID=$!
  for i in $(seq 50); do
    kill -0 $SPID 2>/dev/null || break
    curl -s -o /dev/null http://127.0.0.1:$PORT/__stats && break
    sleep 0.1
  done
  kill -0 $SPID 2>/dev/null && break
  wait $SPID 2>/dev/null
  echo "fake_dispatcharr.py did not start on port $PORT (attempt $attempt), retrying" >&2
done
[ -n "$CTL" ] && curl -s -o /dev/null "http://127.0.0.1:$PORT/__ctl?$CTL"
echo "== $BIN $* (port $PORT) CTL=$CTL"
LEAKS=1; [ "$2" = dtor_inflight ] || [ "$1" = dtor_inflight ] && LEAKS=0
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=$LEAKS} UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
  timeout ${T:-90} "$ROOT/build-glue/$V/$BIN" $PORT "$@" 2>&1 | grep -v -E "${HIDE:-^\[kodi}" | head -${LINES_MAX:-80}
echo "rc=${PIPESTATUS[0]}"
kill $SPID; wait $SPID 2>/dev/null; true
