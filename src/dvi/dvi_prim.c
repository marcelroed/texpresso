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

#include <string.h>
#include "mydvi.h"
#include "mydvi_interp.h"
#include "fz_util.h"

#define color_params fz_default_color_params

static void output_fill_rect(fz_context *ctx, dvi_context *dc, dvi_state *st, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
  if (ctx && dc->dev)
  {
    float s = dc->scale;
    fz_path *path = fz_new_path(ctx);
    fz_rectto(ctx, path, x0 * s, - y0 * s, x1 * s, - y1 * s);
    fz_fill_path(ctx, dc->dev, path, 0, st->gs.ctm, fz_device_rgb(ctx),
                 st->gs.colors.fill, 1.0, color_params);
    fz_drop_path(ctx, path);
  }
}

static void output_debug_rect(fz_context *ctx, dvi_context *dc, dvi_state *st, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
  if (ctx && dc->dev)
  {
    fz_path *path = fz_new_path(ctx);
    fz_rectto(ctx, path, x0 * dc->scale, - y0 * dc->scale, x1 * dc->scale, - y1 * dc->scale);
    fz_stroke_path(ctx, dc->dev, path, &fz_default_stroke_state, st->gs.ctm,
                   fz_device_rgb(ctx), st->gs.colors.line, 0.8, color_params);
    fz_drop_path(ctx, path);
  }
}

void dvi_context_flush_text(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  if (dc->text)
  {
    if (!dc->dev)
      abort();
    fz_fill_text(ctx, dc->dev, dc->text, fz_identity, fz_device_rgb(ctx),
        st->gs.colors.fill, 1.0, color_params);
    fz_drop_text(ctx, dc->text);
    dc->text = NULL;
  }
}

static fz_text *get_text(fz_context *ctx, dvi_context *dc)
{
  if (!dc->text)
    dc->text = fz_new_text(ctx);
  return dc->text;
}

// Extend the link being collected with a glyph drawn at trm
static void link_glyph(fz_context *ctx, dvi_context *dc, fz_font *font, int gid, fz_matrix trm)
{
  if (!dc->links || !dc->links->active)
    return;
  float adv = fz_advance_glyph(ctx, font, gid, 0);
  fz_rect box = fz_transform_rect(fz_make_rect(0, -0.25f, adv, 0.8f), trm);
  dvi_links_add_glyph(ctx, dc->links, box);
}

// Characters of glyph names of TeX fonts that the Adobe list lacks: the
// variant letters of cmmi ("epsilon1") and the sized variants of the
// extension fonts ("summationdisplay", "parenleftBig", "radicalbigg").
// Extensible pieces ("bracelefttp", "bracehtipdownleft") are left out.
static int tex_glyph_unicode(const char *name)
{
  static const struct { const char *prefix; int unicode; } table[] = {
    {"summation", 0x2211}, {"product", 0x220F}, {"coproduct", 0x2210},
    {"integral", 0x222B}, {"contintegral", 0x222E},
    {"union", 0x22C3}, {"intersection", 0x22C2},
    {"unionsq", 0x2A06}, {"unionmulti", 0x2A04},
    {"logicaland", 0x22C0}, {"logicalor", 0x22C1},
    {"circledot", 0x2A00}, {"circleplus", 0x2A01}, {"circlemultiply", 0x2A02},
    {"radical", 0x221A},
    {"angbracketleft", 0x27E8}, {"angbracketright", 0x27E9},
    {"floorleft", 0x230A}, {"floorright", 0x230B},
    {"ceilingleft", 0x2308}, {"ceilingright", 0x2309},
  };
  static const struct { const char *prefix; int unicode; } delims[] = {
    {"parenleft", '('}, {"parenright", ')'},
    {"bracketleft", '['}, {"bracketright", ']'},
    {"braceleft", '{'}, {"braceright", '}'},
    {"slash", '/'}, {"backslash", '\\'},
  };
  static const char *sizes[] = {"", "big", "Big", "bigg", "Bigg", "text", "display"};
  // Variant letters of cmmi
  static const struct { const char *name; int unicode; } letters[] = {
    {"epsilon1", 0x3B5}, {"theta1", 0x3D1}, {"pi1", 0x3D6}, {"rho1", 0x3F1},
    {"sigma1", 0x3C2}, {"phi1", 0x3C6}, {"ell", 0x2113},
  };

  for (size_t i = 0; i < sizeof(letters) / sizeof(letters[0]); ++i)
    if (strcmp(name, letters[i].name) == 0)
      return letters[i].unicode;

  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
  {
    size_t n = strlen(table[i].prefix);
    if (strncmp(name, table[i].prefix, n) == 0)
      for (size_t j = 0; j < sizeof(sizes) / sizeof(sizes[0]); ++j)
        if (strcmp(name + n, sizes[j]) == 0)
          return table[i].unicode;
  }
  for (size_t i = 0; i < sizeof(delims) / sizeof(delims[0]); ++i)
  {
    size_t n = strlen(delims[i].prefix);
    if (strncmp(name, delims[i].prefix, n) == 0)
      for (size_t j = 1; j < 5; ++j)
        if (strcmp(name + n, sizes[j]) == 0)
          return delims[i].unicode;
  }
  return 0;
}

