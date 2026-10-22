#!/bin/bash
# run.sh under ThreadSanitizer, retrying while its runtime refuses the address-space layout ("unexpected memory
# mapping", about half of all starts on a kernel with high mmap randomisation). The same layouts sometimes get past
# that check and segfault inside the runtime's own start-up instead, before main(), with no output and no report
# (2 of 60 bare starts on one such kernel); a run that died on a signal without printing a line is retried as well.
HERE=$(cd "$(dirname "$0")" && pwd)
for i in $(seq 1 40); do
  out=$(VARIANT=tsan HIDE='^\[kodi' LINES_MAX=100000 TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 history_size=4 report_signal_unsafe=0" "$HERE/run.sh" "$@" 2>&1)
  echo "$out" | grep -q "unexpected memory mapping" && continue
  rc=$(echo "$out" | sed -n 's/^rc=//p' | tail -1)
  body=$(echo "$out" | grep -v -e '^== ' -e '^rc=' -e '^timeout: the monitored command dumped core$')
  [ -z "$body" ] && [ "${rc:-0}" -gt 128 ] && continue
  echo "$out"; echo "(tsan attempt $i)"; exit 0
done
echo "TSan never started"; exit 1
