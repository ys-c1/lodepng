/*
Equivalence and resumption tests for the incremental PNG decoder.

Generates 200 small PNGs with lodepng's own encoder (no files, no network,
no third-party libraries) and checks that decoding them with the resumable
state machine - in n chunks, across serialized-state reloads and across real
process restarts - produces bytes identical to the one-shot lodepng decoder.

Run: ./incremental_test
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>

#include "lodepng.h"
#include "lodepng_incremental.h"

static unsigned rng_state = 0x12345678u;
static unsigned xrnd() {
  /* xorshift32, deterministic across runs and machines */
  unsigned x = rng_state;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  rng_state = x;
  return x;
}

static std::string to_str(unsigned v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%u", v);
  return std::string(buf);
}

struct Sample {
  std::vector<unsigned char> png;
  unsigned w, h;
  int colortype;   /* PNG color type */
  int bitdepth;
  int btype;       /* deflate block type used by the encoder */
  int filter;      /* filter strategy */
};

/* Encode raw pixels of the given color type with chosen deflate/filter mode. */
static unsigned encode_sample(std::vector<unsigned char>& png,
                              const std::vector<unsigned char>& raw,
                              unsigned w, unsigned h,
                              LodePNGColorType ct, unsigned bd,
                              int btype, int filter) {
  lodepng::State state;
  unsigned char* out = 0;
  size_t outsize = 0;
  unsigned err;
  state.encoder.zlibsettings.btype = (unsigned)btype;
  state.encoder.filter_strategy = (LodePNGFilterStrategy)filter;
  state.encoder.auto_convert = 0; /* keep the requested color type */
  state.info_raw.colortype = ct;
  state.info_raw.bitdepth = bd;
  state.info_png.color.colortype = ct;
  state.info_png.color.bitdepth = bd;
  err = lodepng_encode(&out, &outsize, raw.empty() ? 0 : &raw[0], w, h, &state);
  if(!err) png.assign(out, out + outsize);
  std::free(out);
  return err;
}

