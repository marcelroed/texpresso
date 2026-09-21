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

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "synctex.h"
#include "editor.h"
#include "myabort.h"

struct int_buffer
{
  int *ptr, len, cap;
};

enum kind {
  STEX_ENTER_V,
  STEX_ENTER_H,
  STEX_LEAVE_V,
  STEX_LEAVE_H,
  STEX_CURRENT,
  STEX_KERN,
  STEX_GLUE,
  STEX_MATH,
  STEX_OTHER,
};

struct link
{
  int tag, line, column;
};

struct point
{
  int x, y;
};

struct size
{
  int width, height, depth;
};

struct record
{
  enum kind kind;
  struct link link;
  struct point point;
  struct size size;
};

static bool synctex_input_closed(fz_context *ctx, synctex_t *stx, unsigned index);

static void ib_init(struct int_buffer *ob)
{
  *ob = (struct int_buffer){.ptr = NULL, .len = 0, .cap = 0};
}

static void ib_free(fz_context *ctx, struct int_buffer *ib)
{
  if (ib->ptr)
    fz_free(ctx, ib->ptr);
  ib_init(ib);
}

static int int_abs(int i)
{
  return (i < 0) ? -i : i;
}

static void ib_rollback(fz_context *ctx, struct int_buffer *ib, int offset)
{
  while (ib->len > 0 && int_abs(ib->ptr[ib->len - 1]) >= offset)
    ib->len -= 1;
}

static void ib_append(fz_context *ctx, struct int_buffer *ib, int offset)
{
  if (ib->len >= ib->cap)
  {
    if (ib->len != 0)
    {
      int cap = ib->cap * 2;
      int *ptr = fz_malloc_array(ctx, cap, int);
      memcpy(ptr, ib->ptr, sizeof(int) * ib->cap);
      fz_free(ctx, ib->ptr);
      ib->ptr = ptr;
      ib->cap = cap;
    }
    else
    {
      ib->ptr = fz_malloc_array(ctx, 32, int);
      ib->cap = 32;
    }
  }
  ib->ptr[ib->len] = offset;
  ib->len += 1;
}

// Set TEXPRESSO_SYNCTEX_DEBUG in the environment to trace forward search.
static int synctex_debug = -1;

struct synctex_s
{
  struct int_buffer input_off, page_off, close_off, close_inp;
  int bol, cur;

  /* Backward search state */

  /* Step 0. Initiating search. */

  /* The file:line being searched for, or target_path[0] == 0 if there is no
     search going on. */
  char target_path[1024];
  int target_line;
  /* Column within target_line (0-based, number of characters before the
     cursor), or -1 when the editor did not provide one. Records carry the
     column of the input reader when TeXpresso's engine wrote them; the
     candidate is then refined to the record closest to the target column. */
  int target_column;

  /* The page that was being displayed when the search started.
     The heuristics uses it to pick the match closest to the current page when
     there are multiple matches. */
  int target_current_page;

  /* Step 1. Finding input file. */

  /* Find the SyncTeX input tag corresponding to target_path (if there is a search going on).

     If input_found, then the input tag is in input_tag.
     If !input_found, then input_tag is the number of inputs that have already been checked.
     Search should resume from inputs.ptr[input_tag].
   */
  int input_tag, input_found;

  /* Step 2. Scanning pages. */

  /* The number of pages that have been scanned so far.
     Search should resume from pages.ptr[scanned_pages * 2]. */
  int scanned_pages;

  /* Best candidate so far (or candidate_page == -1 if there has been no match). */
  int candidate_page, candidate_line, candidate_x, candidate_y;
  /* The typeset line (innermost horizontal box) holding the candidate, in
     synctex units, or an empty rectangle when unknown. */
  fz_irect candidate_box;
  /* Column of the record the candidate was derived from, and whether the
     search is still open because the target column lies beyond every record
     of the target line seen so far: a paragraph split across pages continues
     on the next page with larger columns. */
  int candidate_column, candidate_open;
};

synctex_t *synctex_new(fz_context *ctx)
{
  if (synctex_debug < 0)
    synctex_debug = getenv("TEXPRESSO_SYNCTEX_DEBUG") != NULL;
  synctex_t *stx = fz_malloc_struct(ctx, synctex_t);
  ib_init(&stx->input_off);
  ib_init(&stx->page_off);
  ib_init(&stx->close_off);
  ib_init(&stx->close_inp);
  stx->cur = 0;
  stx->target_path[0] = 0;
  return stx;
}

