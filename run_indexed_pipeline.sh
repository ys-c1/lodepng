#!/usr/bin/env bash
# Build and run the indexed-color pipeline benchmark/acceptance test.
# Does not touch the repo's Makefile or any public header.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p artifacts/samples

g++ -std=c++17 -O3 -W -Wall -Wextra -I. \
    indexed_pipeline_bench.cpp lodepng.cpp -o artifacts/indexed_pipeline_bench

./artifacts/indexed_pipeline_bench "$@"
