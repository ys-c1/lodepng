/*
LodePNG version 20250929

Incremental, interruptible / resumable PNG decoder.

This is an ADD-ON to the lodepng public API. None of the existing lodepng
functions are changed. The functions below decode a non-interlaced PNG one
row block at a time (a row block is a fixed number of scanlines). After each
step the complete machine state can be serialized to a flat byte blob; a later
process can reload that blob and continue decoding from the exact same point.

The decoder uses the exact same internals as the one-shot lodepng decoder
(same zlib/inflate implementation, same unfilterScanline, same color
conversion), only driven in a per-row-block state machine instead of in one
call. Pixel output is therefore identical to lodepng_decode_memory.

Serialized state is a fixed 32-byte record holding only a row-level breakpoint
(image identity, dimensions, rows_done, zlib length). It deliberately stores
no mid-inflate state: on restore the deterministic decoder is replayed from
the start of the zlib stream up to rows_done, rebuilding an in-memory state
byte-identical to the suspended one (bit position, deflate block, any
in-flight LZ77 copy). This keeps the blob small enough to be no larger than
two row blocks even for narrow images, at the cost of an O(rows_done) replay
on each restore.

Limitations:
- non-interlaced PNGs only (interlaced PNGs return error 134);
- output color mode is fixed to 8-bit RGBA (LCT_RGBA, 8);
- the input PNG bytes must stay available and identical between resume calls
  (the state binds to them with a checksum, but stores no input data).
*/

#ifndef LODEPNG_INCREMENTAL_H
#define LODEPNG_INCREMENTAL_H

#include "lodepng.h"

/* Opaque handle to an incremental decoding session. */
typedef struct LodePNGIncremental LodePNGIncremental;

/*
Begin an incremental decode of the PNG in [in, in+insize).

On success returns 0 and:
- *out       points to the output image buffer (RGBA8, 4 * width * height
              bytes), allocated once here and filled in progressively;
- *width, *height are the image dimensions;
- *dec       is the session handle (destroy with lodepng_inc_destroy).

The output buffer and the handle are independent: the output buffer stays
valid and keeps its contents until freed with lodepng_free.
*/
unsigned lodepng_inc_begin(LodePNGIncremental** dec, unsigned char** out,
                           unsigned* width, unsigned* height,
                           const unsigned char* in, size_t insize);

/*
Decode one row block of at most max_rows scanlines. Returns:
- 0              : the whole image has been decoded (session finished);
- 130            : the block was decoded but more rows remain (not an error);
- any other value: a lodepng error code.

*rows_done (if non-NULL) receives the total number of scanlines decoded so far.
*/
unsigned lodepng_inc_step(LodePNGIncremental* dec, unsigned max_rows,
                          unsigned* rows_done);

/* Size in bytes of the serialized state, or 0 on error. */
size_t lodepng_inc_state_size(const LodePNGIncremental* dec);

/*
Serialize the resumable state into blob (must have room for
lodepng_inc_state_size bytes). The input PNG and the output image are not
part of the blob. Returns the number of bytes written, or 0 on error.
*/
size_t lodepng_inc_save(const LodePNGIncremental* dec, unsigned char* blob);

/*
Reload a state previously written with lodepng_inc_save and continue.

The same PNG bytes must be given again (in/insize, same content as at begin),
and out must point to the output image buffer with everything decoded so far
still in it (the per-row unfilter needs the previous reconstructed row).
The new session is written to *dec. Returns 0 on success.
*/
unsigned lodepng_inc_restore(LodePNGIncremental** dec,
                             const unsigned char* blob, size_t blobsize,
                             const unsigned char* in, size_t insize,
                             unsigned char* out);

/* Free a session. Does not free the output image buffer. */
void lodepng_inc_destroy(LodePNGIncremental* dec);

#endif /*LODEPNG_INCREMENTAL_H*/