static dvi_fontdef *dvi_current_font(fz_context *ctx, dvi_state *st)
{
  return dvi_fonttable_get(ctx, st->fonts, st->f);
}

// Record the source position of character c of a TeX font, at the current
// position. Its box spans at least the height of a line of text (from
// -0.25 to 0.75 em), for pointing at it.
static void glyph_src(fz_context *ctx, dvi_context *dc, dvi_state *st,
                      dvi_font *font, uint32_t c, fixed_t scale_factor)
{
  float em = scale_factor.value;
  float w = 0, h = 0.75f * em, d = 0.25f * em;
  if (font && font->tfm && c <= 255)
  {
    w = fixed_mul(tex_tfm_char_width(font->tfm, c), scale_factor).value;
    h = fz_max(h, fixed_mul(tex_tfm_char_height(font->tfm, c), scale_factor).value);
    d = fz_max(d, fixed_mul(tex_tfm_char_depth(font->tfm, c), scale_factor).value);
  }
  float s = dc->scale;
  fz_matrix m = dvi_get_ctm(dc, st);
  dvi_srcmap_add(ctx, dc->srcmap,
                 fz_transform_point_xy(0, 0, m),
                 fz_transform_point_xy(w * s, 0, m),
                 fz_transform_rect(fz_make_rect(0, -d * s, w * s, h * s), m));
}

void dvi_exec_char(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t c, bool set)
{
  int debug = 0;

  // fprintf(stderr, "%s_char: %C = %u\n", set ? "set" : "put", c, c);

  dvi_fontdef *def = dvi_current_font(ctx, st);
  if (def->kind != TEX_FONT)
  {
    fprintf(stderr, "dvi_exec_char: expecting TeX font\n");
    abort();
  }

  dvi_font *font = def->tex_font.font;
  fixed_t scale_factor = def->tex_font.spec.scale_factor;
  // Glyphs of the page, not the characters of virtual fonts
  if (dc->srcmap && st == &dc->root)
    glyph_src(ctx, dc, st, font, c, scale_factor);
  if (def && font)
  {
    if (!font->fz && !font->vf)
    {
      fprintf(stderr, "No fz nor vf font for %s\n", font->name);
    }
    if (font->fz)
    {
      // Glyph and character for the code c. The character comes from the
      // glyph name (the font encoding's, else the font's own): TeX fonts
      // put ligatures, quotes and dashes at arbitrary codes.
      int u = -1, uni = c;
      if (c >= 0 && c <= 255)
      {
        // Glyphs at [0, 256), characters at [256, 512)
        if (font->glyph_map)
        {
          u = font->glyph_map[c];
          uni = font->glyph_map[256 + c];
        }
        else
        {
          int *buf = fz_malloc_array(ctx, 512, int);
          if (!buf) abort();
          for (int i = 0; i < 512; ++i) buf[i] = -1;
          font->glyph_map = buf;
        }

        if (u == -1)
        {
          const char *name = NULL;
          char buf[64];
          if (font->enc)
            name = tex_enc_get(font->enc, c);
          if (name)
            u = fz_encode_character_by_glyph_name(ctx, font->fz, (const char *)name);
          else
          {
            u = fz_encode_character(ctx, font->fz, c);
            buf[0] = 0;
            if (u > 0)
              fz_get_glyph_name(ctx, font->fz, u, buf, sizeof(buf));
            name = buf;
          }
          // An unknown name is a symbol, not the character at code c
          // (cmex has the summation sign at 'X').
          if (!name[0])
            uni = c;
          else
          {
            uni = fz_unicode_from_glyph_name(name);
            if (uni <= 0 || uni == 0xFFFD)
              uni = tex_glyph_unicode(name);
            if (uni <= 0)
              uni = 0xFFFD;
          }
          font->glyph_map[c] = u;
          font->glyph_map[256 + c] = uni;
        }
      }
      else
      {
        fprintf(stderr, "character out of bounds\n");
        u = fz_encode_character(ctx, font->fz, c);
      }

      float s = dc->scale * scale_factor.value;
      fz_matrix trm = fz_pre_scale(dvi_get_ctm(dc, st), s, s);
      if (dc->dev)
        fz_show_glyph(ctx, get_text(ctx, dc), font->fz, trm, u, uni, 0, 0,
                      FZ_BIDI_LTR, FZ_LANG_UNSET);
      link_glyph(ctx, dc, font->fz, u, trm);
    }
    else if (font->vf)
    {
      dvi_state vfst;
      tex_vf_char *vfc = tex_vf_get(font->vf, c);
      if (vfc &&
          dvi_state_enter_vf(dc, &vfst, st, tex_vf_fonttable(font->vf), tex_vf_default_font(font->vf), scale_factor))
      {
        int pos = 0, dvi_length = vfc->dvi_length;
        const uint8_t *dvi = vfc->dvi;
        while (pos < dvi_length)
        {
          int size = dvi_instr_size(dvi + pos, dvi_length - pos, DVI_VF);
          if (size <= 0 || size > dvi_length - pos) break;
          //fprintf(stderr, "VF: %s (%d) at offset %d/%d\n", dvi_opname(dvi[pos]), size, pos, dvi_length);
          if (!dvi_interp_sub(ctx, dc, &vfst, dvi + pos))
          {
            fprintf(stderr, "VF: failed\n");
            break;
          }
          pos += size;
        }
      }
      else
        fprintf(stderr, "VirtualFont: cannot enter state (vfc: %p)\n", vfc);
      if (vfc && set)
      {
        st->registers.h += fixed_mul(vfc->width, scale_factor).value;
        return;
      }
    }
    if (set && font->tfm)
    {
      tex_tfm *tfm = font->tfm;
      fixed_t w = fixed_mul(tex_tfm_char_width(tfm, c), scale_factor);
      if (debug)
      {
        float s = dc->scale * scale_factor.value;
        fixed_t h = tex_tfm_char_height(tfm, c);
        fixed_t d = tex_tfm_char_depth(tfm, c);
        if (debug)
        {
          h = fixed_mul(h, scale_factor);
          d = fixed_mul(d, scale_factor);
          fprintf(stderr, "setchar%u h:=%d+%d=%d\n", c, st->registers.h,
                  w.value, st->registers.h + w.value);
          fprintf(stderr, "  char: w:%dr, h:%dr, d:%dr\n", w.value, h.value,
                  d.value);
          fprintf(stderr, "  box: (%dr, %dr, %dr, %dr)\n", st->registers.h,
                  st->registers.v - h.value, st->registers.h + w.value,
                  st->registers.v + d.value);

          output_debug_rect(
              ctx, dc, st, st->registers.h, st->registers.v - h.value,
              st->registers.h + w.value, st->registers.v + d.value);
        }
      }
      if (set)
        st->registers.h += w.value;
    }
  }
}

