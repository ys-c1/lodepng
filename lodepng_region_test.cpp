/*
Named Region Table demo/test for lodepng.

Feature: embed a table of named rectangular regions into a PNG as a private
ancillary chunk ("rgNt") at encode time; at decode time, look regions up by
name and extract their pixel blocks. Supports overlapping and nested regions.
Max region count and overlap resolution strategy are configurable.

This file only uses the public lodepng API; the library itself is unmodified,
so decoding output of the original codec is pixel-for-pixel unchanged.

Region table chunk payload ("rgNt", all integers big-endian):
  u8  version (=1)
  u16 region count
  per region:
    u8  name length (1..31)
    name bytes (ASCII, no NUL)
    u32 x, u32 y, u32 w, u32 h

Build (repo root):  g++ -I . -W -Wall -Wextra -ansi -pedantic -O3 lodepng.cpp lodepng_region_test.cpp -o region_test
*/

#include "lodepng.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

/* ------------------------------------------------------------------ */
/* region table module (uses only public lodepng API)                  */
/* ------------------------------------------------------------------ */

#define REGION_CHUNK_TYPE "rgNt"
#define REGION_VERSION 1
#define REGION_MAX_NAME 31

/* overlap resolution strategies for region_at() */
enum RegionOverlapStrategy {
  REGION_FIRST = 0, /* first registered region wins (outermost for nesting) */
  REGION_LAST  = 1  /* last registered region wins (innermost for nesting)  */
};

struct RegionConfig {
  unsigned max_regions;    /* hard limit on table size, 0 = unlimited */
  int overlap_strategy;    /* RegionOverlapStrategy */
};

struct Region {
  std::string name;
  unsigned x, y, w, h;
};

/* error codes of this module */
enum RegionError {
  RERR_OK = 0,
  RERR_TOO_MANY = 1,     /* region count exceeds config limit */
  RERR_BAD_NAME = 2,     /* empty or too long name */
  RERR_EMPTY_RECT = 3,   /* zero width/height */
  RERR_OUT_OF_BOUNDS = 4,/* rectangle (partially) outside the image */
  RERR_NOT_FOUND = 5,    /* no region with this name */
  RERR_NO_TABLE = 6,     /* no region table chunk in the PNG */
  RERR_CORRUPT = 7,      /* malformed chunk payload */
  RERR_ENCODE = 8        /* lodepng reported an error */
};

static const char* region_error_str(unsigned e) {
  switch(e) {
    case RERR_OK: return "ok";
    case RERR_TOO_MANY: return "too many regions (exceeds configured max_regions)";
    case RERR_BAD_NAME: return "bad region name (empty or longer than 31 bytes)";
    case RERR_EMPTY_RECT: return "empty rectangle (zero width or height)";
    case RERR_OUT_OF_BOUNDS: return "region rectangle out of image bounds";
    case RERR_NOT_FOUND: return "no region with this name";
    case RERR_NO_TABLE: return "no region table chunk found in PNG";
    case RERR_CORRUPT: return "corrupt region table chunk";
    case RERR_ENCODE: return "lodepng error";
    default: return "unknown error";
  }
}

static void put32be(std::vector<unsigned char>* v, unsigned x) {
  v->push_back((unsigned char)((x >> 24) & 255));
  v->push_back((unsigned char)((x >> 16) & 255));
  v->push_back((unsigned char)((x >> 8) & 255));
  v->push_back((unsigned char)(x & 255));
}

static unsigned get32be(const unsigned char* p) {
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | (unsigned)p[3];
}

/* validate one region against image size and config */
static unsigned region_validate(const Region& r, unsigned imgw, unsigned imgh,
                                const RegionConfig& cfg, unsigned index, unsigned total) {
  if(cfg.max_regions && total > cfg.max_regions) return RERR_TOO_MANY;
  (void)index;
  if(r.name.empty() || r.name.size() > REGION_MAX_NAME) return RERR_BAD_NAME;
  if(r.w == 0 || r.h == 0) return RERR_EMPTY_RECT;
  /* coordinates are unsigned; only the far edges can overflow the image */
  if(r.x >= imgw || r.y >= imgh || r.w > imgw - r.x || r.h > imgh - r.y) return RERR_OUT_OF_BOUNDS;
  return RERR_OK;
}

