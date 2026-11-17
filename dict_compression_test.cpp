/*
dict_compression_test.cpp

Standalone test + benchmark for shared-dictionary batch compression of
same-style assets, built on top of lodepng's zlib implementation:

  stage 1: buildDictionary() extracts repeated fragments from a batch of
           assets and assembles a shared dictionary (stored once, never
           embedded in the individual compressed blobs);
  stage 2: lodepng_zlib_compress_dict() feeds the dictionary to the deflate
           LZ77 matcher to raise the compression ratio of every asset;
  stage 3: lodepng_zlib_decompress_dict() restores the data using the same
           single shared dictionary.

Dictionary source, size cap and minimum fragment length are all configurable
(DictBuilderConfig / command line). When the dictionary grows past its cap,
the lowest-value fragments are evicted instead of failing.

This file is intentionally NOT part of the Makefile. Build manually:

  g++ -W -Wall -Wextra -O2 -I . dict_compression_test.cpp lodepng.cpp -o dict_test

Run:            ./dict_test [max_dict_size] [min_frag_len]
Dump baseline:  ./dict_test --dump <dir>   (writes plain no-dict zlib blobs)
Reference build (against a pristine lodepng.cpp, for the byte-identical check):
  g++ -O2 -DLODEPNG_REF_BUILD -I . dict_compression_test.cpp lodepng_ref.cpp -o dict_test_ref
*/

#include "lodepng.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef LODEPNG_REF_BUILD
/* Dictionary zlib API: implemented in lodepng.cpp, deliberately not declared
in lodepng.h so the public interface is unchanged. */
unsigned lodepng_zlib_compress_dict(unsigned char** out, size_t* outsize,
                                    const unsigned char* dict, size_t dictsize,
                                    const unsigned char* in, size_t insize,
                                    const LodePNGCompressSettings* settings);
unsigned lodepng_zlib_decompress_dict(unsigned char** out, size_t* outsize,
                                      const unsigned char* dict, size_t dictsize,
                                      const unsigned char* in, size_t insize,
                                      const LodePNGDecompressSettings* settings);
#endif

