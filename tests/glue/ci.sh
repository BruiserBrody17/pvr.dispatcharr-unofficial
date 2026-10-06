#!/bin/bash
# The scenarios CI runs (the "glue-harness" job in .github/workflows/build.yml): every stream path and the Kodi-facing
# PVR surface under ASan + UBSan, each against a fresh fake Dispatcharr. Build first with tests/glue/build.sh asan.
# Exits non-zero when any scenario reports a failure or a non-zero exit status, and names which. The scenarios not
# listed (ip_chaos, the TSan variants, the plugin integration run, the longer fault matrices) stay by-hand; see README.md.
HERE=$(cd "$(dirname "$0")" && pwd)
failed=0
run() {  # run <label> <env assignments...> -- <run.sh args...>
  local label=$1; shift
  local envs=(); while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
  local out; out=$(env LINES_MAX=100000 "${envs[@]}" "$HERE/run.sh" "$@" 2>&1)
  # A sanitizer report that did not stop the run (UBSan prints "runtime error:" and carries on, run.sh sets
  # halt_on_error=0 for by-hand exploring) is still a failure here.
  if echo "$out" | grep -q '^RESULT fails=0$' && echo "$out" | grep -q '^rc=0$' && ! echo "$out" | grep -q 'runtime error:'; then
    echo "ok    $label"
  else
    echo "FAIL  $label"; echo "$out" | tail -n 25 | sed 's/^/        /'; failed=$((failed + 1))
  fi
}
run "live"                       TMO=30 T=120 -- live 1500
run "live (chaos seeker)"        TMO=30 T=120 -- live_chaos
run "live (reopen x5)"           TMO=30 T=120 -- live_reopen
run "live (close at the tail)"   TMO=30 T=120 -- live_tail_close
run "live (hung API, close)"     TMO=30 T=120 -- live_blackhole_close
run "live (hung API, 60 s timeout)" TMO=60 T=200 -- live_blackhole_close
run "live (6 s API latency)"     TMO=30 T=300 -- live_slow_open
run "completed recording"        TMO=30 T=120 -- rec
run "recording, Range dropped"   TMO=30 T=120 -- rec_range_dropped
run "recording with no file"     TMO=30 T=120 -- rec_open_missing
run "in-progress recording"      TMO=30 T=120 -- ip 6000
run "in-progress, probe cascade" TMO=30 T=120 -- ip_cascade
run "in-progress, hung API"      TMO=30 T=300 -- ip_blackhole_close
run "in-progress, 500 blip"      TMO=30 T=120 IP_EXPECT_RESUME=1 -- ip 9000 "2000:ip_seg_status=500;5500:ip_seg_status=0"
run "destroy with streams open"  TMO=30 T=120 -- dtor_inflight
run "PVR surface: functional"    BIN=pvr_harness T=180 -- functional
run "PVR: M3U refresh event"       BIN=pvr_harness T=60 -- refresh_events
run "PVR: server offset vs table"   BIN=pvr_harness T=60 -- offset_cross_check
run "PVR surface: threads"       BIN=pvr_harness T=180 -- surface 6000
[ "$failed" -eq 0 ] && echo "all glue scenarios passed" || { echo "$failed glue scenario(s) failed"; exit 1; }