/* serialize + validate the table; returns RERR_* */
static unsigned region_table_serialize(std::vector<unsigned char>* out,
                                       const std::vector<Region>& regions,
                                       unsigned imgw, unsigned imgh,
                                       const RegionConfig& cfg) {
  size_t i;
  unsigned e;
  if(cfg.max_regions && regions.size() > cfg.max_regions) return RERR_TOO_MANY;
  if(regions.size() > 65535) return RERR_TOO_MANY;
  out->clear();
  out->push_back(REGION_VERSION);
  out->push_back((unsigned char)((regions.size() >> 8) & 255));
  out->push_back((unsigned char)(regions.size() & 255));
  for(i = 0; i < regions.size(); i++) {
    e = region_validate(regions[i], imgw, imgh, cfg, (unsigned)i, (unsigned)regions.size());
    if(e) return e;
    out->push_back((unsigned char)regions[i].name.size());
    out->insert(out->end(), regions[i].name.begin(), regions[i].name.end());
    put32be(out, regions[i].x);
    put32be(out, regions[i].y);
    put32be(out, regions[i].w);
    put32be(out, regions[i].h);
  }
  return RERR_OK;
}

/* parse + validate a table payload; returns RERR_* */
static unsigned region_table_parse(std::vector<Region>* regions,
                                   const unsigned char* data, size_t size,
                                   unsigned imgw, unsigned imgh,
                                   const RegionConfig& cfg) {
  size_t pos = 0, i;
  unsigned count, e;
  regions->clear();
  if(size < 3) return RERR_CORRUPT;
  if(data[0] != REGION_VERSION) return RERR_CORRUPT;
  count = ((unsigned)data[1] << 8) | (unsigned)data[2];
  pos = 3;
  if(cfg.max_regions && count > cfg.max_regions) return RERR_TOO_MANY;
  for(i = 0; i < count; i++) {
    Region r;
    unsigned nlen;
    if(pos >= size) return RERR_CORRUPT;
    nlen = data[pos++];
    if(nlen == 0 || nlen > REGION_MAX_NAME || pos + nlen + 16 > size) return RERR_CORRUPT;
    r.name.assign((const char*)data + pos, nlen);
    pos += nlen;
    r.x = get32be(data + pos); pos += 4;
    r.y = get32be(data + pos); pos += 4;
    r.w = get32be(data + pos); pos += 4;
    r.h = get32be(data + pos); pos += 4;
    e = region_validate(r, imgw, imgh, cfg, (unsigned)i, count);
    if(e) return e;
    regions->push_back(r);
  }
  if(pos != size) return RERR_CORRUPT;
  return RERR_OK;
}

/* encode image (32-bit RGBA) with a region table embedded as a "rgNt" chunk */
static unsigned encode_with_regions(std::vector<unsigned char>* png,
                                    const unsigned char* image, unsigned w, unsigned h,
                                    const std::vector<Region>& regions,
                                    const RegionConfig& cfg) {
  std::vector<unsigned char> payload;
  unsigned e = region_table_serialize(&payload, regions, w, h, cfg);
  unsigned error;
  lodepng::State state;
  if(e) return e;
  png->clear(); /* lodepng's C++ encode wrapper appends, it does not clear */
  /* append the chunk at position 1 (between PLTE and IDAT) */
  error = lodepng_chunk_create(&state.info_png.unknown_chunks_data[1],
                               &state.info_png.unknown_chunks_size[1],
                               (unsigned)payload.size(), REGION_CHUNK_TYPE, &payload[0]);
  if(error) { printf("lodepng chunk error: %s\n", lodepng_error_text(error)); return RERR_ENCODE; }
  error = lodepng::encode(*png, image, w, h, state);
  if(error) { printf("lodepng encode error: %s\n", lodepng_error_text(error)); return RERR_ENCODE; }
  return RERR_OK;
}

