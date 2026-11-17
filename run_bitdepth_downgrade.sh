#!/bin/sh
# Run the 16-bit -> 8-bit downgrade test/benchmark end to end.
# Uses only files shipped in this repository (lodepng + C++ standard
# library); does not touch the Makefile, the library files or any public
# interface. All generated artifacts go under ./artifacts/downgrade_run/.

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

WORK="artifacts/downgrade_run"
RUN1="$WORK/run1"
RUN2="$WORK/run2"
rm -rf "$WORK"
mkdir -p "$RUN1" "$RUN2"

CXX="${CXX:-g++}"
CXXFLAGS="-W -Wall -Wextra -ansi -pedantic -O3"

echo "== build: library files used as shipped, no modification =="
$CXX $CXXFLAGS -I . bitdepth_downgrade_test.cpp lodepng.cpp -o "$WORK/bitdepth_downgrade_test"
echo "build OK (no warnings/errors)"
echo

echo "== run 1 =="
"$WORK/bitdepth_downgrade_test" "$RUN1" | tee "$WORK/report1.txt"
echo

echo "== run 2: separate process, same inputs and config =="
"$WORK/bitdepth_downgrade_test" "$RUN2" > "$WORK/report2.txt"
cat "$WORK/report2.txt"
echo

echo "== cross-process consistency =="
# Only the timing column (last field of the main metrics rows) and the
# output directory name may differ; normalize both, then the two reports
# must be identical line for line.
normalize_report() {
    sed -E -e '/^(low-contrast|high-contrast|noisy|gradient) / s/[[:space:]]+[0-9.]+$//' \
           -e "s#$RUN1#OUTDIR#g" -e "s#$RUN2#OUTDIR#g" "$1"
}
normalize_report "$WORK/report1.txt" > "$WORK/r1_stable.txt"
normalize_report "$WORK/report2.txt" > "$WORK/r2_stable.txt"
if ! diff -u "$WORK/r1_stable.txt" "$WORK/r2_stable.txt" > "$WORK/report.diff"; then
    echo "FAIL: report metrics differ between runs"
    cat "$WORK/report.diff"
    exit 1
fi
echo "reports (excluding timing row): identical"

# The 8-bit PNG artifacts must be byte identical across runs.
if ! diff -r "$RUN1" "$RUN2" > "$WORK/png.diff"; then
    echo "FAIL: PNG output differs between runs"
    cat "$WORK/png.diff"
    exit 1
fi
echo "8-bit PNG artifacts: byte identical"
sha256sum "$RUN1"/*.png | tee "$WORK/sha256.txt"
echo

echo "== parameter out-of-range rejection: must exit non-zero =="
rej_fail=0
check_reject() {
    if "$WORK/bitdepth_downgrade_test" "$WORK/should_not_exist" "$@" \
            > "$WORK/rej.out" 2>&1; then
        echo "FAIL: invalid parameters accepted: $*"
        rej_fail=1
    else
        echo "OK rejected: $*"
    fi
}
check_reject -0.01 0.995 2 1.0
check_reject 0.005 1.1 2 1.0
check_reject 0.5 0.5 2 1.0
check_reject 0.005 0.995 4 1.0
check_reject 0.005 0.995 2 -0.1
check_reject 0.005 0.995 2 1.5
if [ "$rej_fail" -ne 0 ]; then
    exit 1
fi
echo "all invalid parameter sets rejected"
echo

echo "== existing repository unit tests =="
make unittest >/dev/null
if ./unittest 2>&1 | tee "$WORK/unittest.log" | grep -q "test successful"; then
    echo "repository unittest: PASS"
else
    echo "repository unittest: FAIL"
    exit 1
fi
echo
echo "DONE: artifacts in $WORK"
