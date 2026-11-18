/* Validate integer (non-wrapping) Paeth distance math that SSE2 can compute
with 16-bit lanes, against lodepng's paethPredictor over all 2^24 inputs. */
#include "lodepng.cpp"
#include <cstdio>
#include <cstdlib>

static unsigned abs8(unsigned r) { return r < 128 ? r : 255 - r; }

/* scalar mirror of the planned SSE2 16-bit-lane sequence */
static unsigned char paeth_simd_form(int A, int B, int C) {
  int p = A + B - C;                 /* range -255..510 */
  int da = std::abs(p - A);          /* = |B-C| */
  int db = std::abs(p - B);          /* = |A-C| */
  int dc = std::abs(p - C);          /* = |A+B-2C| */
  int pred = A, best = da;
  if(db < best) { pred = B; best = db; }
  if(dc < best) pred = C;
  return (unsigned char)pred;
}

int main() {
  long mism = 0;
  for(int a = 0; a < 256; ++a)
    for(int b = 0; b < 256; ++b)
      for(int c = 0; c < 256; ++c) {
        unsigned char ref = paethPredictor((unsigned char)a,(unsigned char)b,(unsigned char)c);
        unsigned char got = paeth_simd_form(a,b,c);
        if(ref != got) { if(mism < 8) std::printf("MISMATCH a=%d b=%d c=%d ref=%u got=%u\n",a,b,c,ref,got); ++mism; }
      }
  std::printf("integer paeth form vs paethPredictor: %ld mismatches / 16777216\n", mism);

  long amism = 0;
  for(unsigned r = 0; r < 256; ++r) if(abs8(r) != (r < 128 ? r : 255 - r)) ++amism;
  std::printf("abs8: %ld mismatches / 256\n", amism);
  return (mism == 0 && amism == 0) ? 0 : 1;
}
