/*
 * bitdepth_downgrade_test.cpp
 *
 * High quality 16-bit -> 8-bit grayscale bit depth downgrade, test and
 * benchmark for LodePNG. Self contained: depends only on the shipped
 * lodepng (lodepng.h / lodepng.cpp) and the C++ standard library. No new
 * third party dependency, no network access.
 *
 * Pipeline (three stages, all configurable):
 *   1. histogram based quantile range mapping (q_lo, q_hi configurable)
 *   2. ordered Bayer dithering to break quantization steps
 *      (pattern: none / 2x2 / 4x4 / 8x8)
 *   3. Floyd-Steinberg error diffusion to correct local bias
 *      (diffusion strength 0.0 .. 1.0)
 *
 * The program:
 *   - builds four small synthetic 16-bit samples (low contrast, high
 *     contrast, noisy, gradient), encodes them with lodepng and decodes
 *     them back with the unmodified lodepng decoder (decoder output is
 *     checked pixel by pixel against the generated pixels);
 *   - runs the downgrade twice and requires bit identical results;
 *   - reports tone loss, PSNR against the original 16-bit data (in the
 *     common 8-bit mapped domain), local bias, banding and timing;
 *   - exercises every configurable knob (quantile interval, dither
 *     pattern, diffusion strength) and rejects out of range parameters
 *     with an error code.
 *
 * Usage:
 *   bitdepth_downgrade_test <outdir> [q_lo q_hi dither_pattern diffusion]
 *   dither_pattern: 0 = none, 1 = Bayer2x2, 2 = Bayer4x4, 3 = Bayer8x8
 */

#include "lodepng.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace {

/* ----------------------------- configuration ---------------------------- */

struct Config {
  double q_lo;            /* lower quantile, must be in [0, 1] */
  double q_hi;            /* upper quantile, must be in [0, 1] */
  int dither_pattern;     /* 0 none, 1 bayer2, 2 bayer4, 3 bayer8 */
  double diffusion;       /* Floyd-Steinberg strength, [0, 1] */
};

/* Returns 0 when valid, non zero error code otherwise. */
int configValidate(const Config& cfg) {
  if (!(cfg.q_lo >= 0.0 && cfg.q_lo <= 1.0)) return 1;
  if (!(cfg.q_hi >= 0.0 && cfg.q_hi <= 1.0)) return 2;
  if (!(cfg.q_lo < cfg.q_hi)) return 3;
  if (cfg.dither_pattern < 0 || cfg.dither_pattern > 3) return 4;
  if (!(cfg.diffusion >= 0.0 && cfg.diffusion <= 1.0)) return 5;
  return 0;
}

const char* patternName(int p) {
  static const char* names[4] = {"none", "bayer2x2", "bayer4x4", "bayer8x8"};
  return names[p];
}

/* ------------------------- deterministic noise -------------------------- */

unsigned lcgNext(unsigned& state) {
  state = state * 1103515245u + 12345u;
  return state;
}

int clamp16(int v) {
  if (v < 0) return 0;
  if (v > 65535) return 65535;
  return v;
}

void put16be(std::vector<unsigned char>& buf, int v) {
  v = clamp16(v);
  buf.push_back((unsigned char)((v >> 8) & 255));
  buf.push_back((unsigned char)(v & 255));
}

/* ------------------------- ordered dither tables ------------------------ */

const int BAYER2[2][2] = {
  {0, 2},
  {3, 1}
};

const int BAYER4[4][4] = {
  { 0,  8,  2, 10},
  {12,  4, 14,  6},
  { 3, 11,  1,  9},
  {15,  7, 13,  5}
};

const int BAYER8[8][8] = {
  { 0, 32,  8, 40,  2, 34, 10, 42},
  {48, 16, 56, 24, 50, 18, 58, 26},
  {12, 44,  4, 36, 14, 46,  6, 38},
  {60, 28, 52, 20, 62, 30, 54, 22},
  { 3, 35, 11, 43,  1, 33,  9, 41},
  {51, 19, 59, 27, 49, 17, 57, 25},
  {15, 47,  7, 39, 13, 45,  5, 37},
  {63, 31, 55, 23, 61, 29, 53, 21}
};

