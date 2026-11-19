/*Performance comparison: old (pre-refactor) vs new (refactored) decoder on a 1024x1024 image.
Decodes the same PNG alternately with both implementations and reports wall-clock times.*/
#include "../lodepng.h"
#include "old/oldpng.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

static double now_ms(void) {
  return std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(void) {
  const unsigned w = 1024, h = 1024;

  /*realistic content: smooth gradient + texture so the PNG uses real filters*/
  std::vector<unsigned char> raw((size_t)w * h * 4);
  for(unsigned y = 0; y < h; ++y) {
    for(unsigned x = 0; x < w; ++x) {
      size_t i = 4 * ((size_t)y * w + x);
      raw[i + 0] = (unsigned char)((x + y) & 255);
      raw[i + 1] = (unsigned char)((x * x / 16 + y) & 255);
      raw[i + 2] = (unsigned char)((x * y / 64 + x / 3) & 255);
      raw[i + 3] = (unsigned char)(200 + (x % 7) * 8);
    }
  }

  std::vector<unsigned char> png;
  unsigned e = lodepng::encode(png, raw, w, h, LCT_RGBA, 8);
  if(e) { std::fprintf(stderr, "encode failed: %u\n", e); return 2; }
  std::printf("1024x1024 RGBA8, PNG size: %zu bytes\n", png.size());

  const int rounds = 40;
  std::vector<double> told, tnew;
  std::vector<unsigned char> out;
  unsigned ow, oh;

  /*warmup*/
  for(int i = 0; i < 3; ++i) {
    unsigned eo = oldlodepng::decode(out, ow, oh, png, OLDLCT_RGBA, 8);
    unsigned en = lodepng::decode(out, ow, oh, png, LCT_RGBA, 8);
    if(eo || en) { std::fprintf(stderr, "decode failed: %u %u\n", eo, en); return 2; }
  }

  for(int r = 0; r < rounds; ++r) {
    double t0 = now_ms();
    oldlodepng::decode(out, ow, oh, png, OLDLCT_RGBA, 8);
    double t1 = now_ms();
    lodepng::decode(out, ow, oh, png, LCT_RGBA, 8);
    double t2 = now_ms();
    told.push_back(t1 - t0);
    tnew.push_back(t2 - t1);
  }

  std::sort(told.begin(), told.end());
  std::sort(tnew.begin(), tnew.end());
  double min_o = told.front(), min_n = tnew.front();
  double med_o = told[rounds / 2], med_n = tnew[rounds / 2];
  double mean_o = 0, mean_n = 0;
  for(int r = 0; r < rounds; ++r) { mean_o += told[r]; mean_n += tnew[r]; }
  mean_o /= rounds; mean_n /= rounds;

  std::printf("old: min %.3f ms  median %.3f ms  mean %.3f ms\n", min_o, med_o, mean_o);
  std::printf("new: min %.3f ms  median %.3f ms  mean %.3f ms\n", min_n, med_n, mean_n);
  std::printf("ratio new/old: min %.3f  median %.3f  mean %.3f  (must be <= 1.10)\n",
              min_n / min_o, med_n / med_o, mean_n / mean_o);
  bool ok = mean_n <= mean_o * 1.10 && med_n <= med_o * 1.10;
  std::printf("RESULT: %s\n", ok ? "PASS (no regression > 10%)" : "FAIL (>10% slower)");
  return ok ? 0 : 1;
}
