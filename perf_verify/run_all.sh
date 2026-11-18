#!/usr/bin/env bash
# Builds the three lodepng variants and runs every verification in one shot.
#   liblodepng_base.so       : pristine pre-change lodepng (from ../baseline_build snapshot)
#   liblodepng_new.so        : modified lodepng, normal build (SSE2 where available)
#   liblodepng_new_scalar.so: modified lodepng with LODEPNG_MINSUM_NO_SIMD (portable fallback)
#
# No third-party libraries and no network access are used; only g++, glibc dlfcn.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=..
CXX=${CXX:-g++}
FLAGS="-W -Wall -Wextra -O3 -std=c++11"

echo ">> building shared libraries"
$CXX -I "$ROOT" $FLAGS -fPIC -shared "$ROOT/baseline_build/lodepng.cpp" -o liblodepng_base.so
$CXX -I "$ROOT" $FLAGS -fPIC -shared "$ROOT/lodepng.cpp"            -o liblodepng_new.so
$CXX -I "$ROOT" $FLAGS -fPIC -shared -DLODEPNG_MINSUM_NO_SIMD "$ROOT/lodepng.cpp" -o liblodepng_new_scalar.so

echo ">> exhaustive Paeth / abs arithmetic proof (2^24 + 256 inputs)"
$CXX -I "$ROOT" $FLAGS paeth_simd_check.cpp -o paeth_simd_check
./paeth_simd_check

echo ">> building harnesses"
$CXX -I "$ROOT" $FLAGS driver.cpp -o verify_bench -ldl -L. -llodepng_new -Wl,-rpath,"$PWD"
$CXX -I "$ROOT" $FLAGS edge.cpp   -o edge         -ldl

echo
echo ">> [1/4] 200-image row-by-row + size/timing, new(SIMD) vs pristine baseline"
./verify_bench "$PWD/liblodepng_base.so" "$PWD/liblodepng_new.so"

echo
echo ">> [2/4] 200-image equivalence, forced-scalar new vs SIMD new"
./verify_bench "$PWD/liblodepng_new_scalar.so" "$PWD/liblodepng_new.so" | sed -n '1,4p'

echo
echo ">> [3/4] 780 edge-case sizes/color types, SIMD vs baseline"
./edge "$PWD/liblodepng_base.so" "$PWD/liblodepng_new.so"

echo
echo ">> [4/4] 780 edge-case sizes/color types, scalar vs SIMD"
./edge "$PWD/liblodepng_new_scalar.so" "$PWD/liblodepng_new.so"