/* Threshold in output level units, centered around zero: [-0.5, 0.5). */
double bayerThreshold(int pattern, int x, int y) {
  if (pattern == 0) return 0.0;
  if (pattern == 1) {
    int v = BAYER2[y & 1][x & 1];
    return ((double)v + 0.5) / 4.0 - 0.5;
  }
  if (pattern == 2) {
    int v = BAYER4[y & 3][x & 3];
    return ((double)v + 0.5) / 16.0 - 0.5;
  }
  int v = BAYER8[y & 7][x & 7];
  return ((double)v + 0.5) / 64.0 - 0.5;
}

/* ------------------------ stage 1: range mapping ------------------------ */

void computeRange(const std::vector<int>& src16, size_t n,
                  double q_lo, double q_hi, unsigned& lo, unsigned& hi) {
  std::vector<unsigned> hist(65536, 0);
  for (size_t i = 0; i < n; ++i) hist[src16[i]]++;

  /* nearest rank quantile, rank in [1, n] */
  unsigned long target_lo = (unsigned long)(q_lo * (double)n + 1e-9);
  unsigned long target_hi = (unsigned long)(q_hi * (double)n + 1e-9);
  if (target_lo < 1) target_lo = 1;
  if (target_hi > n) target_hi = n;

  lo = 0;
  hi = 65535;
  unsigned long acc = 0;
  bool got_lo = false;
  for (int v = 0; v < 65536; ++v) {
    acc += hist[v];
    if (!got_lo && acc >= target_lo) { lo = (unsigned)v; got_lo = true; }
    if (acc >= target_hi) { hi = (unsigned)v; break; }
  }
}

/* Range mapped value in the 8-bit output domain, before quantization. */
double mapLevel(int v, unsigned lo, unsigned hi) {
  if (hi <= lo) return 128.0;
  if (v <= (int)lo) return 0.0;
  if (v >= (int)hi) return 255.0;
  return ((double)(v - (int)lo)) * 255.0 / (double)(hi - (int)lo);
}

/* ------------------- the three stage downgrade routine ------------------ */

/*
 * src: 16-bit grayscale, big endian (exactly the lodepng decoder output),
 *      w * h * 2 bytes.
 * dst: w * h 8-bit samples.
 * Returns 0 on success, non zero error code (see configValidate / null
 * argument checks) otherwise; dst is left untouched on error.
 */
int downgrade16to8(const unsigned char* src, int w, int h,
                   const Config& cfg, std::vector<unsigned char>& dst) {
  if (src == 0 || w <= 0 || h <= 0) return 6;
  int err = configValidate(cfg);
  if (err != 0) return err;

  size_t n = (size_t)w * (size_t)h;

  /* stage 1: histogram + quantile based range mapping */
  std::vector<int> src16(n);
  for (size_t i = 0; i < n; ++i) {
    src16[i] = ((int)src[2 * i] << 8) | (int)src[2 * i + 1];
  }
  unsigned lo, hi;
  computeRange(src16, n, cfg.q_lo, cfg.q_hi, lo, hi);

  /* stage 2 + 3: ordered dither, then Floyd-Steinberg error diffusion */
  std::vector<double> errCur((size_t)w, 0.0);
  std::vector<double> errNxt((size_t)w, 0.0);
  dst.resize(n);

  for (int y = 0; y < h; ++y) {
    std::fill(errNxt.begin(), errNxt.end(), 0.0);
    for (int x = 0; x < w; ++x) {
      size_t idx = (size_t)y * (size_t)w + (size_t)x;
      double mapped = mapLevel(src16[idx], lo, hi);
      double target = mapped + bayerThreshold(cfg.dither_pattern, x, y)
                      + errCur[x];
      int q = (int)std::floor(target + 0.5);
      if (q < 0) q = 0;
      if (q > 255) q = 255;
      dst[idx] = (unsigned char)q;

      double e = (target - (double)q) * cfg.diffusion;
      if (x + 1 < w) errCur[(size_t)x + 1] += e * (7.0 / 16.0);
      errNxt[(size_t)x] += e * (5.0 / 16.0);
      if (x > 0) errNxt[(size_t)x - 1] += e * (3.0 / 16.0);
      if (x + 1 < w) errNxt[(size_t)x + 1] += e * (1.0 / 16.0);
    }
    errCur.swap(errNxt);
  }
  return 0;
}

/* ------------------------------ metrics --------------------------------- */