bool dvi_exec_push(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  dvi_context_flush_text(ctx, dc, st);
  if (st->registers_stack.depth >= st->registers_stack.limit)
    return 0;
  st->registers_stack.base[st->registers_stack.depth] = st->registers;
  st->registers_stack.depth += 1;
  return 1;
}

bool dvi_exec_pop(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  dvi_context_flush_text(ctx, dc, st);
  if (st->registers_stack.depth == 0)
    return 0;
  st->registers_stack.depth -= 1;
  st->registers = st->registers_stack.base[st->registers_stack.depth];
  return 1;
}

void dvi_exec_fnt_num(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t f)
{
  (void)dc;
  if (!dvi_current_font(ctx, st))
    fprintf(stderr, "fnt_num: undefined font %u\n", f);
  st->f = f;
}

void dvi_exec_rule(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t w, uint32_t h)
{
  int32_t x = st->registers.h - st->gs.h;
  int32_t y = st->registers.v - st->gs.v;

  //fprintf(stderr, "rule: (%fpt, %fpt, %fpt, %fpt)\n", fx, fy, fw, fh);
  output_fill_rect(ctx, dc, st, x, y, x + w, y - h);
}

bool dvi_exec_fnt_def(fz_context *ctx, dvi_context *dc, dvi_state *st,
                 uint32_t f, uint32_t c, uint32_t s, uint32_t d,
                 const char *path, size_t pathlen, const char *name, size_t namelen)
{
  // fprintf(stderr, "fnt_def:\n");
  // fprintf(stderr, "  f: %u\n", f);
  // fprintf(stderr, "  c: %u\n", c);
  // fprintf(stderr, "  s: %f (%u)\n", fixed_double(fixed_make(s)), s);
  // fprintf(stderr, "  d: %f (%u)\n", fixed_double(fixed_make(d)), d);
  // fprintf(stderr, "  path: ");
  // fwrite(path, 1, pathlen, stderr);
  // fprintf(stderr, "\n");
  // fprintf(stderr, "  name: ");
  // fwrite(name, 1, namelen, stderr);
  // fprintf(stderr, "\n");
  dvi_fontdef *def = dvi_fonttable_get(ctx, st->fonts, f);
  if (def)
  {
    def->kind = TEX_FONT;
    def->tex_font.font = dvi_resmanager_get_tex_font(ctx, dc->resmanager, name, namelen);
    def->tex_font.spec.checksum = c;
    def->tex_font.spec.scale_factor = fixed_make(s);
    def->tex_font.spec.design_size = fixed_make(d);
  }

  return 1;
}