void synctex_free(fz_context *ctx, synctex_t *stx)
{
  ib_free(ctx, &stx->input_off);
  ib_free(ctx, &stx->page_off);
  ib_free(ctx, &stx->close_off);
  ib_free(ctx, &stx->close_inp);
  fz_free(ctx, stx);
}

int synctex_has_target(synctex_t *stx)
{
  return stx && (stx->target_path[0] != 0);
}

void synctex_rollback(fz_context *ctx, synctex_t *stx, size_t offset)
{
  ib_rollback(ctx, &stx->page_off, offset);
  ib_rollback(ctx, &stx->input_off, offset);
  ib_rollback(ctx, &stx->close_off, offset);

  while (stx->close_inp.len > stx->close_off.len)
  {
    stx->close_inp.len -= 1;
    int index = stx->close_inp.ptr[stx->close_inp.len];
    if (index < stx->input_off.len)
    {
      if (!synctex_input_closed(ctx, stx, index))
        myabort();
      stx->input_off.ptr[index] = -stx->input_off.ptr[index];
    }
  }

  if (stx->cur > offset)
    stx->cur = offset;

  if (synctex_has_target(stx))
  {
    if (stx->input_tag >= stx->input_off.len)
    {
      stx->input_tag = stx->input_off.len;
      stx->input_found = 0;
    }
    else
    {
      int pages = stx->page_off.len / 2;
      if (stx->scanned_pages > pages)
        stx->scanned_pages = pages;
      if (stx->candidate_page > pages)
        stx->candidate_page = -1;
    }
  }
}

static const uint8_t *string_parse_int(const uint8_t *string, int *i)
{
  *i = 0;
  int neg = 0;
  if (*string == '-')
  {
    neg = 1;
    string += 1;
  }
  while (*string >= '0' && *string <= '9')
  {
    *i = *i * 10 + (*string - '0');
    string += 1;
  }
  if (neg)
    *i = - *i;
  return string;
}

static const uint8_t *string_skip_prefix(const uint8_t *string, const char *prefix)
{
  const uint8_t *ptr = (void *)prefix;
  while (*ptr)
  {
    if (*ptr == *string)
    {
      ptr += 1;
      string += 1;
    }
    else
      return NULL;
  }
  return string;
}

static void synctex_process_line(fz_context *ctx, synctex_t *stx, int offset, const uint8_t *bol, uint8_t *eol)
{
  int index = 0;
  uint8_t c = *bol;
  bol += 1;

  switch (c)
  {
    case '{': case '}':
    {
      int is_closing = (c == '}');
      if (!(bol = string_parse_int(bol, &index))) break;
      if (index != stx->page_off.len / 2 + 1 || is_closing != (stx->page_off.len & 1))
      {
        fprintf(stderr, "[synctex] Invalid page index: index=%d/is_closing=%d expected=%d/%d\n",
                index, is_closing, stx->page_off.len / 2 + 1, stx->page_off.len & 1);
        myabort();
      }
      ib_append(ctx, &stx->page_off, offset);
      break;
    }

    case 'I':
    {
      if (!(bol = string_skip_prefix(bol, "nput:"))) break;
      if (!(bol = string_parse_int(bol, &index))) break;
      if (!(bol = string_skip_prefix(bol, ":"))) break;
      if (index != stx->input_off.len + 1)
      {
        fprintf(stderr, "[synctex] Invalid input index: index=%d expected=%d\n",
                index, stx->input_off.len + 1);
        myabort();
      }
      ib_append(ctx, &stx->input_off, offset);
      editor_notify_file_opened(index, (const char *)bol, eol - bol);
      break;
    }

    case '/':
    {
      if (!(bol = string_parse_int(bol, &index))) break;
      fprintf(stderr, "[synctex] Closed input: %d\n", index);
      index -= 1;
      if (index < 0 || index >= stx->input_off.len) myabort();
      if (synctex_input_closed(ctx, stx, index))
        myabort();
      stx->input_off.ptr[index] = -stx->input_off.ptr[index];
      if (stx->close_off.len != stx->close_inp.len) myabort();
      ib_append(ctx, &stx->close_off, offset);
      ib_append(ctx, &stx->close_inp, index);
      break;
    }

    default:
      break;
  }
}