struct Metrics {
  size_t in_levels;       /* distinct 16-bit levels in source */
  size_t out_levels;      /* distinct 8-bit levels in output */
  size_t trunc_levels;    /* distinct levels a naive v >> 8 would give */
  double tone_loss_pct;   /* 100 * (1 - out_levels / min(in_levels, 256)) */
  double psnr8_db;        /* vs direct range mapped quantization, 8-bit */
  double psnr16_db;       /* vs original 16-bit pixels, inverse mapped */
  double mean_error;      /* signed mean(output - reference), bias */
  int max_abs_error;      /* max |output - reference| */
  size_t max_plateau;     /* longest run of equal output values (banding) */
};

Metrics computeMetrics(const unsigned char* src, int w, int h,
                       const std::vector<unsigned char>& dst,
                       const Config& cfg) {
  size_t n = (size_t)w * (size_t)h;
  std::vector<int> src16(n);
  std::vector<unsigned char> seenIn(65536, 0);
  std::vector<unsigned char> seenOut(256, 0);
  std::vector<unsigned char> seenTrunc(256, 0);

  for (size_t i = 0; i < n; ++i) {
    int v = ((int)src[2 * i] << 8) | (int)src[2 * i + 1];
    src16[i] = v;
    seenIn[v] = 1;
    seenOut[dst[i]] = 1;
    seenTrunc[(v >> 8) & 255] = 1;
  }
  unsigned lo, hi;
  computeRange(src16, n, cfg.q_lo, cfg.q_hi, lo, hi);

  size_t in_levels = 0, out_levels = 0, trunc_levels = 0;
  for (int i = 0; i < 65536; ++i) in_levels += seenIn[i];
  for (int i = 0; i < 256; ++i) {
    out_levels += seenOut[i];
    trunc_levels += seenTrunc[i];
  }

  double se = 0.0;
  double se16 = 0.0;
  double sum_err = 0.0;
  int max_abs = 0;
  double span = (double)(hi - lo);
  for (size_t i = 0; i < n; ++i) {
    int ref = (int)std::floor(mapLevel(src16[i], lo, hi) + 0.5);
    if (ref < 0) ref = 0;
    if (ref > 255) ref = 255;
    int d = (int)dst[i] - ref;
    double dd = (double)d;
    se += dd * dd;
    sum_err += dd;
    int absd = d < 0 ? -d : d;
    if (absd > max_abs) max_abs = absd;

    /* inverse map the 8-bit output back to the 16-bit domain and compare
     * against the original 16-bit pixel over the full 0..65535 scale */
    double recon16 = span > 0.0
        ? (double)lo + (double)dst[i] * span / 255.0
        : (double)lo;
    double e16 = recon16 - (double)src16[i];
    se16 += e16 * e16;
  }
  double mse = se / (double)n;
  double psnr = mse <= 1e-12 ? 999.0 : 10.0 * std::log10((255.0 * 255.0) / mse);
  double mse16 = se16 / (double)n;
  double psnr16 = mse16 <= 1e-12
      ? 999.0 : 10.0 * std::log10((65535.0 * 65535.0) / mse16);

  size_t max_plateau = 1;
  size_t run = 1;
  for (size_t i = 1; i < n; ++i) {
    if (dst[i] == dst[i - 1]) {
      ++run;
      if (run > max_plateau) max_plateau = run;
    } else {
      run = 1;
    }
  }

  Metrics m;
  m.in_levels = in_levels;
  m.out_levels = out_levels;
  m.trunc_levels = trunc_levels;
  size_t cap = in_levels < 256 ? in_levels : 256;
  m.tone_loss_pct = 100.0 * (1.0 - (double)out_levels / (double)cap);
  if (m.tone_loss_pct < 0.0) m.tone_loss_pct = 0.0;
  m.psnr8_db = psnr;
  m.psnr16_db = psnr16;
  m.mean_error = sum_err / (double)n;
  m.max_abs_error = max_abs;
  m.max_plateau = max_plateau;
  return m;
}

/* --------------------------- sample generation -------------------------- */

const int SAMPLE_W = 64;
const int SAMPLE_H = 64;

