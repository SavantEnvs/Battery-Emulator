#!/usr/bin/env bash
# Builds the sanitized fuzz binaries (+ -standalone reproducers) and upstream's own gtest suite.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
OUT=/mayhem
CMAKE_DIR="$SRC/mayhem/cmake"

# Build dirs are named *build so upstream's .gitignore (`*build/`) covers them (git clean -ffdX
# removes them) and start fresh so a re-run never trips over cmake cache state.
rm -rf "$SRC/tests-build" "$SRC/fuzz-build"

# 1) The project's own gtest binary, normal flags. First, so the shared cmake machinery is warm and
#    test.sh never compiles.
cmake -S "$CMAKE_DIR" -B "$SRC/tests-build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DFUZZ_LIB=OFF \
  -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS"
cmake --build "$SRC/tests-build" -j"$MAYHEM_JOBS" --target tests

# 2) The firmware + emulation layer, sanitized with fuzzer coverage instrumentation.
#    NB: -fsanitize=fuzzer-no-link goes on the project compile; the harness links the engine.
FUZZ_CXXFLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link"
cmake -S "$CMAKE_DIR" -B "$SRC/fuzz-build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DFUZZ_LIB=ON \
  -DCMAKE_CXX_FLAGS_RELEASE="-O1" -DCMAKE_CXX_FLAGS="$FUZZ_CXXFLAGS"
cmake --build "$SRC/fuzz-build" -j"$MAYHEM_JOBS" --target batteryfw

# 3) Harnesses: fuzzer binary and standalone reproducer each.
STD_O="$(mktemp -d)"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$STD_O/standalone_main.o"
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$STD_O/lsan_off.o"

# -fno-sized-deallocation: the harness deletes Battery/InverterProtocol objects through their base
# pointer (upstream's own test listener does the same), whose destructors are non-virtual; sized
# delete would make ASan flag that harness-side pattern as new-delete-type-mismatch on every input.
HARNESS_CXXFLAGS="-fno-sized-deallocation $SANITIZER_FLAGS $DEBUG_FLAGS -O1 -std=c++20 -DUNIT_TEST -DESP32 -DHW_LILYGO -DCOMMON_IMAGE \
  -I$SRC/test/emul -I$SRC/Software -I$SRC/mayhem/harness"
# world_reset.ld: see harness/world_reset.h.
RESET_LD="-Wl,-T,$SRC/mayhem/harness/world_reset.ld"
for h in fuzz_battery_can fuzz_inverter_can fuzz_rs485_modbus fuzz_can_peripherals; do
  $CXX $HARNESS_CXXFLAGS -fsanitize=fuzzer-no-link -c "$SRC/mayhem/harness/$h.cpp" -o "$STD_O/$h.o"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$STD_O/$h.o" "$STD_O/lsan_off.o" \
    "$SRC/fuzz-build/libbatteryfw.a" $RESET_LD -o "$OUT/$h"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS "$STD_O/$h.o" "$STD_O/standalone_main.o" "$STD_O/lsan_off.o" \
    "$SRC/fuzz-build/libbatteryfw.a" $RESET_LD -o "$OUT/$h-standalone"
done
rm -rf "$STD_O"
