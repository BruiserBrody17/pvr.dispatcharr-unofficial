#!/bin/bash
# run.sh under ThreadSanitizer, retrying while its runtime refuses the address-space layout ("unexpected memory
# mapping", about half of all starts on a kernel with high mmap randomisation).
HERE=$(cd "$(dirname "$0")" && pwd)
for i in $(seq 1 40); do
  out=$(VARIANT=tsan HIDE='^\[kodi' LINES_MAX=100000 TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 history_size=4 report_signal_unsafe=0" "$HERE/run.sh" "$@" 2>&1)
  if ! echo "$out" | grep -q "unexpected memory mapping"; then echo "$out"; echo "(tsan attempt $i)"; exit 0; fi
done
echo "TSan never started"; exit 1