/* kind: 0 low contrast, 1 high contrast, 2 noisy, 3 gradient */
std::vector<unsigned char> makeSample(int kind) {
  std::vector<unsigned char> raw;
  raw.reserve((size_t)SAMPLE_W * SAMPLE_H * 2);
  unsigned seed = 2463534242u + (unsigned)kind * 2654435761u;

  for (int y = 0; y < SAMPLE_H; ++y) {
    for (int x = 0; x < SAMPLE_W; ++x) {
      int v = 0;
      if (kind == 0) {
        /* narrow window around mid gray: naive truncation collapses it */
        int noise = (int)(lcgNext(seed) % 801) - 400;
        v = 32768 + (x - 32) * 25 + noise;
      } else if (kind == 1) {
        /* full range ramp plus hard stripes: spans almost 0..65535 */
        int ramp = x * 65535 / (SAMPLE_W - 1);
        int stripe = ((x / 8) & 1) ? 12000 : -12000;
        v = ramp + stripe;
      } else if (kind == 2) {
        /* smooth vertical ramp with heavy deterministic noise */
        int ramp = y * 65535 / (SAMPLE_H - 1);
        int noise = (int)(lcgNext(seed) % 6001) - 3000;
        v = ramp + noise;
      } else {
        /* smooth raster order gradient, one level per pixel */
        size_t idx = (size_t)y * SAMPLE_W + (size_t)x;
        v = (int)(idx * 65535 / ((size_t)SAMPLE_W * SAMPLE_H - 1));
      }
      put16be(raw, v);
    }
  }
  return raw;
}

const char* sampleName(int kind) {
  static const char* names[4] = {
    "low-contrast", "high-contrast", "noisy", "gradient"
  };
  return names[kind];
}

/* Encode 16-bit gray with lodepng, decode it back, require a pixel exact
 * round trip: proves the (unmodified) decoder output feeding the pipeline
 * is unchanged pixel by pixel. */
int roundTripDecode(const std::vector<unsigned char>& raw, int w, int h,
                    std::vector<unsigned char>& decoded) {
  unsigned char* png = 0;
  size_t pngsize = 0;
  unsigned err = lodepng_encode_memory(&png, &pngsize,
                                       &raw[0], (unsigned)w, (unsigned)h,
                                       LCT_GREY, 16);
  if (err != 0) {
    std::cerr << "encode failed: " << lodepng_error_text(err) << "\n";
    return 1;
  }
  unsigned char* out = 0;
  unsigned ow = 0, oh = 0;
  err = lodepng_decode_memory(&out, &ow, &oh, png, pngsize, LCT_GREY, 16);
  std::free(png);
  if (err != 0) {
    std::cerr << "decode failed: " << lodepng_error_text(err) << "\n";
    return 2;
  }
  if (ow != (unsigned)w || oh != (unsigned)h ||
      std::memcmp(out, &raw[0], raw.size()) != 0) {
    std::free(out);
    return 3;
  }
  decoded.assign(out, out + (size_t)ow * oh * 2);
  std::free(out);
  return 0;
}

int writeGray8Png(const std::string& path,
                  const std::vector<unsigned char>& px, int w, int h) {
  unsigned char* png = 0;
  size_t pngsize = 0;
  unsigned err = lodepng_encode_memory(&png, &pngsize,
                                       &px[0], (unsigned)w, (unsigned)h,
                                       LCT_GREY, 8);
  if (err != 0) {
    std::cerr << "8-bit encode failed: " << lodepng_error_text(err) << "\n";
    return 1;
  }
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == 0) {
    std::free(png);
    std::cerr << "cannot open " << path << "\n";
    return 2;
  }
  std::fwrite(png, 1, pngsize, f);
  std::fclose(f);
  std::free(png);
  return 0;
}

/* ------------------------------ self tests ------------------------------ */

int expectRejected(const Config& cfg, const unsigned char* src, int w, int h,
                   const char* label) {
  std::vector<unsigned char> dst;
  int err = downgrade16to8(src, w, h, cfg, dst);
  if (err == 0) {
    std::cerr << "SELFTEST FAIL: invalid config accepted (" << label << ")\n";
    return 1;
  }
  std::cout << "  reject[" << label << "] -> error " << err << " OK\n";
  return 0;
}

