#!/bin/sh
# One-shot build + test runner for the BLC1 block-level codec addition.
#
# It does not touch the repository Makefile or any LodePNG source. It builds
# the existing upstream unit test exactly as the Makefile does, then builds the
# new self-contained block codec test against the unmodified lodepng.cpp, using
# the same warning/strictness flags the project uses.
#
# Usage:  ./run_blockcodec.sh [--block N] [--threshold P]
# Exits non-zero if any test fails.
set -e

cd "$(dirname "$0")"

CXX="${CXX:-g++}"
FLAGS="-W -Wall -Wextra -ansi -pedantic -O3 -I ./"

echo "################ [1/2] upstream LodePNG unit test ################"
$CXX $FLAGS -c lodepng.cpp -o lodepng.o
$CXX $FLAGS -c lodepng_util.cpp -o lodepng_util.o
$CXX $FLAGS -c lodepng_unittest.cpp -o lodepng_unittest.o
$CXX $FLAGS lodepng.o lodepng_util.o lodepng_unittest.o -o unittest
./unittest

echo
echo "################ [2/2] BLC1 block codec test ####################"
$CXX $FLAGS blockcodec_test.cpp lodepng.cpp -o blockcodec_test
./blockcodec_test "$@"

echo
echo "ALL RUNNERS OK"