void synctex_update(fz_context *ctx, synctex_t *stx, fz_buffer *buf)
{
  int cur = stx->cur, len = buf->len;

  if (len <= cur)
  {
    if (len < cur)
      synctex_rollback(ctx, stx, len);
    return;
  }

  uint8_t *ptr = buf->data;
  int bol = stx->bol;

  if (bol > cur)
  {
    bol = cur;
    while (bol > 0 && ptr[bol - 1] != '\n')
      bol -= 1;
  }

  while (cur < len)
  {
    if (ptr[cur] == '\n')
    {
      if (cur > bol)
        synctex_process_line(ctx, stx, bol, ptr + bol, ptr + cur);
      cur += 1;
      // fprintf(stderr, "synctex: %.*s\n", (int)(cur - bol - 1), &ptr[bol]);
      bol = cur;
    }
    else
      cur += 1;
  }

  stx->bol = bol;
  stx->cur = cur;
}

int synctex_page_count(synctex_t *stx)
{
  return stx ? stx->page_off.len / 2 : 0;
}

int synctex_input_count(synctex_t *stx)
{
  return stx ? stx->input_off.len : 0;
}

void synctex_page_offset(fz_context *ctx, synctex_t *stx, unsigned index, int *bop, int *eop)
{
  if (index * 2 + 1 >= stx->page_off.len)
    myabort();

  *bop = stx->page_off.ptr[2 * index + 0];
  *eop = stx->page_off.ptr[2 * index + 1];
}

int synctex_input_offset(fz_context *ctx, synctex_t *stx, unsigned index)
{
  if (index >= stx->input_off.len)
    myabort();

  return int_abs(stx->input_off.ptr[index]);
}

static bool synctex_input_closed(fz_context *ctx, synctex_t *stx, unsigned index)
{
  if (index >= stx->input_off.len)
    myabort();

  return stx->input_off.ptr[index] < 0;
}

static const uint8_t *nextline(const uint8_t *ptr)
{
  while (*ptr != '\n')
    ptr++;
  return ptr + 1;
}

static const uint8_t *
skip_tree(const uint8_t *ptr, uint8_t open, uint8_t close)
{
  int nest = 1;
  while (nest > 0)
  {
    if (*ptr == open)
      nest += 1;
    else if (*ptr == close)
      nest -= 1;
    else if (*ptr == '}')
      break;
    ptr = nextline(ptr);
  }
  return ptr;
}

static const uint8_t *
skip_record(const uint8_t *ptr, struct record *r)
{
  switch (r->kind)
  {
    case STEX_ENTER_H:
      return skip_tree(ptr, '(', ')');
    case STEX_ENTER_V:
      return skip_tree(ptr, '[', ']');
    default:
      return ptr;
  }
}

static _Bool
parse_link(const uint8_t **ptr, struct link *link)
{
  *ptr = string_parse_int(*ptr, &link->tag);
  if (**ptr != ',')
    return 0;
  *ptr = string_parse_int(*ptr + 1, &link->line);
  if (**ptr == ',')
    *ptr = string_parse_int(*ptr + 1, &link->column);
  else
    link->column = -1;
  return 1;
}

static _Bool
parse_point(const uint8_t **ptr, struct point *point)
{
  *ptr = string_parse_int(*ptr, &point->x);
  if (**ptr != ',')
    return 0;
  if (**ptr == '=')
    *ptr += 1;
  else
    *ptr = string_parse_int(*ptr + 1, &point->y);
  return 1;
}

static _Bool
parse_size(const uint8_t **ptr, struct size *size)
{
  *ptr = string_parse_int(*ptr, &size->width);
  if (**ptr != ',')
    return 0;
  *ptr = string_parse_int(*ptr + 1, &size->height);
  if (**ptr != ',')
    return 0;
  *ptr = string_parse_int(*ptr + 1, &size->depth);
  return 1;
}