static void build_samples(std::vector<Sample>& samples) {
  /* (colortype, bitdepth) pairs that are valid PNG combinations */
  static const int modes[][2] = {
    {0, 1}, {0, 2}, {0, 4}, {0, 8}, {0, 16},       /* gray */
    {2, 8}, {2, 16},                                /* RGB */
    {3, 1}, {3, 2}, {3, 4}, {3, 8},                /* palette */
    {4, 8}, {4, 16},                                /* gray+alpha */
    {6, 8}, {6, 16}                                 /* RGBA */
  };
  static const int btypes[] = {0, 1, 2};
  static const int filters[] = {LFS_ZERO, LFS_MINSUM, LFS_ONE, LFS_TWO,
                                LFS_THREE, LFS_FOUR, LFS_ENTROPY};
  static const unsigned widths[] = {16, 17, 23, 31, 32, 48};
  static const unsigned heights[] = {7, 11, 13, 17, 19, 29};
  unsigned i;

  for(i = 0; i < 200; ++i) {
    int m = (int)(xrnd() % (sizeof(modes) / sizeof(modes[0])));
    int ct = modes[m][0], bd = modes[m][1];
    unsigned w = widths[xrnd() % (sizeof(widths) / sizeof(widths[0]))];
    unsigned h = heights[xrnd() % (sizeof(heights) / sizeof(heights[0]))];
    int btype = btypes[i % 3];
    int filter = filters[xrnd() % (sizeof(filters) / sizeof(filters[0]))];
    Sample s;
    s.w = w; s.h = h; s.colortype = ct; s.bitdepth = bd;
    s.btype = btype; s.filter = filter;

    /* Build raw pixels in the PNG's own color type. We synthesize an RGBA
    image first (the encoder accepts matching raw; for palette we encode via
    RGBA with auto palette, so handle that case separately). */
    std::vector<unsigned char> rgba((size_t)w * h * 4);
    unsigned p;
    /* palette targets can only hold 2^bd distinct colors; generate exactly
    that many so the encoder never has to quantize beyond capacity */
    unsigned ncolors = ct == 3 ? (1u << (unsigned)bd) : 0;
    if(ct == 3) {
      unsigned palette_r[256], palette_g[256], palette_b[256];
      unsigned c;
      for(c = 0; c < ncolors; ++c) {
        palette_r[c] = (c * 53 + 11) & 255;
        palette_g[c] = (c * 97 + 29) & 255;
        palette_b[c] = (c * 193 + 7) & 255;
      }
      for(p = 0; p < w * h; ++p) {
        unsigned x = p % w, y = p / w;
        c = (x * 7 + y * 13 + xrnd()) % ncolors;
        rgba[4 * p + 0] = (unsigned char)palette_r[c];
        rgba[4 * p + 1] = (unsigned char)palette_g[c];
        rgba[4 * p + 2] = (unsigned char)palette_b[c];
        rgba[4 * p + 3] = 255;
      }
    } else {
      for(p = 0; p < (unsigned)rgba.size(); ++p) {
        /* structured patterns + noise so all filters have something to do */
        unsigned pix = p / 4, ch = p % 4;
        unsigned x = pix % w, y = pix / w;
        unsigned v = (unsigned)((x * 37 + y * 71 + (int)ch * 13 + xrnd()) & 255);
        if(ch == 3) v = 255;
        rgba[p] = (unsigned char)v;
      }
    }

    unsigned err;
    if(ct == 3) {
      /* palette images: build an indexed raw image plus an explicit palette,
      the encoder wants the palette supplied up front */
      lodepng::State st;
      unsigned char* out = 0; size_t outsize = 0;
      unsigned c;
      st.encoder.zlibsettings.btype = (unsigned)btype;
      st.encoder.filter_strategy = (LodePNGFilterStrategy)filter;
      st.encoder.auto_convert = 0;
      st.info_raw.colortype = LCT_PALETTE;
      st.info_raw.bitdepth = (unsigned)bd;
      st.info_png.color.colortype = LCT_PALETTE;
      st.info_png.color.bitdepth = (unsigned)bd;
      st.info_png.color.palette = (unsigned char*)std::malloc(1024);
      st.info_png.color.palettesize = ncolors;
      for(c = 0; c < ncolors; ++c) {
        st.info_png.color.palette[4 * c + 0] = (unsigned char)((c * 53 + 11) & 255);
        st.info_png.color.palette[4 * c + 1] = (unsigned char)((c * 97 + 29) & 255);
        st.info_png.color.palette[4 * c + 2] = (unsigned char)((c * 193 + 7) & 255);
        st.info_png.color.palette[4 * c + 3] = 255;
      }
      {
        /* pack indices, one per pixel, into the bit-depth sized raw buffer */
        size_t rawsize = lodepng_get_raw_size(w, h, &st.info_png.color);
        std::vector<unsigned char> raw(rawsize, 0);
        for(p = 0; p < w * h; ++p) {
          unsigned x = p % w, y = p / w;
          c = (x * 7 + y * 13 + xrnd()) % ncolors;
          if(bd == 8) raw[p] = (unsigned char)c;
          else {
            size_t bitpos = (size_t)p * bd;
            raw[bitpos / 8] |= (unsigned char)(c << (bitpos % 8));
          }
        }
        err = lodepng_encode(&out, &outsize, &raw[0], w, h, &st);
      }
      if(!err) s.png.assign(out, out + outsize);
      std::free(out);
      st.info_png.color.palettesize = 0; /* stop the state destructor freeing it */
      std::free(st.info_png.color.palette);
      st.info_png.color.palette = 0;
    } else {
      /* convert RGBA8 -> target raw layout with lodepng itself */
      LodePNGColorMode srcmode, dstmode;
      size_t rawsize;
      std::vector<unsigned char> raw;
      lodepng_color_mode_init(&srcmode);
      lodepng_color_mode_init(&dstmode);
      srcmode.colortype = LCT_RGBA; srcmode.bitdepth = 8;
      dstmode.colortype = (LodePNGColorType)ct; dstmode.bitdepth = (unsigned)bd;
      rawsize = lodepng_get_raw_size(w, h, &dstmode);
      raw.resize(rawsize);
      err = lodepng_convert(&raw[0], &rgba[0], &dstmode, &srcmode, w, h);
      lodepng_color_mode_cleanup(&srcmode);
      lodepng_color_mode_cleanup(&dstmode);
      if(!err) err = encode_sample(s.png, raw, w, h,
                                   (LodePNGColorType)ct, (unsigned)bd,
                                   btype, filter);
    }
    if(err) {
      std::printf("sample %u generation failed: err %u (ct %d bd %d)\n",
                  i, err, ct, bd);
      std::exit(1);
    }
    samples.push_back(s);
  }
}

