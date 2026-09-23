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

#include "mydvi.h"

dvi_context *dvi_context_new(fz_context *ctx, dvi_reshooks hooks)
{
  dvi_context *dc = fz_malloc_struct(ctx, dvi_context);

  dc->dev = NULL;
  dc->resmanager = dvi_resmanager_new(ctx, hooks);
  dvi_scratch_init(&dc->scratch);

  dvi_state *st = &dc->root;
  st->fonts = dvi_fonttable_new(ctx);
  st->gs.ctm = fz_identity;
  st->registers_stack.base = &dc->registers_stack[0];
  st->registers_stack.depth = 0;
  st->registers_stack.limit = sizeof(dc->registers_stack)/sizeof(dc->registers_stack[0]);
  st->gs_stack.base = &dc->gs_stack[0];
  st->gs_stack.depth = 0;
  st->gs_stack.limit = sizeof(dc->gs_stack)/sizeof(dc->gs_stack[0]);
  return dc;
}

static void
dvi_context_set_device(fz_context *ctx, dvi_context *dc, fz_device *dev)
{
  if (dev == dc->dev)
    return;
  if (dc->dev)
    fz_drop_device(ctx, dc->dev);
  dc->dev = dev;
  if (dev)
    fz_keep_device(ctx, dev);
}

void dvi_context_free(fz_context *ctx, dvi_context *dc)
{
  if (dc->links)
  {
    dvi_links_clear(ctx, dc->links);
    fz_free(ctx, dc->links->links);
    fz_free(ctx, dc->links->dests);
    fz_free(ctx, dc->links);
  }
  dvi_context_set_device(ctx, dc, NULL);
  dvi_resmanager_free(ctx, dc->resmanager);
  dvi_scratch_release(ctx, &dc->scratch);
  fz_free(ctx, dc);
}

void dvi_context_begin_frame(fz_context *ctx, dvi_context *dc, fz_device *dev)
{
  dvi_context_set_device(ctx, dc, dev);
  dvi_state *st = &dc->root;
  st->registers_stack.depth = 0;
  st->gs = (dvi_graphicstate){0,};
  st->gs.ctm = fz_identity;
  st->gs.ctm.d = -1;
  st->gs.ctm.e = 72;
  st->gs.ctm.f = 72;
  st->gs_stack.depth = 0;

  dc->colorstack.depth = 0;
  for (int i = 0; i < dc->pdfcolorstacks.capacity; i++)
    dc->pdfcolorstacks.stacks[i].depth = 0;

  if (dc->links)
    dvi_links_clear(ctx, dc->links);
}

void dvi_context_end_frame(fz_context *ctx, dvi_context *dc)
{
  dvi_scratch_clear(ctx, &dc->scratch);
  dvi_context_set_device(ctx, dc, NULL);

  if (dc->colorstack.depth > 0)
    fprintf(stderr, "default color stack: ending frame with %d colors\n", dc->colorstack.depth);

  for (int i = 0; i < dc->pdfcolorstacks.capacity; i++)
    if (dc->pdfcolorstacks.stacks[i].depth > 0)
      fprintf(stderr, "default color stack: ending frame with %d colors\n",
              dc->pdfcolorstacks.stacks[i].depth);
}

dvi_state *dvi_context_state(dvi_context *dc)
{
  return &dc->root;
}

bool dvi_state_enter_vf(dvi_context *dc, dvi_state *vfst, const dvi_state *st, dvi_fonttable *fonts, int font, fixed_t scale)
{
  float s = fixed_double(scale);
  vfst->version = DVI_VF;
  vfst->f = font;
  vfst->gs = st->gs;
  vfst->gs.ctm = fz_pre_scale(dvi_get_ctm(dc, st), s, s);
  vfst->gs.h = vfst->gs.v = 0;
  vfst->registers.h = 0;
  vfst->registers.v = 0;
  vfst->registers.x = 0;
  vfst->registers.y = 0;
  vfst->registers.w = 0;
  vfst->registers.z = 0;
  vfst->registers_stack.base = st->registers_stack.base + st->registers_stack.depth;
  vfst->registers_stack.limit = st->registers_stack.limit - st->registers_stack.depth;
  vfst->registers_stack.depth = 0;
  vfst->gs_stack.base = st->gs_stack.base + st->gs_stack.depth;
  vfst->gs_stack.limit = st->gs_stack.limit - st->gs_stack.depth;
  vfst->gs_stack.depth = 0;
  vfst->fonts = fonts;
  return 1;
}


/* Hyperlinks */

void dvi_links_clear(fz_context *ctx, dvi_links *l)
{
  fz_free(ctx, l->active);
  l->active = NULL;
  for (int i = 0; i < l->link_count; ++i)
    fz_free(ctx, l->links[i].target);
  for (int i = 0; i < l->dest_count; ++i)
    fz_free(ctx, l->dests[i].name);
  l->link_count = 0;
  l->dest_count = 0;
}

static void push_link(fz_context *ctx, dvi_links *l)
{
  if (!l->active || fz_is_empty_rect(l->rect))
    return;
  if (l->link_count == l->link_cap)
  {
    int cap = l->link_cap ? l->link_cap * 2 : 16;
    l->links = fz_realloc_array(ctx, l->links, cap, dvi_link);
    l->link_cap = cap;
  }
  l->links[l->link_count].rect = l->rect;
  l->links[l->link_count].target = fz_strdup(ctx, l->active);
  l->link_count += 1;
}

void dvi_links_begin(fz_context *ctx, dvi_links *l, const char *target)
{
  dvi_links_end(ctx, l);
  l->active = fz_strdup(ctx, target);
  l->rect = fz_empty_rect;
}

void dvi_links_end(fz_context *ctx, dvi_links *l)
{
  push_link(ctx, l);
  fz_free(ctx, l->active);
  l->active = NULL;
}

void dvi_links_add_dest(fz_context *ctx, dvi_links *l, const char *name, fz_point pt)
{
  if (l->dest_count == l->dest_cap)
  {
    int cap = l->dest_cap ? l->dest_cap * 2 : 16;
    l->dests = fz_realloc_array(ctx, l->dests, cap, dvi_dest);
    l->dest_cap = cap;
  }
  l->dests[l->dest_count].name = fz_strdup(ctx, name);
  l->dests[l->dest_count].pt = pt;
  l->dest_count += 1;
}

void dvi_links_add_glyph(fz_context *ctx, dvi_links *l, fz_rect box)
{
  if (!l->active || fz_is_empty_rect(box))
    return;
  if (fz_is_empty_rect(l->rect))
  {
    l->rect = box;
    return;
  }
  // A glyph that is not on the line of the current rectangle starts a new
  // one: a link broken across lines is clickable on each piece.
  float cy = (box.y0 + box.y1) / 2;
  if (cy < l->rect.y0 || cy > l->rect.y1 || box.x1 < l->rect.x0)
  {
    push_link(ctx, l);
    l->rect = box;
  }
  else
    l->rect = fz_union_rect(l->rect, box);
}