static const uint8_t *
parse_line(const uint8_t *ptr, struct record *r)
{
  if (!ptr) return NULL;
  if (ptr[-1] != '\n') myabort();

  int has_link = 0, has_point = 0, has_size = 0, has_width = 0;

  *r = (struct record){0, };

  switch (*ptr)
  {
    case 'x':
      r->kind = STEX_CURRENT;
      has_link = has_point = 1;
      break;

    case 'k':
      r->kind = STEX_KERN;
      has_link = has_point = has_width = 1;
      break;

    case 'g':
      r->kind = STEX_GLUE;
      has_link = has_point = 1;
      break;

    case '$':
      r->kind = STEX_MATH;
      has_link = has_point = 1;
      break;

    case '(':
      r->kind = STEX_ENTER_H;
      has_link = has_point = has_size = 1;
      break;

    case ')':
      r->kind = STEX_LEAVE_H;
      break;

    case '[':
      r->kind = STEX_ENTER_V;
      has_link = has_point = has_size = 1;
      break;

    case ']':
      r->kind = STEX_LEAVE_V;
      break;

    case '}':
      return NULL;

    default:
      r->kind = STEX_OTHER;
      break;
  }

  ptr += 1;

  if (has_link && !parse_link(&ptr, &r->link))
    myabort();

  if (has_point && ((*ptr++ != ':') || !parse_point(&ptr, &r->point)))
    myabort();

  if (has_size && ((*ptr++ != ':') || !parse_size(&ptr, &r->size)))
    myabort();

  if (has_width)
  {
    if (*ptr++ != ':')
      myabort();
    ptr = string_parse_int(ptr, &r->size.width);
  }

  return nextline(ptr);
}

struct candidate {
  double area;
  fz_irect rect;
  struct link link;
  int len;
  const char *filename;
};

static double rect_area(fz_irect r)
{
  return (double)(r.y1 - r.y0) * (double)(r.x1 - r.x0);
}

static int get_filename(synctex_t *stx, fz_buffer *buf, struct candidate *c, int tag)
{
  if (tag <= 0)
    return 0;

  const uint8_t *filename = &buf->data[int_abs(stx->input_off.ptr[tag - 1])];
  while (*filename != ':')
    filename++;
  filename++;
  while (*filename != ':')
    filename++;
  filename++;
  const uint8_t *fend = filename;
  while (*fend != '\n')
    fend++;
  int len = (fend - filename);

  if (len)
  {
    c->filename = (const char *)filename;
    c->len = len;
  }

  // fprintf(stderr, "tag:%d, filename:%.*s\n", tag, len, filename);

  return len;
}

// Clicks between two typeset lines (or a little above/below a line) snap to
// the nearest line box within this vertical distance, in sp (12pt).
#define STX_GAP_TOLERANCE (12 * 65536)
// Vertical distance dominates the score: a line box a few points away beats
// any horizontally-nearer node on a farther line.
#define STX_DY_WEIGHT 1e14
// Vertical boxes are only a fallback when no line box or inner node matches.
#define STX_VBOX_PENALTY 1e20

static void
parse_tree(synctex_t *stx, fz_buffer *buf, const uint8_t *ptr, int x, int y, struct candidate *c)
{
  int nest = 0;
  struct { enum kind kind; struct size size; int dy; } saved[256];

  struct record r = {0,};
  while ((ptr = parse_line(ptr, &r)))
  {
    fz_irect rect;
    rect.x0 = r.point.x;
    rect.x1 = r.point.x + r.size.width;
    rect.y0 = r.point.y - r.size.height;
    rect.y1 = r.point.y + r.size.depth;
    switch (r.kind)
    {
      case STEX_CURRENT:
      case STEX_KERN:
      case STEX_GLUE:
      case STEX_MATH:
      {
        // Kern, glue, math and "current" records carry no vertical extent of
        // their own, so they would only ever match a click exactly on the
        // baseline. Inherit the height and depth of the innermost enclosing
        // horizontal box (the typeset line) instead, as the reference SyncTeX
        // parser does. Otherwise the enclosing box always wins, and its line
        // number is where the paragraph was packed (the line after the
        // paragraph), not the source line of the material under the cursor.
        int dy = 0;
        if (nest > 0 && nest <= 256 && saved[nest - 1].kind == STEX_ENTER_H)
        {
          rect.y0 = r.point.y - saved[nest - 1].size.height;
          rect.y1 = r.point.y + saved[nest - 1].size.depth;
          dy = saved[nest - 1].dy;
        }
        else if (!(rect.y0 <= y && y <= rect.y1))
          break;

        if (rect.x0 < x)
          rect.x1 = x;
        else
        {
          rect.x1 = rect.x0;
          rect.x0 = x;
        }
        double score = dy * STX_DY_WEIGHT + rect_area(rect);
        if (score < c->area && get_filename(stx, buf, c, r.link.tag))
        {
          c->area = score;
          c->rect = rect;
          c->link = r.link;
        }
        break;
      }
      case STEX_ENTER_H:
      case STEX_ENTER_V:
      {
        int dy = 0;
        _Bool inside;
        double score;
        if (r.kind == STEX_ENTER_H)
        {
          // A horizontal box is a typeset line: accept clicks slightly above
          // or below it, so that clicking in the gap between two lines picks
          // the nearest line instead of falling through to the page box.
          if (y < rect.y0)
            dy = rect.y0 - y;
          else if (y > rect.y1)
            dy = y - rect.y1;
          inside = rect.x0 <= x && x < rect.x1 && dy <= STX_GAP_TOLERANCE;
          score = dy * STX_DY_WEIGHT + rect_area(rect);
        }
        else
        {
          inside = fz_is_point_inside_irect(x, y, rect);
          score = STX_VBOX_PENALTY + rect_area(rect);
        }
        if (inside)
        {
          if (score < c->area && get_filename(stx, buf, c, r.link.tag))
          {
            c->area = score;
            c->rect = rect;
            c->link = r.link;
          }
          if (nest < 256)
          {
            saved[nest].kind = r.kind;
            saved[nest].size = r.size;
            saved[nest].dy = dy;
          }
          nest += 1;
        }
        else
          ptr = skip_record(ptr, &r);
        break;
      }
      case STEX_LEAVE_H:
      case STEX_LEAVE_V:
        nest -= 1;
        if (nest < 0)
          return;
      case STEX_OTHER:
        continue;
    }
  }
}