bool dvi_exec_bop(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t c[10], uint32_t p)
{
  // fprintf(stderr, "beginning_of_page\n");
  memset(&st->registers, 0, sizeof(dvi_registers));
  (void)dc;
  if (st->gs_stack.depth != 0)
  {
    fprintf(stderr, "beginning_of_page: transformation stack was not at empty\n");
    st->gs_stack.depth = 0;
  }
  if (st->registers_stack.depth != 0)
  {
    fprintf(stderr, "beginning_of_page: stack was not at empty\n");
    st->registers_stack.depth = 0;
  }
  (void)c;
  (void)p;
  return 1;
}

void dvi_exec_eop(fz_context *ctx, dvi_context *dc, dvi_state *st)
{
  dvi_context_flush_text(ctx, dc, st);
  // fprintf(stderr, "end_of_page\n");
}

bool dvi_exec_pre(fz_context *ctx, dvi_context *dc, dvi_state *st, uint8_t i, uint32_t num, uint32_t den, uint32_t mag, const char *comment, size_t len)
{
  (void)dc;
  fprintf(stderr, "pre:\n");
  fprintf(stderr, "  i: %u\n", i);
  fprintf(stderr, "  num: %u\n", num);
  fprintf(stderr, "  den: %u\n", den);
  fprintf(stderr, "  mag: %u\n", mag);
  fprintf(stderr, "  comment: %.*s\n", (int)len, comment);

  dc->scale = num/254000.0*72.27/den*mag/1000.0 * 800/803;

  return 1;
}

void dvi_exec_xdvfontdef(fz_context *ctx, dvi_context *dc, dvi_state *st, uint32_t fontnum,
    const char *name, int name_len, int index, dvi_xdvfontspec spec)
{
  dvi_fontdef *def = dvi_fonttable_get(ctx, st->fonts, fontnum);
  if (def)
  {
    def->kind = XDV_FONT;
    def->xdv_font.font = dvi_resmanager_get_xdv_font(ctx, dc->resmanager, name, name_len, index,
                                                     &def->xdv_font.unicode,
                                                     &def->xdv_font.glyph_count);
    def->xdv_font.spec = spec;
  }
}

void dvi_exec_xdvglyphs(fz_context *ctx, dvi_context *dc, dvi_state *st, fixed_t width,
                int char_count, uint16_t *chars,
                int num_glyphs, fixed_t *dx, fixed_t dy0, fixed_t *dy, uint16_t *glyphs)
{
  //fprintf(stderr, "dvi_exec_xdvglyphs: width:%d, chars:%d, glyphs:%d\n", width.value, char_count, num_glyphs);
  dvi_fontdef *def = dvi_current_font(ctx, st);
  if (def->kind != XDV_FONT)
  {
    fprintf(stderr, "dvi_exec_xdvglyphs: expecting XDV font\n");
    abort();
  }
  fz_font *font = def->xdv_font.font;
  fixed_t size = def->xdv_font.spec.size;
  bool linking = dc->links && dc->links->active;
  if (!dc->dev && !linking);
  else if (font)
  {
    float ds = dc->scale;
    float fs = size.value * ds;

    int32_t sh = st->registers.h - st->gs.h;
    int32_t sv = st->registers.v + dy0.value - st->gs.v;
    fz_text *text = dc->dev ? get_text(ctx, dc) : NULL;
    for (int i = 0; i < num_glyphs; ++i)
    {
      int32_t h = sh + dx[i].value;
      int32_t v = dy ? sv + dy[i].value : sv;
      fz_matrix ctm =
          fz_pre_scale(fz_pre_translate(st->gs.ctm, h * ds, -v * ds), fs, fs);
      int uni = glyphs[i] < def->xdv_font.glyph_count
                    ? def->xdv_font.unicode[glyphs[i]] : 0;
      if (text)
        fz_show_glyph(ctx, text, font, ctm, glyphs[i], uni, 0, 0, FZ_BIDI_LTR,
                      FZ_LANG_UNSET);
      if (linking)
        link_glyph(ctx, dc, font, glyphs[i], ctm);
    }
  }
  else
    fprintf(stderr, "dvi_exec_xdvglyphs: font not found\n");
  st->registers.h += width.value;
}