namespace {

/* ---------------- deterministic RNG (no external deps) ---------------- */
struct Rng {
  unsigned s;
  explicit Rng(unsigned seed) : s(seed ? seed : 1u) {}
  unsigned next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  unsigned range(unsigned n) { return n ? next() % n : 0; }
};

/* ---------------- synthetic sample assets (self-made, small) ---------------- */

const unsigned IMG_W = 64, IMG_H = 64, IMG_BPP = 4;
const size_t IMG_SIZE = IMG_W * IMG_H * IMG_BPP; /* 16 KB */
const unsigned TILE = 16; /* 16x16 RGBA tiles */

/* one 16x16 RGBA "sprite part": random base colors + structured noise */
std::vector<unsigned char> makeTile(Rng& rng) {
  std::vector<unsigned char> t(TILE * TILE * 4);
  unsigned char base[3];
  unsigned char accent[3];
  for (int i = 0; i < 3; ++i) base[i] = (unsigned char)(40 + rng.range(180));
  for (int i = 0; i < 3; ++i) accent[i] = (unsigned char)(40 + rng.range(180));
  unsigned period = 2 + rng.range(6);
  for (unsigned y = 0; y < TILE; ++y) {
    for (unsigned x = 0; x < TILE; ++x) {
      size_t p = (y * TILE + x) * 4;
      bool a = ((x / period) + (y / period)) % 2 == 0;
      for (int c = 0; c < 3; ++c) {
        int v = (a ? base[c] : accent[c]) + (int)(rng.range(21)) - 10;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        t[p + c] = (unsigned char)v;
      }
      t[p + 3] = 255;
    }
  }
  return t;
}

/* a 64x64 image assembled as a 4x4 grid of tiles picked from a shared pool */
std::vector<unsigned char> composeFromPool(const std::vector<std::vector<unsigned char> >& pool,
                                           Rng& rng) {
  std::vector<unsigned char> img(IMG_SIZE);
  for (unsigned ty = 0; ty < IMG_H / TILE; ++ty) {
    for (unsigned tx = 0; tx < IMG_W / TILE; ++tx) {
      const std::vector<unsigned char>& t = pool[rng.range((unsigned)pool.size())];
      for (unsigned y = 0; y < TILE; ++y) {
        for (unsigned x = 0; x < TILE; ++x) {
          size_t dst = ((ty * TILE + y) * IMG_W + (tx * TILE + x)) * 4;
          size_t src = (y * TILE + x) * 4;
          for (int c = 0; c < 4; ++c) img[dst + c] = t[src + c];
        }
      }
    }
  }
  return img;
}

std::vector<std::vector<unsigned char> > genSameStyleBatch(unsigned count, unsigned seed) {
  Rng rng(seed);
  std::vector<std::vector<unsigned char> > pool;
  for (unsigned i = 0; i < 24; ++i) pool.push_back(makeTile(rng)); /* shared "style" */
  std::vector<std::vector<unsigned char> > batch;
  for (unsigned i = 0; i < count; ++i) batch.push_back(composeFromPool(pool, rng));
  return batch;
}

std::vector<std::vector<unsigned char> > genMixedBatch(unsigned seed) {
  Rng rng(seed);
  std::vector<std::vector<unsigned char> > batch(4, std::vector<unsigned char>(IMG_SIZE));
  /* style 1: pure noise */
  for (size_t i = 0; i < IMG_SIZE; ++i) batch[0][i] = (unsigned char)rng.next();
  /* style 2: vertical gradient */
  for (unsigned y = 0; y < IMG_H; ++y)
    for (unsigned x = 0; x < IMG_W; ++x) {
      size_t p = (y * IMG_W + x) * 4;
      batch[1][p + 0] = (unsigned char)(y * 4);
      batch[1][p + 1] = (unsigned char)(255 - y * 3);
      batch[1][p + 2] = (unsigned char)(x * 2);
      batch[1][p + 3] = 255;
    }
  /* style 3: diagonal stripes */
  for (unsigned y = 0; y < IMG_H; ++y)
    for (unsigned x = 0; x < IMG_W; ++x) {
      size_t p = (y * IMG_W + x) * 4;
      unsigned char v = ((x + y) % 11 < 5) ? 230 : 20;
      batch[2][p + 0] = v; batch[2][p + 1] = (unsigned char)(v / 2);
      batch[2][p + 2] = (unsigned char)(255 - v); batch[2][p + 3] = 255;
    }
  /* style 4: random solid rectangles */
  for (size_t i = 0; i < IMG_SIZE; i += 4) { batch[3][i + 3] = 255; }
  for (unsigned r = 0; r < 40; ++r) {
    unsigned rx = rng.range(IMG_W - 8), ry = rng.range(IMG_H - 8);
    unsigned rw = 4 + rng.range(12), rh = 4 + rng.range(12);
    unsigned char col[3];
    for (int c = 0; c < 3; ++c) col[c] = (unsigned char)rng.next();
    for (unsigned y = ry; y < ry + rh && y < IMG_H; ++y)
      for (unsigned x = rx; x < rx + rw && x < IMG_W; ++x) {
        size_t p = (y * IMG_W + x) * 4;
        for (int c = 0; c < 3; ++c) batch[3][p + c] = col[c];
      }
  }
  return batch;
}

/* ---------------- stage 1: dictionary builder ---------------- */

struct DictBuilderConfig {
  size_t max_dict_size; /* 字典大小上限 (bytes) */
  size_t min_frag;      /* 最小片段长度 (bytes) */
  DictBuilderConfig() : max_dict_size(16384), min_frag(64) {}
};

struct DictBuildResult {
  std::vector<unsigned char> dict;
  std::vector<std::string> fragments; /* the selected fragments, in stored order */
  long est_savings;                   /* estimated corpus-wide bytes saved by the dictionary */
  size_t repeated_candidates; /* distinct repeated fragments found in the corpus */
  size_t selected;            /* fragments that ended up in the dictionary */
  size_t evicted;             /* fragments evicted by the size-cap policy */
  double build_ms;
  DictBuildResult() : est_savings(0) {}
};

/*
Extract repeated fragments (>= min_frag bytes, occurring at least twice) from
the corpus, score them by estimated savings, and greedily assemble a dictionary
of at most max_dict_size bytes. If a new fragment does not fit, the
lowest-scoring already-selected fragments are evicted (eviction policy) until
it fits or it is rejected -- the builder never fails because of the cap.
The most valuable fragments are placed at the END of the dictionary, because
deflate backward references can only reach windowsize bytes back.
If 'seed' is given, its fragments start in the selection (re-scored against the
new corpus); better new fragments then evict stale seed fragments -- this is how
a dictionary is updated when a new asset pack with a different style arrives.
If the estimated savings do not cover the dictionary's own size, an empty
dictionary is returned (a dictionary that does not pay for itself is not used).
*/
DictBuildResult buildDictionary(const std::vector<std::vector<unsigned char> >& corpus,
                                const DictBuilderConfig& cfg,
                                const DictBuildResult* seed = 0) {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  DictBuildResult res;
  res.repeated_candidates = res.selected = res.evicted = 0;

  std::string all;
  for (size_t i = 0; i < corpus.size(); ++i)
    all.append((const char*)corpus[i].data(), corpus[i].size());

  /* count occurrences of every fragment of length min_frag */
  std::map<std::string, unsigned> counts;
  if (all.size() >= cfg.min_frag) {
    for (size_t i = 0; i + cfg.min_frag <= all.size(); ++i)
      ++counts[all.substr(i, cfg.min_frag)];
  }

  struct Cand {
    std::string frag;
    long score; /* estimated bytes saved: each occurrence becomes a ~4-byte backref */
  };
  std::vector<Cand> cands;
  for (std::map<std::string, unsigned>::const_iterator it = counts.begin(); it != counts.end(); ++it) {
    if (it->second < 2) continue;
    long score = (long)it->second * ((long)cfg.min_frag - 4);
    if (score <= 0) continue;
    Cand c; c.frag = it->first; c.score = score;
    cands.push_back(c);
  }
  res.repeated_candidates = cands.size();
  std::sort(cands.begin(), cands.end(),
            [](const Cand& a, const Cand& b) { return a.score > b.score; });

  /* coverage of corpus positions, to skip fragments that only repeat content
  already covered by a previously selected fragment */
  std::vector<char> covered(all.size(), 0);

  std::vector<Cand> sel;
  size_t cur = 0;

  /* seed fragments from a previous dictionary, re-scored against this corpus */
  if (seed) {
    for (size_t i = 0; i < seed->fragments.size(); ++i) {
      const std::string& f = seed->fragments[i];
      std::map<std::string, unsigned>::const_iterator it = counts.find(f);
      unsigned count = it != counts.end() ? it->second : 0;
      long score = (long)count * ((long)f.size() - 4);
      Cand c; c.frag = f; c.score = score;
      sel.push_back(c);
      cur += f.size();
    }
  }

  for (size_t ci = 0; ci < cands.size(); ++ci) {
    const Cand& c = cands[ci];
    /* skip fragments already present in the selection (e.g. from the seed) */
    bool dup = false;
    for (size_t k = 0; k < sel.size(); ++k) if (sel[k].frag == c.frag) { dup = true; break; }
    if (dup) continue;
    /* find all occurrences and measure how much is already covered */
    std::vector<size_t> occ;
    size_t pos = 0, cov = 0, total = 0;
    while ((pos = all.find(c.frag, pos)) != std::string::npos) {
      occ.push_back(pos);
      for (size_t k = 0; k < c.frag.size(); ++k) { ++total; if (covered[pos + k]) ++cov; }
      pos += 1;
    }
    if (!occ.empty() && cov * 2 > total) continue; /* >50% already covered: redundant */

    /* size cap with eviction: never fail, evict the weakest fragments instead */
    if (cur + c.frag.size() > cfg.max_dict_size) {
      while (cur + c.frag.size() > cfg.max_dict_size && !sel.empty()) {
        size_t worst = 0;
        for (size_t k = 1; k < sel.size(); ++k) if (sel[k].score < sel[worst].score) worst = k;
        if (sel[worst].score < c.score) {
          cur -= sel[worst].frag.size();
          sel.erase(sel.begin() + worst);
          ++res.evicted;
        } else break;
      }
      if (cur + c.frag.size() > cfg.max_dict_size) continue; /* reject candidate, no failure */
    }
    sel.push_back(c);
    cur += c.frag.size();
    for (size_t k = 0; k < occ.size(); ++k)
      for (size_t j = 0; j < c.frag.size(); ++j) covered[occ[k] + j] = 1;
  }

  /* assemble: lowest value first, most valuable last (reachable by the window) */
  std::sort(sel.begin(), sel.end(),
            [](const Cand& a, const Cand& b) { return a.score < b.score; });
  std::string dict;
  for (size_t i = 0; i < sel.size(); ++i) dict += sel[i].frag;
  if (dict.size() > cfg.max_dict_size) dict.erase(0, dict.size() - cfg.max_dict_size); /* last-resort trim */

  res.selected = sel.size();
  res.est_savings = 0;
  for (size_t i = 0; i < sel.size(); ++i) res.est_savings += sel[i].score;
  for (size_t i = 0; i < sel.size(); ++i) res.fragments.push_back(sel[i].frag);
  res.dict.assign(dict.begin(), dict.end());

  /* cost-benefit guard: a dictionary that does not pay for itself is not used */
  if (res.est_savings <= (long)res.dict.size()) {
    res.dict.clear();
    res.fragments.clear();
  }

  std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
  res.build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  return res;
}

/* ---------------- benchmark helpers ---------------- */

double nowMs() {
  return std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifndef LODEPNG_REF_BUILD
/*
Trial-compress the corpus with and without the dictionary (same settings) and
report whether the dictionary pays for its own storage. This is the adaptive
guard: a dictionary that does not shrink the batch in a real trial compression
is discarded and the batch falls back to plain no-dictionary compression.
*/
bool dictPaysOff(const std::vector<std::vector<unsigned char> >& corpus,
                 const std::vector<unsigned char>& dict,
                 size_t* plain_total, size_t* dict_total) {
  LodePNGCompressSettings cs = lodepng_default_compress_settings;
  cs.windowsize = 32768;
  size_t p = 0, d = 0;
  for (size_t i = 0; i < corpus.size(); ++i) {
    unsigned char* out = 0; size_t outsize = 0;
    unsigned e = lodepng_zlib_compress(&out, &outsize, corpus[i].data(), corpus[i].size(), &cs);
    if (!e) p += outsize;
    free(out); out = 0; outsize = 0;
    e = lodepng_zlib_compress_dict(&out, &outsize, dict.empty() ? 0 : dict.data(), dict.size(),
                                   corpus[i].data(), corpus[i].size(), &cs);
    if (!e) d += outsize;
    free(out);
  }
  *plain_total = p;
  *dict_total = d;
  return d + dict.size() < p;
}
#endif

struct GroupResult {
  std::string name;
  size_t raw_bytes, plain_bytes, dict_bytes, dict_size;
  double plain_compress_ms, dict_compress_ms, dict_decompress_ms;
  bool decompress_ok;   /* dict decompress == original, byte exact */
  bool plain_ok;        /* plain decompress == original, byte exact */
  bool empty_identical; /* empty-dict output == plain output, byte exact */
};

const int REPS = 20; /* repeat compression to get measurable timings; runs once overall */

GroupResult runGroup(const std::string& name,
                     const std::vector<std::vector<unsigned char> >& images,
                     const std::vector<unsigned char>& dict) {
  GroupResult r;
  r.name = name;
  r.raw_bytes = r.plain_bytes = r.dict_bytes = 0;
  r.plain_compress_ms = r.dict_compress_ms = r.dict_decompress_ms = 0;
  r.decompress_ok = r.plain_ok = r.empty_identical = true;
  r.dict_size = dict.size();

  LodePNGCompressSettings cs = lodepng_default_compress_settings;
  cs.windowsize = 32768; /* allow backrefs to reach deep into the dictionary */
  LodePNGDecompressSettings ds = lodepng_default_decompress_settings;

  for (size_t i = 0; i < images.size(); ++i) {
    const std::vector<unsigned char>& img = images[i];
    r.raw_bytes += img.size();

    /* plain, no dictionary */
    unsigned char* plain = 0; size_t plainsize = 0;
    double t0 = nowMs();
    for (int rep = 0; rep < REPS; ++rep) {
      free(plain); plain = 0; plainsize = 0;
      unsigned e = lodepng_zlib_compress(&plain, &plainsize, img.data(), img.size(), &cs);
      if (e) { printf("  ERROR plain compress: %s\n", lodepng_error_text(e)); r.plain_ok = false; break; }
    }
    r.plain_compress_ms += nowMs() - t0;
    r.plain_bytes += plainsize;

    /* plain decompress sanity check */
    unsigned char* dec = 0; size_t decsize = 0;
    unsigned e = lodepng_zlib_decompress(&dec, &decsize, plain, plainsize, &ds);
    if (e || decsize != img.size() || std::memcmp(dec, img.data(), img.size()) != 0) r.plain_ok = false;
    free(dec);

#ifndef LODEPNG_REF_BUILD
    /* with shared dictionary */
    unsigned char* dz = 0; size_t dzsize = 0;
    t0 = nowMs();
    for (int rep = 0; rep < REPS; ++rep) {
      free(dz); dz = 0; dzsize = 0;
      e = lodepng_zlib_compress_dict(&dz, &dzsize, dict.empty() ? 0 : dict.data(), dict.size(),
                                     img.data(), img.size(), &cs);
      if (e) { printf("  ERROR dict compress: %s\n", lodepng_error_text(e)); r.decompress_ok = false; break; }
    }
    r.dict_compress_ms += nowMs() - t0;
    r.dict_bytes += dzsize;

    /* stage 3: decompress with the same shared dictionary, must be byte exact */
    dec = 0; decsize = 0;
    t0 = nowMs();
    for (int rep = 0; rep < REPS; ++rep) {
      free(dec); dec = 0; decsize = 0;
      e = lodepng_zlib_decompress_dict(&dec, &decsize, dict.empty() ? 0 : dict.data(), dict.size(),
                                       dz, dzsize, &ds);
      if (e) break;
    }
    r.dict_decompress_ms += nowMs() - t0;
    if (e || decsize != img.size() || std::memcmp(dec, img.data(), img.size()) != 0) {
      r.decompress_ok = false;
      printf("  ERROR dict decompress img %zu: %s\n", i, e ? lodepng_error_text(e) : "content mismatch");
    }
    free(dec);

    /* empty dictionary must give byte-identical output to the plain path */
    if (dict.empty()) {
      unsigned char* z0 = 0; size_t z0size = 0;
      e = lodepng_zlib_compress_dict(&z0, &z0size, 0, 0, img.data(), img.size(), &cs);
      if (e || z0size != plainsize || std::memcmp(z0, plain, plainsize) != 0) r.empty_identical = false;
      free(z0);
    }
    free(dz);
#else
    r.dict_bytes += plainsize;
#endif
    free(plain);
  }
  return r;
}

void printGroup(const GroupResult& r, double dict_build_ms, double target_ratio, bool use_target) {
  double ratio = r.plain_bytes ? (double)r.dict_bytes / (double)r.plain_bytes : 1.0;
  double amortized = r.plain_bytes ? ((double)r.dict_bytes + (double)r.dict_size) / (double)r.plain_bytes : 1.0;
  double dict_share = ((double)r.dict_bytes + (double)r.dict_size) > 0
      ? (double)r.dict_size / ((double)r.dict_bytes + (double)r.dict_size) : 0.0;
  printf("  raw total:            %10zu bytes\n", r.raw_bytes);
  printf("  plain zlib total:     %10zu bytes\n", r.plain_bytes);
  printf("  dict zlib total:      %10zu bytes  (体积比 %.3f vs plain)\n", r.dict_bytes, ratio);
  printf("  + shared dictionary:  %10zu bytes  (摊销后体积比 %.3f, 字典体积占比 %.1f%%)\n",
         r.dict_size, amortized, dict_share * 100.0);
  printf("  解压一致性 (byte-exact): dict %s, plain %s\n",
         r.decompress_ok ? "OK" : "FAIL", r.plain_ok ? "OK" : "FAIL");
  printf("  总耗时 (%d reps):    dict build %.2f ms, dict compress %.2f ms, dict decompress %.2f ms (plain compress %.2f ms)\n",
         REPS, dict_build_ms, r.dict_compress_ms, r.dict_decompress_ms, r.plain_compress_ms);
  if (use_target) {
    double basis = (double)(r.dict_bytes + r.dict_size) / (double)r.plain_bytes;
    if (basis <= target_ratio) printf("  目标: 摊销体积比 <= %.2f -> PASS (%.3f)\n", target_ratio, basis);
    else printf("  目标: 摊销体积比 <= %.2f -> MISS, 实际 %.3f, 超出 %.1f%%\n",
                target_ratio, basis, (basis - target_ratio) * 100.0);
  }
}

/* dump plain (default settings, no dict) zlib blobs for the byte-identical
regression check against a pristine lodepng build */
int dumpMode(const char* dir) {
  std::vector<std::vector<unsigned char> > groups[3];
  groups[0] = genSameStyleBatch(8, 12345);
  groups[1] = genMixedBatch(777);
  groups[2] = genSameStyleBatch(1, 999);
  LodePNGCompressSettings cs = lodepng_default_compress_settings; /* untouched defaults */
  size_t total = 0;
  for (size_t g = 0; g < 3; ++g) {
    for (size_t i = 0; i < groups[g].size(); ++i) {
      unsigned char* out = 0; size_t outsize = 0;
      unsigned e = lodepng_zlib_compress(&out, &outsize, groups[g][i].data(), groups[g][i].size(), &cs);
      if (e) { printf("dump compress error: %s\n", lodepng_error_text(e)); return 1; }
      char path[512];
      std::snprintf(path, sizeof(path), "%s/plain_%zu_%zu.zlib", dir, g, i);
      FILE* f = std::fopen(path, "wb");
      if (!f) { printf("cannot write %s\n", path); return 1; }
      std::fwrite(out, 1, outsize, f);
      std::fclose(f);
      total += outsize;
      free(out);
    }
  }
  printf("dumped %zu bytes of plain zlib blobs to %s\n", total, dir);
  return 0;
}

} /* namespace */

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--dump") == 0) {
    if (argc < 3) { printf("usage: %s --dump <dir>\n", argv[0]); return 1; }
    return dumpMode(argv[2]);
  }