static int get_input(fz_buffer *buf,
                      synctex_t *stx,
                      int index,
                      const char **name)
{
  const char *filename =
    (const char *)&buf->data[int_abs(stx->input_off.ptr[index])];
  while (*filename != ':')
    filename++;
  filename++;
  while (*filename != ':')
    filename++;
  filename++;
  *name = filename;
  const char *fend = filename;
  while (*fend != '\n')
    fend++;
  return (fend - filename);
}

void synctex_scan(fz_context *ctx,
                  synctex_t *stx,
                  fz_buffer *buf,
                  const char *doc_dir,
                  unsigned page,
                  int x,
                  int y)
{
  if (synctex_page_count(stx) <= page)
    return;

  int bop, eop;
  synctex_page_offset(ctx, stx, page, &bop, &eop);

  const uint8_t *ptr = &buf->data[bop];

  struct candidate c = {0,};
  c.area = INFINITY;

  parse_tree(stx, buf, ptr, x, y, &c);
  if (c.link.tag)
  {
    const char *fname;
    int len = get_input(buf, stx, c.link.tag-1, &fname);
    fprintf(stderr,
            "synctex best candidate: (%d,%d)-(%d,%d) "
            "file:%.*s line:%d column:%d\n",
            c.rect.x0, c.rect.y0, c.rect.x1, c.rect.y1,
            len, fname,
            c.link.line, c.link.column);
    editor_synctex(doc_dir, fname, len, c.link.line, c.link.column);
  }
}

void synctex_set_target(synctex_t *stx, int current_page, const char *path, int line, int column)
{
  if (!stx)
    return;

  if (!path)
  {
    stx->target_path[0] = 0;
    return;
  }

  int length = strlen(path);
  if (length >= sizeof(stx->target_path))
  {
    // FIXME: Warn about path too long?!
    abort();
  }

  memcpy(stx->target_path, path, length);
  stx->target_path[length] = 0;
  stx->target_line = line;
  stx->target_column = column;
  stx->target_current_page = current_page;

  stx->input_tag = 0;
  stx->input_found = 0;
}

static bool is_oneliner(enum kind k)
{
  return (k >= STEX_CURRENT && k <= STEX_MATH);
}

static bool synctex_find_input(fz_context *ctx, synctex_t *stx, fz_buffer *buf)
{
  if (stx->input_found)
    return 1;

  if (stx->input_tag == stx->input_off.len)
    return 0;

  int plen = strlen(stx->target_path);
  while (stx->input_tag < stx->input_off.len)
  {
    const char *fname;
    if ((plen != get_input(buf, stx, stx->input_tag, &fname)) ||
        strncmp(stx->target_path, fname, plen) != 0)
    {
      stx->input_tag += 1;
      continue;
    }

    int page = 0, 
      pages = synctex_page_count(stx), 
      offset = int_abs(stx->input_off.ptr[stx->input_tag]);
    while (page < pages && stx->page_off.ptr[page * 2 + 1] < offset)
      page += 1;
    stx->scanned_pages = page;
    stx->input_found = 1;
    stx->candidate_page = -1;
    stx->candidate_open = 0;
    return 1;
  }

  return 0;
}

