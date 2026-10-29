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

#ifndef _RENDERER_H_
#define _RENDERER_H_

#include <stdbool.h>
#include <SDL2/SDL.h>
#include <mupdf/fitz.h>

typedef struct txp_renderer_s txp_renderer;

txp_renderer *txp_renderer_new(fz_context *ctx, SDL_Renderer *sdl);
void txp_renderer_free(fz_context *ctx, txp_renderer *r);

enum txp_fit_mode
{
  FIT_WIDTH,
  FIT_PAGE,
};

typedef struct
{
  float zoom;
  enum txp_fit_mode fit;
  // Offset of the middle of the current page from that of the window, in
  // pixels
  fz_point pan;
  // Vertical offset of the page past the end of its panning range (while
  // scrolling past it), in pixels
  float overscroll;
  bool crop, themed_color, invert_color;
  uint32_t background_color, foreground_color;
} txp_renderer_config;

struct fullscreen_state {
  txp_renderer_config windowed_backup;
  bool has_backup;
  bool prev_fs;
};

typedef struct
{
  // Bounds of the page being displayed (after cropping), in
  // document space.
  fz_rect page_bounds;

  // Size of the window where the document is displayed.
  fz_point window_size;

  // Size of the page being displayed, in window space.
  fz_point document_size;

  // Panning range from -pan_interval to pan_interval (of the page alone,
  // see txp_renderer_pan_range).
  fz_point pan_interval;
} txp_renderer_bounds;

// The renderer shows the current page between the previous and the next
// ones (when they are set), as a strip that scrolls continuously. Pages are
// designated by their place relative to the current one: -1, 0 or 1. The
// contents, the selection, the positions and the text are those of the
// current page, unless stated otherwise.

void txp_renderer_set_contents(fz_context *ctx, txp_renderer *self, fz_display_list *dl);
fz_display_list *txp_renderer_get_contents(fz_context *ctx, txp_renderer *self);
// Contents of the page at d, NULL for none
void txp_renderer_set_page(fz_context *ctx, txp_renderer *self, int d, fz_display_list *dl);
fz_display_list *txp_renderer_get_page(fz_context *ctx, txp_renderer *self, int d);
// Make the page at d (-1 or 1) the current one, and leave the place that
// opens on the other side empty. With keep_view, the pages stay where they
// are on screen; otherwise the new current page takes the place of the old
// one. The pages keep what they rendered.
void txp_renderer_shift(fz_context *ctx, txp_renderer *self, int d, bool keep_view);
txp_renderer_config *txp_renderer_get_config(fz_context* ctx, txp_renderer *self);

bool txp_renderer_page_bounds(fz_context *ctx, txp_renderer *self, txp_renderer_bounds *result);
bool txp_renderer_page_position(fz_context *ctx, txp_renderer *self, SDL_FRect *rect, fz_point *translate, float *scale);
// Range of the vertical pan, where the strip covers the window
bool txp_renderer_pan_range(fz_context *ctx, txp_renderer *self, float *min, float *max);
// The page in the middle of the window
int txp_renderer_middle_page(fz_context *ctx, txp_renderer *self);
// The page other than the current one at a screen position: -1 or 1, 0 if
// none
int txp_renderer_page_at(fz_context *ctx, txp_renderer *self, fz_point pt);
bool txp_renderer_page_visible(fz_context *ctx, txp_renderer *self, int d);

void txp_renderer_render(fz_context *ctx, txp_renderer *self);
void txp_renderer_set_scale_factor(fz_context *ctx, txp_renderer *self, fz_point scale);
// Returns whether there was a selection
bool txp_renderer_clear_selection(fz_context *ctx, txp_renderer *self);
bool txp_renderer_start_selection(fz_context *ctx, txp_renderer *self, fz_point pt);
bool txp_renderer_drag_selection(fz_context *ctx, txp_renderer *self, fz_point pt);
bool txp_renderer_select_word(fz_context *ctx, txp_renderer *self, fz_point pt);
bool txp_renderer_select_char(fz_context *ctx, txp_renderer *self, fz_point pt);
void txp_renderer_screen_size(fz_context *ctx, txp_renderer *self, int *w, int *h);
fz_point txp_renderer_screen_to_document(fz_context *ctx, txp_renderer *self, fz_point pt);
fz_point txp_renderer_document_to_screen(fz_context *ctx, txp_renderer *self, fz_point pt);
// Draw a caret like that of an empty selection at x, from y0 to y1
// (document coordinates), in the current draw color.
void txp_renderer_draw_caret(fz_context *ctx, txp_renderer *self, float x, float y0, float y1);

// Fold a character for text matching (lower case, no accents), or 0 when it
// does not take part (punctuation, spaces).
int txp_fold_char(int c);

#define TXP_TEXT_MAX_GAP 64
#define TXP_TEXT_LOOKAHEAD 5
// Flag of a gap that can start anywhere, not only at the start of a word
#define TXP_TEXT_GAP_ANYWHERE 0x80

// Find `needle` (folded characters, see txp_fold_char) in the text of the
// displayed page, ignoring characters that do not take part. Where gap[j]
// > 0 (gap can be NULL), up to gap[j] (at most TXP_TEXT_MAX_GAP) characters
// of the page that are not in the needle can come before needle[j], from
// the start of a word (anywhere if gap[j] has TXP_TEXT_GAP_ANYWHERE): the
// fewest after which the next TXP_TEXT_LOOKAHEAD
// characters of the needle (up to its next gap) match. With max_distance >= 0,
// take the match nearest to `anchor` in reading order, if its character at
// `offset` is at most max_distance characters from the one closest to the
// anchor (or if it is the only match, and the needle is long). Otherwise,
// prefer a match inside `region` (when not empty), then
// the first one after `anchor` in reading order. On success, *out is the
// left edge of the character at `offset` on its baseline (its right edge if
// `after`), and *out_line the bounding box of its text line.
bool txp_renderer_find_text(fz_context *ctx, txp_renderer *self,
                            const int *needle, const unsigned char *gap, int len,
                            int offset, bool after, fz_point anchor,
                            fz_rect region, int max_distance,
                            fz_point *out, fz_rect *out_line);

// The folded characters of the displayed page around `pt`, in reading
// order: up to `radius` characters on each side of the one nearest to pt.
// Returns how many were stored in `out` (at most 2 * radius + 1), 0 if pt
// is not on or next to a text line. *index is the character at pt, and
// *after is set when pt is past its middle.
int txp_renderer_text_at(fz_context *ctx, txp_renderer *self, fz_point pt,
                         int radius, int *out, int *index, bool *after);

// The character of the text of the current contents nearest to pt: its box,
// that of its line and its origin (if not NULL). Returns false if there is no
// text.
bool txp_renderer_nearest_char(fz_context *ctx, txp_renderer *self, fz_point pt,
                               fz_rect *char_box, fz_rect *line_box,
                               fz_point *origin);

// The character of the text of the current contents before the one nearest
// to pt, on the same line (spaces are left out): its box. Returns false if
// the one nearest to pt starts its line.
bool txp_renderer_char_before(fz_context *ctx, txp_renderer *self, fz_point pt,
                              fz_rect *char_box);

// Testing: write the text of the current contents as JSON, one entry per
// text line with the code point, box and origin of each character, in
// document units.
void txp_renderer_dump_text(fz_context *ctx, txp_renderer *self, FILE *f);

#endif /*!_RENDERER_H_*/
