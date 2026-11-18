/* Filter-stage-only micro benchmark: btype=0 stored deflate blocks, so the
zlib stage does almost no work and the measured delta is the filter selection.
Also reports the full-pipeline numbers for reference. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lodepng.h"

#include <dlfcn.h>
#include <link.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <vector>

enum { CT_GREY = 0, CT_RGB = 2, CT_PALETTE = 3, CT_GREY_ALPHA = 4, CT_RGBA = 6 };

typedef unsigned (*encode_fn)(unsigned char**, size_t*, const unsigned char*,
                              unsigned, unsigned, LodePNGState*);
typedef void (*state_init_fn)(LodePNGState*);
typedef void (*state_cleanup_fn)(LodePNGState*);
typedef void (*libc_free_fn)(void*);

struct Lib {
  void* handle;
  encode_fn encode;
  state_init_fn state_init;
  state_cleanup_fn state_cleanup;
  libc_free_fn lib_free;
};

static Lib load_lib(const char* path) {
  Lib lib;
  std::memset(&lib, 0, sizeof(lib));
  lib.handle = dlmopen(LM_ID_NEWLM, path, RTLD_NOW | RTLD_LOCAL);
  if(!lib.handle) { std::fprintf(stderr, "dlmopen: %s\n", dlerror()); std::exit(2); }
  lib.encode = (encode_fn)dlsym(lib.handle, "_Z14lodepng_encodePPhPmPKhjjP12LodePNGState");
  lib.state_init = (state_init_fn)dlsym(lib.handle, "_Z18lodepng_state_initP12LodePNGState");
  lib.state_cleanup = (state_cleanup_fn)dlsym(lib.handle, "_Z21lodepng_state_cleanupP12LodePNGState");
  lib.lib_free = (libc_free_fn)dlsym(lib.handle, "free");
  if(!lib.encode || !lib.state_init || !lib.state_cleanup || !lib.lib_free) std::exit(2);
  return lib;
}

static uint64_t rng_state;
static unsigned rng_next() {
  rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
  return (unsigned)(rng_state >> 16);
}

static double now_sec() {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned channels_of(int ct) {
  static const unsigned c[7] = {1, 0, 3, 1, 2, 0, 4};
  return c[ct];
}

static std::vector<unsigned char> gen(unsigned w, unsigned h, int cls, int ct) {
  unsigned ch = channels_of(ct);
  std::vector<unsigned char> img((size_t)w * h * ch);
  unsigned maxv = 255;
  for(unsigned y = 0; y < h; ++y)
    for(unsigned x = 0; x < w; ++x) {
      unsigned char* px = &img[((size_t)y * w + x) * ch];
      for(unsigned c = 0; c < ch; ++c) {
        unsigned v;
        switch(cls) {
          case 0: v = (rng_next() + ((x * 7 + y * 13) & 0xff) + c * 17) & maxv; break;       /* photo */
          case 1: v = ((x * maxv / w) + (y * maxv / h) / 2 + c * 31) & maxv; break;          /* gradient */
          case 2: v = (((x / 3 + y / 5) % 7 < 3) ? maxv : maxv / 8); break;                  /* text */
          default: v = ((x / (w / 3 + 1) + y / (h / 2 + 1)) * 64 + c * 64) & maxv; break;    /* flat */
        }
        px[c] = (unsigned char)v;
      }
    }
  return img;
}

static double run(const Lib& lib, const std::vector<unsigned char>& img, unsigned w, unsigned h,
                  int ct, unsigned iters, int stored) {
  double best = 1e9;
  for(unsigned rep = 0; rep < 3; ++rep) {
    double t0 = now_sec();
    for(unsigned k = 0; k < iters; ++k) {
      LodePNGState st; lib.state_init(&st);
      st.info_raw.colortype = (LodePNGColorType)ct;
      st.info_raw.bitdepth = 8;
      st.encoder.filter_strategy = LFS_MINSUM;
      st.encoder.auto_convert = 0;
      st.info_png.color.colortype = (LodePNGColorType)ct;
      st.info_png.color.bitdepth = 8;
      if(stored) { st.encoder.zlibsettings.btype = 0; st.encoder.zlibsettings.use_lz77 = 0; }
      unsigned char* out = 0; size_t osz = 0;
      lib.encode(&out, &osz, img.data(), w, h, &st);
      lib.lib_free(out);
      lib.state_cleanup(&st);
    }
    double dt = now_sec() - t0;
    if(dt < best) best = dt;
  }
  return best;
}

int main(int argc, char** argv) {
  const char* bp = argc > 1 ? argv[1] : "./liblodepng_base.so";
  const char* np = argc > 2 ? argv[2] : "./liblodepng_new.so";
  Lib base = load_lib(bp), nlib = load_lib(np);
  const unsigned W = 1024, H = 1024;
  struct Cfg { int cls; int ct; const char* name; } cfgs[4] = {
    {0, CT_RGBA, "photo RGBA8"}, {1, CT_RGB, "gradient RGB8"},
    {2, CT_RGBA, "text RGBA8"}, {3, CT_RGB, "flat RGB8"},
  };
  for(int stored = 1; stored >= 0; --stored) {
    std::printf("=== %s (1024x1024, best of 3) ===\n", stored ? "filter stage (btype=0 stored)" : "full pipeline (default deflate)");
    std::printf("%-14s %-12s %-12s %-10s\n", "image", "base", "new", "time cut");
    double tb = 0, tn = 0;
    for(int i = 0; i < 4; ++i) {
      rng_state = 0xabcdef0123456789ULL ^ (uint64_t)(i + 1) * 0x9e3779b97f4a7c15ULL;
      std::vector<unsigned char> img = gen(W, H, cfgs[i].cls, cfgs[i].ct);
      unsigned iters = stored ? 30 : 12;
      double b = run(base, img, W, H, cfgs[i].ct, iters, stored);
      double n = run(nlib, img, W, H, cfgs[i].ct, iters, stored);
      tb += b; tn += n;
      std::printf("%-14s %-12.4f %-12.4f %-9.2f%%\n", cfgs[i].name, b, n, 100.0 * (1 - n / b));
    }
    std::printf("%-14s %-12.4f %-12.4f %-9.2f%%\n\n", "TOTAL", tb, tn, 100.0 * (1 - tn / tb));
  }
  return 0;
}
