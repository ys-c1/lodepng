# Single-pass multi-candidate PNG filter selection — verification harness

This directory contains the self-contained correctness and performance tests for
the LFS_MINSUM change in `../lodepng.cpp`. It uses only g++ and glibc `dlfcn` —
no third-party libraries, no network access.

## What is being compared

Two lodepng builds are loaded as separate shared libraries, each in its own
linker namespace (`dlmopen` with `LM_ID_NEWLM`), so identical internal symbol
names never collide:

- `liblodepng_base.so` — pristine pre-change lodepng (the original
  "try all five filters per row" implementation), compiled from
  `../baseline_build/lodepng.cpp`.
- `liblodepng_new.so` — modified lodepng (single-scan selection, SSE2 fast path).
- `liblodepng_new_scalar.so` — same source with `-DLODEPNG_MINSUM_NO_SIMD`,
  forcing the portable scalar fallback so the SIMD and scalar paths can be
  cross-checked against each other.

The harness reaches into each namespace's own libc `free` (buffers allocated in
a dlmopen namespace must be freed there) and resolves the mangled C++ API
symbols with `dlsym`.

## Files

- `driver.cpp` — 200-image row-by-row correctness check (filter type byte plus
  every filtered byte, and byte-identical whole-PNG output) and the four-class
  512x512 encode timing/size benchmark.
- `edge.cpp` — 780 edge-case sizes/color types: widths straddling the 8-byte
  SIMD boundary (1..257), heights of 1..3, 8/16-bit RGB/RGBA/gray/gray-alpha,
  plus sub-byte 1-bit gray and 8-bit palette with MINSUM explicitly enabled.
- `paeth_simd_check.cpp` — exhaustive proof that the integer Paeth-distance
  math used by the SSE2 path matches `paethPredictor` for every one of the
  2^24 `(a,b,c)` inputs, plus the absolute-byte rule over all 256 bytes.
- `micro.cpp` — filter-stage-only timing (`btype=0` stored deflate blocks, so
  the measured cost is the filter selection itself) and full-pipeline timing at
  1024x1024.
- `run_all.sh` — builds all three libraries and runs every check.

## Run

```sh
./run_all.sh
```

All sample images are generated in-process by a deterministic xorshift PRNG
(seeded per image), so a run is fully reproducible and needs no input files.

## Notes on the performance images

The correctness images stay small and varied (1..31 px wide). The performance
images are 512x512 and keep each class's characteristic structure (photo noise,
smooth gradient, hard-edged glyphs, solid color bands) but carry realistic
per-pixel grain. Pure two-color synthetic content makes deflate dominate (>85%
of encode time) and is not representative of real text/flat scans, which contain
anti-aliasing, sensor grain and compression remnants; the grain restores the
filter/deflate balance (~30-45% filter share) that the optimization targets.
The filter code is identical regardless of which generator feeds it.