int selfTests(const std::vector<unsigned char>& sample) {
  std::cout << "[selftest] parameter validation, out of range must error\n";
  const unsigned char* src = &sample[0];
  Config base = {0.005, 0.995, 2, 1.0};

  Config c = base; c.q_lo = -0.01;
  if (expectRejected(c, src, SAMPLE_W, SAMPLE_H, "q_lo < 0")) return 1;
  c = base; c.q_hi = 1.1;
  if (expectRejected(c, src, SAMPLE_W, SAMPLE_H, "q_hi > 1")) return 1;
  c = base; c.q_lo = 0.5; c.q_hi = 0.5;
  if (expectRejected(c, src, SAMPLE_W, SAMPLE_H, "q_lo >= q_hi")) return 1;
  c = base; c.dither_pattern = 4;
  if (expectRejected(c, src, SAMPLE_W, SAMPLE_H, "bad pattern")) return 1;
  c = base; c.diffusion = -0.1;
  if (expectRejected(c, src, SAMPLE_W, SAMPLE_H, "diffusion < 0")) return 1;
  c = base; c.diffusion = 1.01;
  if (expectRejected(c, src, SAMPLE_W, SAMPLE_H, "diffusion > 1")) return 1;
  if (expectRejected(base, 0, SAMPLE_W, SAMPLE_H, "null input")) return 1;
  if (expectRejected(base, src, 0, SAMPLE_H, "zero width")) return 1;

  std::vector<unsigned char> dst;
  if (downgrade16to8(src, SAMPLE_W, SAMPLE_H, base, dst) != 0) {
    std::cerr << "SELFTEST FAIL: valid config rejected\n";
    return 1;
  }
  std::cout << "  valid config accepted OK\n";
  std::cout << "[selftest] all parameter checks passed\n\n";
  return 0;
}

} /* namespace */

