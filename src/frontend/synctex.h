/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef SYNCTEX_H_
#define SYNCTEX_H_

#include <mupdf/fitz/context.h>
#include <mupdf/fitz/buffer.h>
#include <stdbool.h>

typedef struct synctex_s synctex_t;

synctex_t *synctex_new(fz_context *ctx);
void synctex_free(fz_context *ctx, synctex_t *stx);
void synctex_rollback(fz_context *ctx, synctex_t *stx, size_t offset);
void synctex_update(fz_context *ctx, synctex_t *stx, fz_buffer *buf);
int synctex_page_count(synctex_t *stx);
int synctex_input_count(synctex_t *stx);
void synctex_page_offset(fz_context *ctx, synctex_t *stx, unsigned index, int *bop, int *eop);
int synctex_input_offset(fz_context *ctx, synctex_t *stx, unsigned index);
// Name of the input file with a tag (as in the records), pointing into buf;
// returns its length, 0 if the tag is unknown.
int synctex_input_name(synctex_t *stx, fz_buffer *buf, int tag, const char **name);
// Backward search: the source of the material at (x, y) on a page. On
// success, *name and *name_len are the input file name (pointing into buf),
// *line is 1-based and *column the position of the engine's input reader
// when the material was read (0-based, in characters, -1 if unknown).
bool synctex_scan(fz_context *ctx, synctex_t *stx, fz_buffer *buf,
                  unsigned page, int x, int y,
                  const char **name, int *name_len, int *line, int *column);

int synctex_has_target(synctex_t *stx);
// column is 0-based (characters before the cursor), or -1 if unknown.
void synctex_set_target(synctex_t *stx, int current_page, const char *path, int line, int column);
// On a hit, *box receives the typeset line holding the position (empty
// rectangle if unknown), in the same units as *x and *y.
// How precise the last candidate found by synctex_find_target is: 0, or
// SYNCTEX_IMPRECISE when it locates the target line only coarsely (no record
// of the line, or no usable column), plus SYNCTEX_FLOATING when its
// coordinates are not where the material is drawn (inside a TikZ picture);
// the box is then the enclosing picture. SYNCTEX_OTHER_LINE is set when the
// target line has no record and the candidate is the nearest other line.
#define SYNCTEX_IMPRECISE 1
#define SYNCTEX_FLOATING 2
#define SYNCTEX_OTHER_LINE 4
int synctex_candidate_imprecise(synctex_t *stx);
int synctex_find_target(fz_context *ctx, synctex_t *stx, fz_buffer *buf, int *page, int *x, int *y, fz_irect *box);

#endif // SYNCTEX_H_
