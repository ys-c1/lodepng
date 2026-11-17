#!/bin/sh
# Builds and runs the resumable-decode test (lodepng_resume_test.cpp) without
# touching the repo Makefile. Uses the same warning flags as the Makefile.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
$CXX -I . -W -Wall -Wextra -ansi -pedantic -O3 -c lodepng.cpp -o lodepng_resume_lib.o
$CXX -I . -W -Wall -Wextra -ansi -pedantic -O3 -c lodepng_resume_test.cpp -o lodepng_resume_test.o
$CXX lodepng_resume_lib.o lodepng_resume_test.o -W -Wall -Wextra -ansi -pedantic -O3 -o resume_test
rm -f lodepng_resume_lib.o lodepng_resume_test.o
./resume_test