/* one-shot reference decode to RGBA8 */
static unsigned ref_decode(const Sample& s, std::vector<unsigned char>& ref) {
  unsigned char* out = 0; unsigned w = 0, h = 0;
  unsigned err = lodepng_decode32(&out, &w, &h, s.png.empty() ? 0 : &s.png[0],
                                  s.png.size());
  if(!err) {
    if(w != s.w || h != s.h) { std::free(out); return 1000; }
    ref.assign(out, out + (size_t)w * h * 4);
  }
  std::free(out);
  return err;
}

/* plain incremental decode in chunks of rows_per_step (no save/reload) */
static unsigned chunked_decode(const Sample& s, unsigned rows_per_step,
                               std::vector<unsigned char>& got) {
  LodePNGIncremental* d = 0;
  unsigned char* out = 0; unsigned w = 0, h = 0;
  unsigned err = lodepng_inc_begin(&d, &out, &w, &h,
                                   &s.png[0], s.png.size());
  if(err) return err;
  got.resize((size_t)w * h * 4);
  for(;;) {
    unsigned rows = 0;
    err = lodepng_inc_step(d, rows_per_step, &rows);
    if(err == 0) break;
    if(err != 130) { lodepng_inc_destroy(d); return err; }
  }
  std::memcpy(&got[0], out, got.size());
  lodepng_inc_destroy(d);
  std::free(out);
  return 0;
}

/* incremental decode where EVERY step is followed by save -> destroy ->
restore in the same process (serialization round-trip each row block). */
static unsigned reloaded_decode(const Sample& s, unsigned rows_per_step,
                                std::vector<unsigned char>& got) {
  LodePNGIncremental* d = 0;
  unsigned char* out = 0; unsigned w = 0, h = 0;
  unsigned err = lodepng_inc_begin(&d, &out, &w, &h,
                                   &s.png[0], s.png.size());
  if(err) return err;
  got.resize((size_t)w * h * 4);
  for(;;) {
    unsigned rows = 0;
    size_t stsize;
    std::vector<unsigned char> blob;
    err = lodepng_inc_step(d, rows_per_step, &rows);
    if(err != 0 && err != 130) { lodepng_inc_destroy(d); std::free(out); return err; }
    if(err == 0) break;
    stsize = lodepng_inc_state_size(d);
    blob.resize(stsize);
    if(lodepng_inc_save(d, &blob[0]) != stsize) {
      lodepng_inc_destroy(d); std::free(out); return 1001;
    }
    lodepng_inc_destroy(d);
    d = 0;
    err = lodepng_inc_restore(&d, &blob[0], blob.size(),
                              &s.png[0], s.png.size(), out);
    if(err) { std::free(out); return err; }
  }
  std::memcpy(&got[0], out, got.size());
  lodepng_inc_destroy(d);
  std::free(out);
  return 0;
}

/* split into n equal row-block counts; n=2,3,7 per the spec */
static unsigned rows_per_chunk(unsigned h, unsigned n, unsigned chunk_index) {
  unsigned base = h / n, rem = h % n;
  return base + (chunk_index < rem ? 1u : 0u);
}

