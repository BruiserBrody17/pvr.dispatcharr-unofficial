#!/bin/bash
# Builds the glue harness: the real src/*.cpp (DispatcharrClient, PVRDispatcharr, ...) against Kodi's real
# dev-kit headers, with the Kodi runtime stubbed (kodi_stub.cpp, pvr_stub.cpp), linked into two drivers.
#
#   tests/glue/build.sh [asan|tsan|plain]      default asan (AddressSanitizer + UBSan)
#
# Needs libcurl dev headers, g++ 13+, and two things the Catch2 suite's own build already fetches:
#   build-tests/_deps/json-src and build-tests/_deps/pugixml-src  (run `cmake -S tests -B build-tests` once)
# and a Kodi source checkout for its dev-kit headers: KODI_INCLUDE, default
#   ~/kodi-build/kodi-source/xbmc/addons/kodi-dev-kit/include   (docs/BUILDING.md describes the checkout).
# Output: build-glue/<variant>/stream_harness and pvr_harness.
set -e
V=${1:-asan}
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
KI=${KODI_INCLUDE:-$HOME/kodi-build/kodi-source/xbmc/addons/kodi-dev-kit/include}
DEPS=$ROOT/build-tests/_deps
[ -f "$KI/kodi/addon-instance/PVR.h" ] || { echo "Kodi dev-kit headers not found in $KI (set KODI_INCLUDE)"; exit 1; }
[ -d "$DEPS/json-src" ] || { echo "run: cmake -S tests -B build-tests   (fetches nlohmann-json and pugixml)"; exit 1; }
case $V in
  asan) FLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" ;;
  tsan) FLAGS="-O1 -g -fsanitize=thread -fno-pie -no-pie -fno-omit-frame-pointer" ;;
  plain) FLAGS="-O1 -g" ;;
  *) echo "variant must be asan, tsan or plain"; exit 1 ;;
esac
O=$ROOT/build-glue/$V; mkdir -p "$O/obj"
# A stale object left by an earlier build would be linked in if its source failed to compile this time.
rm -f "$O"/obj/*.o
INC="-I$KI -I$DEPS/json-src/single_include -I$DEPS/pugixml-src/src -I$ROOT/src"
CXX="g++ -std=c++17 $FLAGS $INC"
# addon.cpp registers the real addon entry point, which the drivers replace.
ls "$ROOT"/src/*.cpp | grep -v '/addon\.cpp$' | xargs -P"$(nproc)" -I{} sh -c \
  "n=\$(basename {} .cpp); $CXX -c {} -o $O/obj/\$n.o || { echo FAILED \$n; exit 255; }"
g++ -std=c++17 $FLAGS -c "$DEPS/pugixml-src/src/pugixml.cpp" -o "$O/obj/pugixml.o"
OBJS=$(ls "$O"/obj/*.o | grep -v '/PVRDispatcharr\.o$')
$CXX -c "$HERE/stream_driver.cpp" -o "$O/stream_driver.o"
$CXX -c "$HERE/kodi_stub.cpp" -o "$O/kodi_stub.o"
g++ $FLAGS -o "$O/stream_harness" "$O/stream_driver.o" "$O/kodi_stub.o" $OBJS -lcurl -lpthread
$CXX -c "$HERE/pvr_driver.cpp" -o "$O/pvr_driver.o"
$CXX -c "$HERE/pvr_stub.cpp" -o "$O/pvr_stub.o"
g++ $FLAGS -o "$O/pvr_harness" "$O/pvr_driver.o" "$O/pvr_stub.o" $(ls "$O"/obj/*.o) -lcurl -lpthread
echo "built $O/stream_harness and $O/pvr_harness"
