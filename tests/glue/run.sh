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
BIN=${BIN:-stream_harness}; V=${VARIANT:-asan}; PORT=$((20000 + RANDOM % 20000))
python3 "$HERE/fake_dispatcharr.py" $PORT & SPID=$!
for i in $(seq 50); do curl -s -o /dev/null http://127.0.0.1:$PORT/__stats && break; sleep 0.1; done
[ -n "$CTL" ] && curl -s -o /dev/null "http://127.0.0.1:$PORT/__ctl?$CTL"
echo "== $BIN $* (port $PORT) CTL=$CTL"
LEAKS=1; [ "$2" = dtor_inflight ] || [ "$1" = dtor_inflight ] && LEAKS=0
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=$LEAKS} UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
  timeout ${T:-90} "$ROOT/build-glue/$V/$BIN" $PORT "$@" 2>&1 | grep -v -E "${HIDE:-^\[kodi}" | head -${LINES_MAX:-80}
echo "rc=${PIPESTATUS[0]}"
kill $SPID; wait $SPID 2>/dev/null; true