#ifdef LODEPNG_REF_BUILD
  printf("reference build: only --dump mode is available\n");
  return 1;
#else
  DictBuilderConfig dcfg;
  if (argc >= 2) dcfg.max_dict_size = (size_t)std::atol(argv[1]);
  if (argc >= 3) dcfg.min_frag = (size_t)std::atol(argv[2]);
  printf("=== shared-dictionary batch compression benchmark ===\n");
  printf("config: dict cap = %zu bytes, min fragment = %zu bytes, image = %zux%zu RGBA (%zu bytes)\n\n",
         dcfg.max_dict_size, dcfg.min_frag, (size_t)IMG_W, (size_t)IMG_H, IMG_SIZE);

  bool all_ok = true;

  /* group 1: same-style batch, dictionary from the batch itself */
  {
    printf("[group 1] 同风格批 (8 images, shared tile pool)\n");
    std::vector<std::vector<unsigned char> > imgs = genSameStyleBatch(8, 12345);
    DictBuildResult db = buildDictionary(imgs, dcfg);
    printf("  dictionary: %zu bytes (cap %zu), %zu repeated candidates, %zu selected, est. savings %ld bytes, built in %.2f ms\n",
           db.dict.size(), dcfg.max_dict_size, db.repeated_candidates, db.selected, db.est_savings, db.build_ms);
    if (db.dict.size() > dcfg.max_dict_size) { printf("  FAIL: dictionary exceeds cap\n"); all_ok = false; }
    size_t pt = 0, dt = 0;
    bool keep = !db.dict.empty() && dictPaysOff(imgs, db.dict, &pt, &dt);
    printf("  试压验证: dict total %zu + dict %zu = %zu vs plain %zu -> %s\n",
           dt, db.dict.size(), dt + db.dict.size(), pt, keep ? "保留字典" : "弃用字典 (回退无字典)");
    GroupResult r = runGroup("same-style", imgs, keep ? db.dict : std::vector<unsigned char>());
    printGroup(r, db.build_ms, 0.75, true);
    all_ok = all_ok && r.decompress_ok && r.plain_ok;
    printf("\n");
  }

  /* group 2: mixed styles, dictionary from the batch itself */
  {
    printf("[group 2] 异风格 (4 images, unrelated generators)\n");
    std::vector<std::vector<unsigned char> > imgs = genMixedBatch(777);
    DictBuildResult db = buildDictionary(imgs, dcfg);
    printf("  dictionary: %zu bytes (cap %zu), %zu repeated candidates, %zu selected, est. savings %ld bytes, built in %.2f ms\n",
           db.dict.size(), dcfg.max_dict_size, db.repeated_candidates, db.selected, db.est_savings, db.build_ms);
    size_t pt = 0, dt = 0;
    bool keep = !db.dict.empty() && dictPaysOff(imgs, db.dict, &pt, &dt);
    printf("  试压验证: dict total %zu + dict %zu = %zu vs plain %zu -> %s\n",
           dt, db.dict.size(), dt + db.dict.size(), pt, keep ? "保留字典" : "弃用字典 (回退无字典)");
    GroupResult r = runGroup("mixed", imgs, keep ? db.dict : std::vector<unsigned char>());
    printGroup(r, db.build_ms, 1.05, true);
    all_ok = all_ok && r.decompress_ok && r.plain_ok;
    printf("\n");
  }

  /* group 3: single image, dictionary from itself */
  {
    printf("[group 3] 单图 (1 image, dictionary from itself)\n");
    std::vector<std::vector<unsigned char> > imgs = genSameStyleBatch(1, 999);
    DictBuildResult db = buildDictionary(imgs, dcfg);
    printf("  dictionary: %zu bytes (cap %zu), %zu repeated candidates, %zu selected, est. savings %ld bytes, built in %.2f ms\n",
           db.dict.size(), dcfg.max_dict_size, db.repeated_candidates, db.selected, db.est_savings, db.build_ms);
    size_t pt = 0, dt = 0;
    bool keep = !db.dict.empty() && dictPaysOff(imgs, db.dict, &pt, &dt);
    printf("  试压验证: dict total %zu + dict %zu = %zu vs plain %zu -> %s\n",
           dt, db.dict.size(), dt + db.dict.size(), pt, keep ? "保留字典" : "弃用字典 (回退无字典)");
    GroupResult r = runGroup("single", imgs, keep ? db.dict : std::vector<unsigned char>());
    printGroup(r, db.build_ms, 1.10, true);
    all_ok = all_ok && r.decompress_ok && r.plain_ok;
    printf("\n");
  }

  /* group 4: empty dictionary */
  {
    printf("[group 4] 空字典 (dict size 0)\n");
    std::vector<std::vector<unsigned char> > imgs = genSameStyleBatch(8, 12345);
    std::vector<unsigned char> empty;
    GroupResult r = runGroup("empty-dict", imgs, empty);
    printGroup(r, 0.0, 1.0, false);
    printf("  空字典输出与无字典输出逐字节一致: %s\n", r.empty_identical ? "OK" : "FAIL");
    all_ok = all_ok && r.decompress_ok && r.empty_identical;
    printf("\n");
  }

  /* eviction policy: when a new asset pack with a different style arrives and
  the dictionary is updated under the same size cap, stale fragments of the old
  style are evicted by better new ones -- the cap is never exceeded and the
  builder never fails */
  {
    printf("[eviction] 字典上限淘汰策略 (dictionary update, cap = 2048 bytes)\n");
    std::vector<std::vector<unsigned char> > pack1 = genSameStyleBatch(4, 111); /* old style */
    std::vector<std::vector<unsigned char> > pack2 = genSameStyleBatch(4, 222); /* new style */
    DictBuilderConfig cap2k; cap2k.max_dict_size = 2048; cap2k.min_frag = dcfg.min_frag;
    DictBuildResult db1 = buildDictionary(pack1, cap2k);
    DictBuildResult db2 = buildDictionary(pack2, cap2k, &db1); /* update with new pack */
    printf("  wave 1 (old style): dict %zu bytes, %zu selected\n", db1.dict.size(), db1.selected);
    printf("  wave 2 (new style): dict %zu bytes (cap 2048), %zu selected, %zu stale fragments evicted -> %s\n",
           db2.dict.size(), db2.selected, db2.evicted,
           (db2.dict.size() <= 2048 && db2.evicted > 0) ? "OK (cap respected, eviction instead of failure)" : "FAIL");
    /* the updated dictionary must still round-trip the new pack byte-exactly */
    LodePNGCompressSettings cs = lodepng_default_compress_settings;
    LodePNGDecompressSettings ds = lodepng_default_decompress_settings;
    unsigned char* z = 0; size_t zsize = 0;
    unsigned e = lodepng_zlib_compress_dict(&z, &zsize, db2.dict.empty() ? 0 : db2.dict.data(), db2.dict.size(),
                                            pack2[0].data(), pack2[0].size(), &cs);
    unsigned char* dec = 0; size_t decsize = 0;
    if (!e) e = lodepng_zlib_decompress_dict(&dec, &decsize, db2.dict.empty() ? 0 : db2.dict.data(), db2.dict.size(),
                                             z, zsize, &ds);
    bool rt = !e && decsize == pack2[0].size() && std::memcmp(dec, pack2[0].data(), pack2[0].size()) == 0;
    printf("  updated dictionary round-trip on new pack: %s\n", rt ? "OK" : "FAIL");
    all_ok = all_ok && db2.dict.size() <= 2048 && db2.evicted > 0 && rt;
    free(dec); free(z);
    printf("\n");
  }

  /* robustness: missing / wrong dictionary must give clean errors */
  {
    printf("[robustness] 缺字典 / 错字典\n");
    std::vector<std::vector<unsigned char> > imgs = genSameStyleBatch(2, 42);
    DictBuildResult db = buildDictionary(imgs, dcfg);
    LodePNGCompressSettings cs = lodepng_default_compress_settings;
    LodePNGDecompressSettings ds = lodepng_default_decompress_settings;
    unsigned char* z = 0; size_t zsize = 0;
    unsigned e = lodepng_zlib_compress_dict(&z, &zsize, db.dict.data(), db.dict.size(),
                                            imgs[0].data(), imgs[0].size(), &cs);
    unsigned char* dec = 0; size_t decsize = 0;
    unsigned e1 = lodepng_zlib_decompress_dict(&dec, &decsize, 0, 0, z, zsize, &ds);
    free(dec); dec = 0; decsize = 0;
    std::vector<unsigned char> wrong(db.dict.size(), 0xAB);
    unsigned e2 = lodepng_zlib_decompress_dict(&dec, &decsize, wrong.data(), wrong.size(), z, zsize, &ds);
    printf("  no dict -> error %u (%s) %s\n", e1, lodepng_error_text(e1), e1 == 124 ? "OK" : "FAIL");
    printf("  wrong dict -> error %u (%s) %s\n", e2, lodepng_error_text(e2), e2 == 125 ? "OK" : "FAIL");
    all_ok = all_ok && !e && e1 == 124 && e2 == 125;
    free(dec); free(z);
    printf("\n");
  }

  printf("=== overall: %s ===\n", all_ok ? "PASS" : "FAIL");
  return all_ok ? 0 : 1;
#endif
}