int main(int argc, char** argv) {
  if (argc != 2 && argc != 6) {
    std::cerr << "usage: " << argv[0]
              << " <outdir> [q_lo q_hi dither_pattern diffusion]\n";
    return 2;
  }
  std::string outdir = argv[1];
  Config cfg;
  if (argc == 6) {
    cfg.q_lo = std::atof(argv[2]);
    cfg.q_hi = std::atof(argv[3]);
    cfg.dither_pattern = std::atoi(argv[4]);
    cfg.diffusion = std::atof(argv[5]);
    if (configValidate(cfg) != 0) {
      std::cerr << "error: parameters out of range "
                << "(need 0 <= q_lo < q_hi <= 1, pattern in 0..3, "
                << "0 <= diffusion <= 1)\n";
      return 2;
    }
  } else {
    cfg.q_lo = 0.005;
    cfg.q_hi = 0.995;
    cfg.dither_pattern = 2;
    cfg.diffusion = 1.0;
  }

  /* build samples and run the validation selftests */
  std::vector<std::vector<unsigned char> > samples(4);
  std::vector<std::vector<unsigned char> > decoded(4);
  for (int k = 0; k < 4; ++k) {
    samples[k] = makeSample(k);
    if (roundTripDecode(samples[k], SAMPLE_W, SAMPLE_H, decoded[k]) != 0) {
      std::cerr << "pixel exact decoder round trip failed for "
                << sampleName(k) << "\n";
      return 3;
    }
  }
  std::cout << "[decoder] 4 samples encoded + decoded with lodepng, "
               "pixel exact round trip: PASS\n\n";
  if (selfTests(decoded[0]) != 0) return 4;

  const int REPS = 100;
  std::cout << "config: q_lo=" << cfg.q_lo << " q_hi=" << cfg.q_hi
            << " pattern=" << patternName(cfg.dither_pattern)
            << " diffusion=" << cfg.diffusion << "\n";
  std::cout << "samples: " << SAMPLE_W << "x" << SAMPLE_H
            << " 16-bit grayscale, timing over " << REPS
            << " repetitions\n\n";

  std::cout << "== main metrics (full pipeline) ==\n";
  std::cout << "sample         inLvl outLvl truncLvl toneLoss%  "
               "PSNR8dB PSNR16dB  meanErr maxAbsErr maxPlateau  us/call\n";
  for (int k = 0; k < 4; ++k) {
    std::vector<unsigned char> dst1, dst2;
    clock_t t0 = std::clock();
    for (int r = 0; r < REPS; ++r) {
      int e = downgrade16to8(&decoded[k][0], SAMPLE_W, SAMPLE_H, cfg, dst1);
      if (e != 0) {
        std::cerr << "downgrade failed: " << e << "\n";
        return 5;
      }
    }
    clock_t t1 = std::clock();
    /* second independent run, must be bit identical */
    if (downgrade16to8(&decoded[k][0], SAMPLE_W, SAMPLE_H, cfg, dst2) != 0)
      return 5;
    if (dst1.size() != dst2.size() ||
        std::memcmp(&dst1[0], &dst2[0], dst1.size()) != 0) {
      std::cerr << "consistency FAIL on " << sampleName(k) << "\n";
      return 6;
    }

    Metrics m = computeMetrics(&decoded[k][0], SAMPLE_W, SAMPLE_H, dst1, cfg);
    double us_per_call =
        (double)(t1 - t0) * 1000000.0 / (double)CLOCKS_PER_SEC / REPS;
    std::printf("%-13s  %5lu  %5lu  %8lu  %8.2f  %6.2f  %8.2f  %7.4f  %9d  %10lu  %8.1f\n",
                sampleName(k), (unsigned long)m.in_levels,
                (unsigned long)m.out_levels, (unsigned long)m.trunc_levels,
                m.tone_loss_pct, m.psnr8_db, m.psnr16_db, m.mean_error,
                m.max_abs_error, (unsigned long)m.max_plateau, us_per_call);

    std::string path = outdir + "/" + sampleName(k) + "_8bit.png";
    if (writeGray8Png(path, dst1, SAMPLE_W, SAMPLE_H) != 0) return 7;
  }
  std::cout << "\n[consistency] in-process double run bit identical: PASS; "
               "8-bit PNGs written to " << outdir << "\n\n";

  /* knob sweep 1: dither pattern effect on banding (gradient, diffusion 0) */
  std::cout << "== dither pattern sweep on gradient (diffusion=0) ==\n";
  std::cout << "pattern    outLvl toneLoss% maxPlateau PSNR8dB PSNR16dB\n";
  for (int p = 0; p <= 3; ++p) {
    Config c2 = cfg;
    c2.dither_pattern = p;
    c2.diffusion = 0.0;
    std::vector<unsigned char> dst;
    if (downgrade16to8(&decoded[3][0], SAMPLE_W, SAMPLE_H, c2, dst) != 0)
      return 5;
    Metrics m = computeMetrics(&decoded[3][0], SAMPLE_W, SAMPLE_H, dst, c2);
    std::printf("%-9s  %5lu  %8.2f  %10lu  %6.2f  %8.2f\n",
                patternName(p), (unsigned long)m.out_levels, m.tone_loss_pct,
                (unsigned long)m.max_plateau, m.psnr8_db, m.psnr16_db);
  }

  /* knob sweep 2: diffusion strength effect on local bias (gradient) */
  std::cout << "\n== diffusion strength sweep on gradient (bayer4x4) ==\n";
  std::cout << "diffusion  PSNR8dB  PSNR16dB  meanErr  maxAbsErr\n";
  double strengths[3] = {0.0, 0.5, 1.0};
  for (int i = 0; i < 3; ++i) {
    Config c2 = cfg;
    c2.dither_pattern = 2;
    c2.diffusion = strengths[i];
    std::vector<unsigned char> dst;
    if (downgrade16to8(&decoded[3][0], SAMPLE_W, SAMPLE_H, c2, dst) != 0)
      return 5;
    Metrics m = computeMetrics(&decoded[3][0], SAMPLE_W, SAMPLE_H, dst, c2);
    std::printf("%-9s  %7.2f  %8.2f  %7.4f  %9d\n",
                i == 0 ? "0.0" : i == 1 ? "0.5" : "1.0",
                m.psnr8_db, m.psnr16_db, m.mean_error, m.max_abs_error);
  }

  /* knob sweep 3: quantile interval effect on the low contrast sample */
  std::cout << "\n== quantile interval sweep on low-contrast ==\n";
  std::cout << "interval       lo->hi (16-bit)   outLvl truncLvl toneLoss%\n";
  double los[3] = {0.0, 0.005, 0.05};
  double his[3] = {1.0, 0.995, 0.95};
  for (int i = 0; i < 3; ++i) {
    Config c2 = cfg;
    c2.q_lo = los[i];
    c2.q_hi = his[i];
    std::vector<unsigned char> dst;
    if (downgrade16to8(&decoded[0][0], SAMPLE_W, SAMPLE_H, c2, dst) != 0)
      return 5;
    Metrics m = computeMetrics(&decoded[0][0], SAMPLE_W, SAMPLE_H, dst, c2);
    std::vector<int> src16((size_t)SAMPLE_W * SAMPLE_H);
    for (size_t j = 0; j < src16.size(); ++j) {
      src16[j] = ((int)decoded[0][2 * j] << 8) | decoded[0][2 * j + 1];
    }
    unsigned lo, hi;
    computeRange(src16, src16.size(), c2.q_lo, c2.q_hi, lo, hi);
    std::printf("%4.3f..%4.3f   %5u -> %5u   %5lu  %8lu  %8.2f\n",
                los[i], his[i], lo, hi, (unsigned long)m.out_levels,
                (unsigned long)m.trunc_levels, m.tone_loss_pct);
  }

  std::cout << "\nALL CHECKS DONE\n";
  return 0;
}