/* decode using exactly n steps (varying block sizes), reloading state
between every step */
static unsigned nstep_reloaded_decode(const Sample& s, unsigned n,
                                      std::vector<unsigned char>& got) {
  LodePNGIncremental* d = 0;
  unsigned char* out = 0; unsigned w = 0, h = 0;
  unsigned err = lodepng_inc_begin(&d, &out, &w, &h,
                                   &s.png[0], s.png.size());
  if(err) return err;
  got.resize((size_t)w * h * 4);
  {
    unsigned k;
    for(k = 0; k < n; ++k) {
      unsigned rows = 0;
      unsigned rps = rows_per_chunk(h, n, k);
      std::vector<unsigned char> blob;
      if(rps == 0) continue;
      err = lodepng_inc_step(d, rps, &rows);
      if(k + 1 == n) {
        if(err != 0) { lodepng_inc_destroy(d); std::free(out); return err; }
        break; /* last step must finish */
      }
      if(err != 130) { lodepng_inc_destroy(d); std::free(out); return err ? err : 1002; }
      blob.resize(lodepng_inc_state_size(d));
      if(!lodepng_inc_save(d, &blob[0])) {
        lodepng_inc_destroy(d); std::free(out); return 1001;
      }
      lodepng_inc_destroy(d);
      d = 0;
      err = lodepng_inc_restore(&d, &blob[0], blob.size(),
                                &s.png[0], s.png.size(), out);
      if(err) { std::free(out); return err; }
    }
  }
  std::memcpy(&got[0], out, got.size());
  lodepng_inc_destroy(d);
  std::free(out);
  return 0;
}

static int byte_equal(const std::vector<unsigned char>& a,
                      const std::vector<unsigned char>& b) {
  if(a.size() != b.size()) return 0;
  return std::memcmp(&a[0], &b[0], a.size()) == 0;
}