static const uint8_t *synctex_page_pointer(fz_context *ctx, synctex_t *stx, fz_buffer *buf, int page)
{
  int bop, eop;
  synctex_page_offset(ctx, stx, page, &bop, &eop);
  return &buf->data[bop];
}

static void synctex_clear_search(synctex_t *stx)
{
  stx->target_path[0] = 0;
}

// The innermost horizontal box enclosing a record, as a rectangle in synctex
// units. Records inside vertical boxes only (or outside any box) get an empty
// rectangle.
struct box_stack {
  struct { enum kind kind; fz_irect rect; } items[256];
  int nest;
};

static void box_stack_enter(struct box_stack *bs, const struct record *r)
{
  if (bs->nest < 256)
  {
    bs->items[bs->nest].kind = r->kind;
    bs->items[bs->nest].rect.x0 = r->point.x;
    bs->items[bs->nest].rect.x1 = r->point.x + r->size.width;
    bs->items[bs->nest].rect.y0 = r->point.y - r->size.height;
    bs->items[bs->nest].rect.y1 = r->point.y + r->size.depth;
  }
  bs->nest += 1;
}

static void box_stack_leave(struct box_stack *bs)
{
  if (bs->nest > 0)
    bs->nest -= 1;
}

static fz_irect box_stack_line(const struct box_stack *bs)
{
  for (int i = (bs->nest < 256 ? bs->nest : 256) - 1; i >= 0; i--)
    if (bs->items[i].kind == STEX_ENTER_H)
      return bs->items[i].rect;
  return fz_empty_irect;
}

static void set_candidate(synctex_t *stx, int page, const struct record *r, fz_irect box, int *updated)
{
  stx->candidate_page = page;
  stx->candidate_x = r->point.x;
  stx->candidate_y = r->point.y;
  stx->candidate_line = r->link.line;
  stx->candidate_box = box;
  stx->candidate_column = r->link.column;
  stx->candidate_open = 0;
  *updated = 1;
}

// Refinement of an exact line match using the column information TeXpresso's
// engine stores in each record (the position of the input reader when the
// node was created). `best` is the first record whose column does not exceed
// the target column, `next` the first later record with a larger column, and
// `best_end` the last "current point" record between the two: those are
// emitted at the end of each run of characters and carry the (stale) column
// of the node before the run, so they only ever mark where the run that
// started at `best` ends. The target position is interpolated between best
// and the end of its run, proportionally to the column distance.
struct column_match {
  struct record best, best_end, next;
  fz_irect best_box;
  int has_best, has_best_end, has_next;
};

static void column_match_feed(struct column_match *cm, const struct record *r, fz_irect box, int target_column)
{
  if (r->link.column < 0)
    return;
  if (r->kind == STEX_CURRENT)
  {
    if (cm->has_best && !cm->has_next)
    {
      cm->best_end = *r;
      cm->has_best_end = 1;
    }
    return;
  }
  if (r->link.column <= target_column)
  {
    if (!cm->has_best || r->link.column > cm->best.link.column)
    {
      cm->best = *r;
      cm->best_box = box;
      cm->has_best = 1;
      cm->has_best_end = 0;
      cm->has_next = 0;
    }
  }
  else if (cm->has_best && !cm->has_next)
  {
    cm->next = *r;
    cm->has_next = 1;
  }
}

static struct record column_match_result(const struct column_match *cm, int target_column)
{
  struct record r = cm->best;
  int c0 = cm->best.link.column;
  if (target_column <= c0)
    return r;

  // Only interpolate along one baseline: a run that was hyphenated across
  // typeset lines would otherwise place the mark in the void between them.
  int end_x = -1;
  if (cm->has_best_end && cm->best_end.point.y == r.point.y)
    end_x = cm->best_end.point.x;
  else if (cm->has_next && cm->next.point.y == r.point.y)
    end_x = cm->next.point.x;

  if (end_x < r.point.x)
    return r;

  if (cm->has_next && cm->next.link.column > c0)
  {
    double t = (double)(target_column - c0) / (cm->next.link.column - c0);
    if (t > 1.0) t = 1.0;
    r.point.x = r.point.x + (int)((end_x - r.point.x) * t);
  }
  else if (target_column > c0 + 1)
    // Nothing recorded after the run (end of paragraph): the cursor is
    // somewhere in or after the last word, show its end.
    r.point.x = end_x;
  return r;
}