/* decode a PNG (32-bit RGBA) and parse its region table */
static unsigned decode_with_regions(std::vector<unsigned char>* image, unsigned* w, unsigned* h,
                                    std::vector<Region>* regions,
                                    const std::vector<unsigned char>& png,
                                    const RegionConfig& cfg) {
  lodepng::State state;
  unsigned error;
  const unsigned char* chunk;
  const unsigned char* end;
  state.decoder.remember_unknown_chunks = 1;
  error = lodepng::decode(*image, *w, *h, state, png);
  if(error) { printf("lodepng decode error: %s\n", lodepng_error_text(error)); return RERR_ENCODE; }
  /* search all 3 unknown-chunk positions for our table */
  for(int pos = 0; pos < 3; pos++) {
    const unsigned char* data = state.info_png.unknown_chunks_data[pos];
    size_t size = state.info_png.unknown_chunks_size[pos];
    if(!data || size == 0) continue;
    end = data + size;
    chunk = lodepng_chunk_find_const(data, end, REGION_CHUNK_TYPE);
    if(chunk) {
      return region_table_parse(regions, lodepng_chunk_data_const(chunk),
                                lodepng_chunk_length(chunk), *w, *h, cfg);
    }
  }
  return RERR_NO_TABLE;
}

/* find region index by name; -1 if absent */
static int region_find(const std::vector<Region>& regions, const std::string& name) {
  size_t i;
  for(i = 0; i < regions.size(); i++) if(regions[i].name == name) return (int)i;
  return -1;
}

/* extract a region's pixel block by name from a decoded 32-bit RGBA image */
static unsigned region_extract(std::vector<unsigned char>* out,
                               const unsigned char* image, unsigned imgw, unsigned imgh,
                               const std::vector<Region>& regions, const std::string& name) {
  int idx = region_find(regions, name);
  const Region* r;
  unsigned y;
  (void)imgh;
  if(idx < 0) return RERR_NOT_FOUND;
  r = &regions[idx];
  out->resize((size_t)r->w * r->h * 4);
  for(y = 0; y < r->h; y++) {
    const unsigned char* src = image + ((size_t)(r->y + y) * imgw + r->x) * 4;
    std::memcpy(&(*out)[(size_t)y * r->w * 4], src, (size_t)r->w * 4);
  }
  return RERR_OK;
}

/* which region owns pixel (x,y)? honors the overlap strategy; -1 if none */
static int region_at(const std::vector<Region>& regions, unsigned x, unsigned y,
                     const RegionConfig& cfg) {
  int found = -1;
  size_t i;
  for(i = 0; i < regions.size(); i++) {
    const Region* r = &regions[i];
    if(x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
      found = (int)i;
      if(cfg.overlap_strategy == REGION_FIRST) break; /* first registered wins */
      /* REGION_LAST: keep scanning, last registered wins */
    }
  }
  return found;
}

/* ------------------------------------------------------------------ */
/* test scaffolding                                                    */
/* ------------------------------------------------------------------ */

static int g_failures = 0;

static void check(int cond, const char* what) {
  if(cond) {
    printf("  [PASS] %s\n", what);
  } else {
    printf("  [FAIL] %s\n", what);
    g_failures++;
  }
}

/* pixel-exact comparison of an extracted block against the full decode */
static int block_matches_full(const std::vector<unsigned char>& block,
                              const std::vector<unsigned char>& full,
                              unsigned imgw, const Region& r) {
  unsigned x, y;
  for(y = 0; y < r.h; y++) {
    for(x = 0; x < r.w; x++) {
      size_t a = ((size_t)y * r.w + x) * 4;
      size_t b = ((size_t)(r.y + y) * imgw + (r.x + x)) * 4;
      if(block[a] != full[b] || block[a+1] != full[b+1] ||
         block[a+2] != full[b+2] || block[a+3] != full[b+3]) return 0;
    }
  }
  return 1;
}