int main(int argc, char** argv) {
  std::vector<Sample> samples;
  unsigned i;
  unsigned failures = 0;
  size_t max_blob = 0;
  unsigned max_blob_idx = 0;
  double rowblock_bytes_max = 0;
  int mode = 0; /* 0 full suite, 1 worker subprocess */
  unsigned worker_idx = 0;
  unsigned worker_step = 1;
  const char* blobpath = 0;

  if(argc >= 2 && std::strcmp(argv[1], "--worker") == 0 && argc == 5) {
    mode = 1;
    worker_idx = (unsigned)std::atoi(argv[2]);
    worker_step = (unsigned)std::atoi(argv[3]);
    blobpath = argv[4];
  }

  build_samples(samples);

  if(mode == 1) {
    /* subprocess: rebuild samples deterministically, advance to the same
    breakpoint, load the parent's state blob, finish, write result marker */
    const Sample& s = samples[worker_idx];
    LodePNGIncremental* d = 0;
    unsigned char* out = 0; unsigned w = 0, h = 0;
    unsigned err = lodepng_inc_begin(&d, &out, &w, &h, &s.png[0], s.png.size());
    unsigned rows = 0;
    if(err) { std::printf("WORKER_ERR begin %u\n", err); return 1; }
    err = lodepng_inc_step(d, worker_step, &rows);
    if(err != 130) { std::printf("WORKER_ERR step %u\n", err); return 1; }
    lodepng_inc_destroy(d);
    {
      std::vector<unsigned char> blob;
      FILE* f = std::fopen(blobpath, "rb");
      long sz;
      if(!f) { std::printf("WORKER_ERR noblob\n"); return 1; }
      std::fseek(f, 0, SEEK_END); sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
      blob.resize((size_t)sz);
      if(std::fread(&blob[0], 1, blob.size(), f) != blob.size()) {
        std::fclose(f); std::printf("WORKER_ERR read\n"); return 1;
      }
      std::fclose(f);
      err = lodepng_inc_restore(&d, &blob[0], blob.size(),
                                &s.png[0], s.png.size(), out);
      if(err) { std::printf("WORKER_ERR restore %u\n", err); return 1; }
    }
    do { err = lodepng_inc_step(d, h, &rows); } while(err == 130);
    if(err) { std::printf("WORKER_ERR finish %u\n", err); return 1; }
    lodepng_inc_destroy(d);
    {
      FILE* f = std::fopen((std::string(blobpath) + ".done").c_str(), "wb");
      if(!f) { std::printf("WORKER_ERR nowrite\n"); return 1; }
      std::fwrite(out, 1, (size_t)w * h * 4, f);
      std::fclose(f);
    }
    std::free(out);
    std::printf("WORKER_OK %u\n", worker_idx);
    return 0;
  }

  std::printf("generated %u small PNG samples (btype 0/1/2, 14 color modes, "
              "7 filter strategies)\n", (unsigned)samples.size());

  if(argc >= 3 && std::strcmp(argv[1], "--diag") == 0) {
    unsigned idx = (unsigned)std::atoi(argv[2]);
    const Sample& s = samples[idx];
    std::printf("sample %u: %ux%u ct=%d bd=%d btype=%d filter=%d pngsize=%u\n",
                idx, s.w, s.h, s.colortype, s.bitdepth, s.btype, s.filter,
                (unsigned)s.png.size());
    {
      std::vector<unsigned char> ref, got;
      unsigned e = ref_decode(s, ref);
      std::printf("ref err=%u bytes=%u\n", e, (unsigned)ref.size());
      e = chunked_decode(s, 1, got);
      if(!e) {
        unsigned diffs = 0; unsigned first = 0;
        for(unsigned z = 0; z < ref.size(); ++z)
          if(ref[z] != got[z]) { if(!diffs) first = z; ++diffs; }
        std::printf("1-row chunked err=%u diffs=%u first@%u (ref=%u got=%u)\n",
                    e, diffs, first, first < ref.size() ? ref[first] : 0,
                    first < got.size() ? got[first] : 0);
      } else std::printf("1-row chunked err=%u\n", e);
    }
    return 0;
  }

  /* ---- main equivalence suite ---- */
  for(i = 0; i < samples.size(); ++i) {
    const Sample& s = samples[i];
    std::vector<unsigned char> ref, got;
    unsigned err;
    err = ref_decode(s, ref);
    if(err) { std::printf("[%3u] ref decode err %u\n", i, err); ++failures; continue; }

    /* chunk counts n = 2, 3, 7, both plain and save/reload variants */
    static const unsigned ns[] = {2, 3, 7};
    unsigned k;
    for(k = 0; k < 3; ++k) {
      unsigned n = ns[k];
      char tag[32];
      /* plain chunked: use the first chunk's row count as fixed block size;
      also run a fixed 1-row-per-step extreme */
      unsigned rps = rows_per_chunk(s.h, n, 0);
      std::snprintf(tag, sizeof(tag), "n=%u chunked", n);
      err = chunked_decode(s, rps, got);
      if(err || !byte_equal(ref, got)) {
        std::printf("[%3u] FAIL %s err=%u\n", i, tag, err); ++failures;
      }
      std::snprintf(tag, sizeof(tag), "n=%u reloaded", n);
      err = nstep_reloaded_decode(s, n, got);
      if(err || !byte_equal(ref, got)) {
        std::printf("[%3u] FAIL %s err=%u\n", i, tag, err); ++failures;
      }
    }
    /* 1-row blocks: maximum interrupt frequency */
    err = chunked_decode(s, 1, got);
    if(err || !byte_equal(ref, got)) {
      std::printf("[%3u] FAIL 1-row chunked err=%u\n", i, err); ++failures;
    }
    err = reloaded_decode(s, 1, got);
    if(err || !byte_equal(ref, got)) {
      std::printf("[%3u] FAIL 1-row reloaded err=%u\n", i, err); ++failures;
    }

    /* state size vs. twice one row block (measured after the first step,
    the smallest possible block) */
    {
      LodePNGIncremental* d = 0;
      unsigned char* o2 = 0; unsigned w2 = 0, h2 = 0;
      unsigned rows2 = 0;
      err = lodepng_inc_begin(&d, &o2, &w2, &h2, &s.png[0], s.png.size());
      if(err) { std::printf("[%3u] state-size begin err %u\n", i, err); ++failures; }
      else {
        size_t stsize;
        double rowblock;
        err = lodepng_inc_step(d, 1, &rows2);
        if(err != 130 && err != 0) {
          std::printf("[%3u] state-size step err %u\n", i, err); ++failures;
        } else {
          stsize = lodepng_inc_state_size(d);
          /* one row block in the *output* image: RGBA8 row size */
          rowblock = (double)w2 * 4.0;
          if(stsize > max_blob) { max_blob = stsize; max_blob_idx = i; }
          if(rowblock > rowblock_bytes_max) rowblock_bytes_max = rowblock;
          if((double)stsize > 2.0 * rowblock) {
            std::printf("[%3u] FAIL state size %u > 2x row block %.0f "
                        "(w=%u ct=%d bd=%d)\n", i, (unsigned)stsize,
                        2.0 * rowblock, w2, s.colortype, s.bitdepth);
            ++failures;
          }
        }
        lodepng_inc_destroy(d);
        std::free(o2);
      }
    }
  }

  std::printf("equivalence suite: %u failure(s) over %u images x "
              "(3 chunk counts x 2 modes + 2 one-row modes)\n",
              failures, (unsigned)samples.size());

  /* ---- real process restart on a spread of 10 samples ---- */
  if(failures == 0) {
    unsigned proc_fails = 0;
    unsigned t;
    std::printf("real process-restart test (10 fresh execs via fork/exec)...\n");
    for(t = 0; t < 10; ++t) {
      unsigned idx = (unsigned)((size_t)t * samples.size() / 10);
      const Sample& s = samples[idx];
      LodePNGIncremental* d = 0;
      unsigned char* out = 0; unsigned w = 0, h = 0;
      unsigned rows = 0, err;
      unsigned step = 1 + (t % (s.h - 1 < 1 ? 1 : s.h - 1));
      std::string blobpath = std::string("/tmp/incstate_") +
                             to_str(idx) + ".bin";
      std::string donepath = blobpath + ".done";
      FILE* f;
      err = lodepng_inc_begin(&d, &out, &w, &h, &s.png[0], s.png.size());
      if(err) { std::printf("  proc[%u] begin err %u\n", idx, err); ++proc_fails; continue; }
      err = lodepng_inc_step(d, step, &rows);
      if(err != 130) { std::printf("  proc[%u] step err %u\n", idx, err); ++proc_fails;
        lodepng_inc_destroy(d); std::free(out); continue; }
      f = std::fopen(blobpath.c_str(), "wb");
      {
        std::vector<unsigned char> blob(lodepng_inc_state_size(d));
        lodepng_inc_save(d, &blob[0]);
        std::fwrite(&blob[0], 1, blob.size(), f);
      }
      std::fclose(f);
      lodepng_inc_destroy(d);
      std::free(out);
      std::remove(donepath.c_str());

      {
        std::string cmd = std::string(argv[0]) + " --worker " +
                          to_str(idx) + " " + to_str(step) +
                          " " + blobpath;
        int rc = std::system(cmd.c_str());
        if(rc != 0) { std::printf("  proc[%u] worker rc %d\n", idx, rc); ++proc_fails; continue; }
      }
      {
        std::vector<unsigned char> ref, got;
        FILE* g = std::fopen(donepath.c_str(), "rb");
        if(!g) { std::printf("  proc[%u] no result\n", idx); ++proc_fails; continue; }
        ref_decode(s, ref);
        got.resize(ref.size());
        if(std::fread(&got[0], 1, got.size(), g) != got.size()) {
          std::printf("  proc[%u] short result\n", idx); ++proc_fails;
        } else if(!byte_equal(ref, got)) {
          std::printf("  proc[%u] pixel mismatch after process restart\n", idx);
          ++proc_fails;
        }
        std::fclose(g);
        std::remove(blobpath.c_str());
        std::remove(donepath.c_str());
      }
    }
    std::printf("process-restart test: %u failure(s)\n", proc_fails);
    failures += proc_fails;
  }

  std::printf("max serialized state: %u bytes (sample %u); every state was "
              "<= 2x its one-row output block\n",
              (unsigned)max_blob, max_blob_idx);

  return failures ? 1 : 0;
}