static void
synctex_backscan_page(fz_context *ctx, synctex_t *stx, fz_buffer *buf, int page, int *updated_candidate)
{
  int tag = stx->input_tag + 1;
  int line = stx->target_line;
  const uint8_t *ptr = synctex_page_pointer(ctx, stx, buf, page);

  struct record r = {0,}, r0;
  r0.link.tag = -1;

  int had_record = 0;

  struct box_stack boxes = {.nest = 0};

  // First record on this page whose line is past the target, if any.
  // Material from later source lines can precede the target on a page: an
  // enclosing box packed later (a tabular, a minipage, a float) is recorded
  // before its content. So seeing such a record does not end the search;
  // the rest of the page is still scanned for a closer record.
  struct record past = {0,};
  fz_irect past_box = fz_empty_irect;
  int past_seen = 0;

  // Records on the target line itself, when refining by column.
  struct column_match cm = {.has_best = 0};
  int use_column = stx->target_column >= 0;
  int exact_seen = 0;
  // Last one-line record before the first record of the target line: when
  // it sits on the same baseline, it marks where the material of the target
  // line starts (the source line began mid-paragraph).
  struct record prev = {0,};
  int has_prev = 0;
  // First record of the target line, and whether the line's records carry
  // more than one column. They all share one when the material was typeset
  // from a token list (the body of amsmath's align is collected and typeset
  // at its \end line): the column is then the reader position after the
  // macro and says nothing about the position within the line.
  struct record first_exact = {0,};
  fz_irect first_exact_box = fz_empty_irect;
  int columns_vary = 0;

  while ((ptr = parse_line(ptr, &r)))
  {
    if (r.kind == STEX_ENTER_H || r.kind == STEX_ENTER_V)
      box_stack_enter(&boxes, &r);
    else if (r.kind == STEX_LEAVE_H || r.kind == STEX_LEAVE_V)
      box_stack_leave(&boxes);

    // Remember the first location of the page to skip it:
    // it is the location where the shipout procedure was invoked
    // not the location of actual source contents
    if (r0.link.tag == -1 && (r.kind == STEX_ENTER_H || STEX_ENTER_V))
    {
      r0 = r;

      // Heuristic: if the targetted line is just at the beginning of the next
      // page and before and at the instruction that trigerred the flush, it is
      // useful to use the top of next page as an approximation.
      // (maybe there won't be any other synctex record to attach to).
      if (r0.link.tag == tag && r0.link.line < line)
        return;
      continue;
    }

    if (is_oneliner(r.kind) && !exact_seen &&
        !(r.link.tag == tag && r.link.line == line))
    {
      prev = r;
      has_prev = 1;
    }

    if (is_oneliner(r.kind) && r.link.tag == tag)
    {
      if (r.link.tag == r0.link.tag && r.link.line == r0.link.line)
        // Skip other occurrences of the first location of the page: it doesn't
        // belong to it.
        continue;

      // Remember we processed at least one record
      had_record = 1;

      fz_irect box = box_stack_line(&boxes);

      // Remember that we have seen at least one record
      // Check if candidate: the closest record at or before the target line.
      // On a new page any such record supersedes an older candidate; within a
      // page, a record only supersedes one that is not closer to the target.
      if (r.link.line <= line)
      {
        if (r.link.line == line)
        {
          // An exact match is definitive. With column information, keep
          // scanning the page for the record nearest to the target column.
          if (!exact_seen)
          {
            exact_seen = 1;
            past = r;
            past_box = box;
            past_seen = 1;
            first_exact = r;
            first_exact_box = box;
            if (use_column && r.link.column > 0)
            {
              // Synthetic record for column 0: the end of the preceding
              // material on this baseline, or the left edge of the line box
              // (a paragraph start, give or take the indentation).
              struct record start = r;
              start.kind = STEX_GLUE;
              start.link.column = 0;
              if (has_prev && prev.point.y == r.point.y && prev.point.x < r.point.x)
                start.point.x = prev.point.x;
              else if (!fz_is_empty_irect(box) && box.x0 < r.point.x)
                start.point.x = box.x0;
              else
                start.kind = STEX_OTHER;
              if (start.kind == STEX_GLUE)
                column_match_feed(&cm, &start, box, stx->target_column);
            }
          }
          if (use_column)
          {
            if (r.link.column != first_exact.link.column)
              columns_vary = 1;
            if (synctex_debug)
              fprintf(stderr, "[synctex forward] page %d line %d record kind %d column %d at (%d, %d)\n",
                      page, r.link.line, r.kind, r.link.column, r.point.x, r.point.y);
            column_match_feed(&cm, &r, box, stx->target_column);
            continue;
          }
          break;
        }
        if (stx->candidate_page != page || r.link.line >= stx->candidate_line)
          set_candidate(stx, page, &r, box, updated_candidate);
      }
      else
      {
        if (stx->candidate_page == -1)
          set_candidate(stx, page, &r, box, updated_candidate);
        if (!past_seen || r.link.line < past.link.line)
        {
          past = r;
          past_box = box;
        }
        past_seen = 1;
      }
    }
  }

  // A candidate on the target line from a previous page, whose column search
  // is still open (the target column was past all of that page's records).
  int prev_open = stx->candidate_page != -1 && stx->candidate_open;

  if (exact_seen && cm.has_best)
  {
    if (columns_vary)
    {
      past = column_match_result(&cm, stx->target_column);
      past_box = cm.best_box;
    }
    else
    {
      // Uninformative columns: the start of the line's material.
      past = first_exact;
      past_box = first_exact_box;
      cm.best = first_exact;
      cm.has_next = 1;
    }
  }

  // Definitive match: the target line, or a line past it, was seen on this
  // page, so later pages cannot hold a closer record.
  if (past_seen)
  {
    if (exact_seen)
    {
      if (use_column && cm.has_best)
      {
        // Refined position. It supersedes an open candidate from a previous
        // page only if it is closer to the target column (a synthetic
        // column-0 start on this page is not).
        if (!prev_open || cm.best.link.column > stx->candidate_column)
          set_candidate(stx, page, &past, past_box, updated_candidate);
        if (!cm.has_next)
        {
          // Nothing past the target column on this page: the paragraph may
          // continue on the next page. Keep looking.
          stx->candidate_open = 1;
          return;
        }
      }
      else if (!prev_open)
        // The (refined) position on the target line supersedes whatever
        // earlier line was recorded as a candidate, on this page or a
        // previous one.
        set_candidate(stx, page, &past, past_box, updated_candidate);
    }
    else if (!prev_open)
    {
      // No record of the target line, but a later line has some (on this
      // page; an earlier line may have some on this or a previous page).
      // TeX attributes material to the source line where it was finished:
      // a paragraph's lines to the line that ended the paragraph, the body
      // of a collected environment (amsmath's align, a caption, ...) to its
      // \end line. So the first record of the nearest later line is a better
      // guess for the target than the last record of an earlier line.
      if (synctex_debug)
        fprintf(stderr, "[synctex forward] page %d: no record for line %d, using first record of line %d at (%d, %d)\n",
                page, line, past.link.line, past.point.x, past.point.y);
      set_candidate(stx, page, &past, past_box, updated_candidate);
    }
    stx->candidate_open = 0;
    synctex_clear_search(stx);
    return;
  }

  // No record? Could be an empty page or a beamer page.
  if (!had_record)
  {
    // If it is ending after the target, we have a match or at least a candidate.
    if (r0.link.tag == tag && r0.link.line >= line)
    {
      // If we had no candidate, or the current record is not worse, update.
      if (stx->candidate_page == -1 ||
          (page <= stx->target_current_page && stx->candidate_line == r0.link.line))
        set_candidate(stx, page, &r0, fz_empty_irect, updated_candidate);
    }
  }
}

int synctex_find_target(fz_context *ctx, synctex_t *stx, fz_buffer *buf, int *page, int *x, int *y, fz_irect *box)
{
  if (!stx || !stx->target_path[0])
    return 0;

  if (!synctex_find_input(ctx, stx, buf))
    return 0;

  int pages = synctex_page_count(stx);
  int updated_candidate = 0;
  while (stx->target_path[0] && stx->scanned_pages < pages)
  {
    synctex_backscan_page(ctx, stx, buf, stx->scanned_pages, &updated_candidate);
    stx->scanned_pages += 1;
  }

  if (updated_candidate)
  {
    if (page) *page = stx->candidate_page;
    if (x) *x = stx->candidate_x;
    if (y) *y = stx->candidate_y;
    if (box) *box = stx->candidate_box;
  }

  if (synctex_input_closed(ctx, stx, stx->input_tag))
    synctex_clear_search(stx);

  return updated_candidate;
}