/* time N extractions of one region, report per-call microseconds */
static double time_extract_us(const std::vector<unsigned char>& full, unsigned w, unsigned h,
                              const std::vector<Region>& regions, const std::string& name,
                              int iters) {
  std::vector<unsigned char> block;
  clock_t t0, t1;
  int i;
  region_extract(&block, &full[0], w, h, regions, name); /* warmup */
  t0 = clock();
  for(i = 0; i < iters; i++) region_extract(&block, &full[0], w, h, regions, name);
  t1 = clock();
  return (double)(t1 - t0) / CLOCKS_PER_SEC * 1e6 / iters;
}

/* draw a small test image: colored "icon" squares on a gradient background */
static void make_test_image(std::vector<unsigned char>* img, unsigned w, unsigned h) {
  unsigned x, y;
  img->resize((size_t)w * h * 4);
  for(y = 0; y < h; y++) {
    for(x = 0; x < w; x++) {
      unsigned char* p = &(*img)[((size_t)y * w + x) * 4];
      p[0] = (unsigned char)(x * 4);
      p[1] = (unsigned char)(y * 5);
      p[2] = (unsigned char)(x + y);
      p[3] = 255;
    }
  }
}

static void fill_rect(std::vector<unsigned char>* img, unsigned imgw,
                      unsigned x, unsigned y, unsigned w, unsigned h,
                      unsigned char r, unsigned char g, unsigned char b) {
  unsigned i, j;
  for(j = y; j < y + h; j++) {
    for(i = x; i < x + w; i++) {
      unsigned char* p = &(*img)[((size_t)j * imgw + i) * 4];
      p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    }
  }
}

/* run one group: encode with table, decode, extract each region, verify, time */
static void run_group(const char* title, const std::vector<unsigned char>& image,
                      unsigned w, unsigned h, const std::vector<Region>& regions,
                      const RegionConfig& cfg) {
  std::vector<unsigned char> png, full;
  std::vector<Region> decoded;
  unsigned ew, eh, e;
  size_t i;

  printf("== %s ==\n", title);
  e = encode_with_regions(&png, &image[0], w, h, regions, cfg);
  check(e == RERR_OK, "encode with region table");
  if(e != RERR_OK) { printf("  error: %s\n", region_error_str(e)); return; }
  printf("  png size: %lu bytes, regions: %lu\n",
         (unsigned long)png.size(), (unsigned long)regions.size());

  e = decode_with_regions(&full, &ew, &eh, &decoded, png, cfg);
  check(e == RERR_OK, "decode + parse region table");
  if(e != RERR_OK) { printf("  error: %s\n", region_error_str(e)); return; }
  check(ew == w && eh == h, "decoded dimensions match");
  check(decoded.size() == regions.size(), "region count round-trips");

  for(i = 0; i < regions.size(); i++) {
    std::vector<unsigned char> block;
    char msg[256];
    e = region_extract(&block, &full[0], ew, eh, decoded, regions[i].name);
    std::snprintf(msg, sizeof msg, "extract \"%s\" (%u,%u %ux%u)",
                  regions[i].name.c_str(), regions[i].x, regions[i].y,
                  regions[i].w, regions[i].h);
    check(e == RERR_OK, msg);
    if(e != RERR_OK) continue;
    std::snprintf(msg, sizeof msg, "\"%s\" pixel-exact vs full decode", regions[i].name.c_str());
    check(block_matches_full(block, full, ew, regions[i]), msg);
    std::snprintf(msg, sizeof msg, "\"%s\" table entry round-trips", regions[i].name.c_str());
    check(decoded[i].name == regions[i].name && decoded[i].x == regions[i].x &&
          decoded[i].y == regions[i].y && decoded[i].w == regions[i].w &&
          decoded[i].h == regions[i].h, msg);
    printf("  [time] extract \"%s\": %.3f us/call (%ux%u block)\n",
           regions[i].name.c_str(), time_extract_us(full, ew, eh, decoded, regions[i].name, 20000),
           regions[i].w, regions[i].h);
  }

  /* original decode path must be pixel-identical on the same PNG */
  {
    std::vector<unsigned char> plain;
    unsigned pw, ph;
    unsigned err = lodepng::decode(plain, pw, ph, png);
    check(err == 0 && pw == w && ph == h && plain == full,
          "plain lodepng::decode output pixel-identical (original decode unchanged)");
  }
  printf("\n");
}

int main() {
  const unsigned W = 64, H = 48;
  std::vector<unsigned char> image;
  RegionConfig cfg;
  cfg.max_regions = 16;
  cfg.overlap_strategy = REGION_LAST;

  make_test_image(&image, W, H);
  /* paint distinct "icons" so regions are visually/byte-wise distinct */
  fill_rect(&image, W,  4,  4, 16, 16, 255, 0, 0);     /* logo    */
  fill_rect(&image, W, 28,  4,  8,  8, 0, 200, 0);     /* icon_a  */
  fill_rect(&image, W, 44,  4,  8,  8, 0, 0, 255);     /* icon_b  */
  fill_rect(&image, W, 28, 20,  8,  8, 255, 255, 0);   /* icon_c  */
  fill_rect(&image, W,  8,  8, 32, 24, 60, 60, 60);    /* panel   */
  fill_rect(&image, W, 12, 12,  8,  8, 200, 0, 200);   /* button  */
  fill_rect(&image, W, 30, 26, 12, 10, 0, 200, 200);   /* badge   */

  printf("named region table test (lodepng unmodified, chunk \"%s\")\n\n", REGION_CHUNK_TYPE);

  /* group 1: single region */
  {
    std::vector<Region> rs;
    Region r; r.name = "logo"; r.x = 4; r.y = 4; r.w = 16; r.h = 16; rs.push_back(r);
    run_group("group 1: single region", image, W, H, rs, cfg);
  }

  /* group 2: multiple non-overlapping regions */
  {
    std::vector<Region> rs;
    Region r;
    r.name = "icon_a"; r.x = 28; r.y = 4; r.w = 8; r.h = 8; rs.push_back(r);
    r.name = "icon_b"; r.x = 44; r.y = 4; r.w = 8; r.h = 8; rs.push_back(r);
    r.name = "icon_c"; r.x = 28; r.y = 20; r.w = 8; r.h = 8; rs.push_back(r);
    run_group("group 2: multiple regions", image, W, H, rs, cfg);
  }

  /* group 3: overlapping + nested regions, both strategies */
  {
    std::vector<Region> rs;
    Region r;
    r.name = "panel";  r.x = 8;  r.y = 8;  r.w = 32; r.h = 24; rs.push_back(r);
    r.name = "button"; r.x = 12; r.y = 12; r.w = 8;  r.h = 8;  rs.push_back(r); /* nested in panel */
    r.name = "badge";  r.x = 30; r.y = 26; r.w = 12; r.h = 10; rs.push_back(r); /* overlaps panel edge */
    run_group("group 3: overlapping + nested regions", image, W, H, rs, cfg);

    /* overlap strategy: pixel (14,14) is inside both "panel" and "button";
       pixel (34,30) is inside both "panel" and "badge" */
    RegionConfig first = cfg; first.overlap_strategy = REGION_FIRST;
    RegionConfig last  = cfg; last.overlap_strategy  = REGION_LAST;
    int fi = region_at(rs, 14, 14, first);
    int li = region_at(rs, 14, 14, last);
    check(fi >= 0 && rs[fi].name == "panel",  "strategy FIRST: nested point -> outer \"panel\"");
    check(li >= 0 && rs[li].name == "button", "strategy LAST:  nested point -> inner \"button\"");
    fi = region_at(rs, 34, 30, first);
    li = region_at(rs, 34, 30, last);
    check(fi >= 0 && rs[fi].name == "panel", "strategy FIRST: overlap point -> \"panel\"");
    check(li >= 0 && rs[li].name == "badge", "strategy LAST:  overlap point -> \"badge\"");
    check(region_at(rs, 63, 47, last) == -1, "point outside all regions -> none");
    printf("\n");
  }

  /* group 4: illegal coordinates and config limits */
  {
    std::vector<Region> rs;
    std::vector<unsigned char> png, full;
    std::vector<Region> decoded;
    unsigned ew = 0, eh = 0, e;
    Region r;

    printf("== group 4: illegal coordinates / limits ==\n");

    /* 4a: encode-time rejection of out-of-bounds rectangle */
    r.name = "bad"; r.x = 50; r.y = 40; r.w = 20; r.h = 20; rs.push_back(r); /* exceeds 64x48 */
    e = encode_with_regions(&png, &image[0], W, H, rs, cfg);
    check(e == RERR_OUT_OF_BOUNDS, "encode rejects out-of-bounds region");
    printf("  encode error: %s\n", region_error_str(e));
    rs.clear();

    /* 4b: decode-time rejection of out-of-bounds rectangle (hand-crafted table) */
    {
      std::vector<unsigned char> payload;
      RegionConfig nofail = cfg; /* same limits, table itself is corrupt */
      lodepng::State state;
      unsigned err;
      payload.push_back(REGION_VERSION);
      payload.push_back(0); payload.push_back(1); /* 1 region */
      payload.push_back(3); payload.push_back('b'); payload.push_back('a'); payload.push_back('d');
      put32be(&payload, 60); put32be(&payload, 44); put32be(&payload, 16); put32be(&payload, 16);
      lodepng_chunk_create(&state.info_png.unknown_chunks_data[1],
                           &state.info_png.unknown_chunks_size[1],
                           (unsigned)payload.size(), REGION_CHUNK_TYPE, &payload[0]);
      err = lodepng::encode(png, &image[0], W, H, state);
      check(err == 0, "encode PNG carrying an out-of-bounds table (for decode test)");
      e = decode_with_regions(&full, &ew, &eh, &decoded, png, nofail);
      check(e == RERR_OUT_OF_BOUNDS, "decode reports out-of-bounds region");
      printf("  decode error: %s\n", region_error_str(e));
    }

    /* 4c: configurable region count limit */
    {
      RegionConfig tiny = cfg; tiny.max_regions = 1;
      Region a; a.name = "a"; a.x = 0; a.y = 0; a.w = 4; a.h = 4;
      Region b; b.name = "b"; b.x = 8; b.y = 8; b.w = 4; b.h = 4;
      rs.push_back(a); rs.push_back(b);
      e = encode_with_regions(&png, &image[0], W, H, rs, tiny);
      check(e == RERR_TOO_MANY, "encode rejects table exceeding max_regions=1");
      printf("  encode error: %s\n", region_error_str(e));
      rs.clear();
    }

    /* 4d: extracting an unknown name */
    {
      Region a; a.name = "only"; a.x = 0; a.y = 0; a.w = 4; a.h = 4; rs.push_back(a);
      e = encode_with_regions(&png, &image[0], W, H, rs, cfg);
      check(e == RERR_OK, "encode ok for unknown-name test");
      e = decode_with_regions(&full, &ew, &eh, &decoded, png, cfg);
      check(e == RERR_OK, "decode ok for unknown-name test");
      {
        std::vector<unsigned char> block;
        e = region_extract(&block, &full[0], ew, eh, decoded, "missing");
        check(e == RERR_NOT_FOUND, "extract of unknown name reports not found");
        printf("  extract error: %s\n", region_error_str(e));
      }
      rs.clear();
    }
    printf("\n");
  }

  if(g_failures == 0) {
    printf("all region table tests passed\n");
    return 0;
  }
  printf("%d check(s) FAILED\n", g_failures);
  return 1;
}
