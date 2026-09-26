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

#include <SDL2/SDL.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include "mydvi.h"
#include "providers.h"
#include "renderer.h"
#include "engine.h"
#include "driver.h"
#include "synctex.h"
#include "vstack.h"
#include "prot_parser.h"
#include "editor.h"
#include "base64.h"
#include "scroll.h"

struct persistent_state *pstate;

static void schedule_event(enum custom_events ev)
{
  pstate->schedule_event(ev);
}

static bool should_reload_binary(void)
{
  return pstate->should_reload_binary();
}

static void scroll_wakeup(void)
{
  schedule_event(SCROLL_EVENT);
}

#ifdef __APPLE__
# define st_time(a) st_##a##timespec
#else
# define st_time(a) st_##a##tim
#endif

static bool is_more_recent(uint64_t *time, char *candidate)
{
  struct stat st;
  if (stat(candidate, &st) == 0 && st.st_time(c).tv_sec > *time)
  {
    *time = st.st_time(c).tv_sec;
    return 1;
  }
  return 0;
}

static void set_more_recent(uint64_t *time, char **result, char *candidate)
{
  if (is_more_recent(time, candidate))
    *result = candidate;
}

static void find_engine(char engine_path[4096], const char *exec_path)
{
  strcpy(engine_path, exec_path);
  char *basename = NULL;
  for (int i = 0; i < 4096 && engine_path[i]; ++i)
    if (engine_path[i] == '/')
      basename = engine_path + i + 1;
  uint64_t time = 0;
  if (basename)
  {
    strcpy(basename, "texpresso-xetex");
    if (!is_more_recent(&time, engine_path))
      strcpy(engine_path, "texpresso-xetex");
  }
}

/* UI state */

enum ui_mouse_status {
  UI_MOUSE_NONE,
  UI_MOUSE_SELECT,
  UI_MOUSE_MOVE,
};

typedef struct {
  txp_engine *eng;
  txp_renderer *doc_renderer;
  SDL_Renderer *sdl_renderer;
  SDL_Window *window;

  int page;
  int need_synctex;
  int zoom;

  // Mouse input state
  int last_mouse_x, last_mouse_y;
  uint32_t last_click_ticks;
  enum ui_mouse_status mouse_status;
  bool advancing;

  // Forward sync marker: briefly highlights the position that the editor
  // cursor maps to. Coordinates are in document space (points).
  struct {
    bool active;
    int page;
    fz_point pt;
    fz_rect box; // typeset line holding pt, empty if unknown
    bool no_caret; // pt is unreliable, only highlight box
    uint32_t ticks;
  } sync_mark;

  // Scrolling past the ends of the page (see ui_wheel_pan_y)
  struct {
    float tension;       // scrolled past the top (> 0) or the bottom (< 0), in pixels
    float velocity;      // of the spring, in pixels per millisecond
    uint32_t last_wheel; // time of the last wheel event without phases
    double last_scroll;  // time of the last event with phases (txp_scroll_event)
    bool touching;       // the fingers are on the trackpad
    bool scrolled;       // the current wheel gesture moved the page
    bool flipped;        // the current gesture turned the page
    bool bounced;        // the momentum reached the end of the page
  } overscroll;

  uint32_t last_frame; // time of the last rendering

  // Last forward sync request (the path relative to the document).
  struct {
    char path[1024];
    int line, column;
  } sync_target;

  // Links of the displayed page
  fz_link *links;
  int links_page;
  bool over_link;

  // Positions left by following links, to go back to
  struct {
    int page;
    fz_point pan;
  } history[32];
  int history_len;
} ui_state;

// The marker is shown at full strength for SYNC_MARK_HOLD_MS, then fades
// out over SYNC_MARK_FADE_MS.
#define SYNC_MARK_HOLD_MS 700
#define SYNC_MARK_FADE_MS 900
#define SYNC_MARK_FRAME_MS 25

static bool sync_mark_active(ui_state *ui)
{
  if (!ui->sync_mark.active)
    return false;
  if (SDL_GetTicks() - ui->sync_mark.ticks > SYNC_MARK_HOLD_MS + SYNC_MARK_FADE_MS)
    ui->sync_mark.active = false;
  return ui->sync_mark.active;
}

// Scrolling past the top or the bottom of the page stretches a spring, and
// stretching it OVERSCROLL_FLIP points turns to the previous or the next
// page, which comes in from the side of the old one. The spring holds while
// the fingers are on the trackpad, and goes back once they leave. The
// momentum of the scroll does not stretch it: the page bounces at the end,
// and the rest of the momentum is dropped.
// Mouse wheels (and the SDL events on other systems) have no phases: a
// gesture ends WHEEL_GESTURE_GAP_MS after its last event, only one that starts
// at the end of the page stretches the spring, and OVERSCROLL_FLIP_WHEEL
// points turn the page.
#define OVERSCROLL_FLIP 360
#define OVERSCROLL_FLIP_WHEEL 120
#define OVERSCROLL_STRETCH 80 // points the page can move at most
#define OVERSCROLL_SPRING_MS 45 // time constant of the (critically damped) spring
#define OVERSCROLL_FRAME_MS 16
#define WHEEL_GESTURE_GAP_MS 150

// Milliseconds between the frames of the running animations, 0 if none
static uint32_t animation_frame_ms(ui_state *ui)
{
  if (ui->overscroll.tension != 0 || ui->overscroll.velocity != 0)
    return OVERSCROLL_FRAME_MS;
  if (sync_mark_active(ui))
    return SYNC_MARK_FRAME_MS;
  return 0;
}

/* UI rendering */

static float zoom_factor(int count)
{
  return expf((float)count / 5000.0f);
}

static fz_point get_scale_factor(SDL_Window *window);

static void render_sync_mark(fz_context *ctx, ui_state *ui)
{
  if (!sync_mark_active(ui) || ui->sync_mark.page != ui->page)
    return;

  uint32_t elapsed = ui->last_frame - ui->sync_mark.ticks;
  float strength = 1.0;
  if (elapsed > SYNC_MARK_HOLD_MS)
    strength = 1.0 - (float)(elapsed - SYNC_MARK_HOLD_MS) / SYNC_MARK_FADE_MS;
  if (strength <= 0)
    return;

  fz_point scale = get_scale_factor(ui->window);
  fz_point pt = txp_renderer_document_to_screen(ctx, ui->doc_renderer, ui->sync_mark.pt);
  fz_rect box = ui->sync_mark.box;
  bool has_box = !fz_is_empty_rect(box);
  fz_point b0, b1;
  if (has_box)
  {
    b0 = txp_renderer_document_to_screen(ctx, ui->doc_renderer, fz_make_point(box.x0, box.y0));
    b1 = txp_renderer_document_to_screen(ctx, ui->doc_renderer, fz_make_point(box.x1, box.y1));
  }
  else
  {
    // No enclosing line box known: draw a caret of a typical line height.
    b0 = txp_renderer_document_to_screen(ctx, ui->doc_renderer,
                                         fz_make_point(ui->sync_mark.pt.x, ui->sync_mark.pt.y - 8));
    b1 = txp_renderer_document_to_screen(ctx, ui->doc_renderer,
                                         fz_make_point(ui->sync_mark.pt.x, ui->sync_mark.pt.y + 3));
  }

  SDL_SetRenderDrawBlendMode(ui->sdl_renderer, SDL_BLENDMODE_BLEND);

  if (has_box)
  {
    // Soft band over the whole typeset line.
    SDL_FRect band = {b0.x, b0.y - 1 * scale.y, b1.x - b0.x, b1.y - b0.y + 2 * scale.y};
    SDL_SetRenderDrawColor(ui->sdl_renderer, 255, 170, 0, (Uint8)(48 * strength));
    SDL_RenderFillRectF(ui->sdl_renderer, &band);
  }

  if (ui->sync_mark.no_caret)
    return;

  // Caret at the position itself, with a small halo so it stands out on
  // both light and dark backgrounds.
  float w = 3 * scale.x;
  float pad = 2 * scale.y;
  SDL_FRect halo = {pt.x - w, b0.y - 2 * pad, 3 * w, b1.y - b0.y + 4 * pad};
  SDL_SetRenderDrawColor(ui->sdl_renderer, 255, 120, 0, (Uint8)(70 * strength));
  SDL_RenderFillRectF(ui->sdl_renderer, &halo);
  SDL_FRect caret = {pt.x - w / 2, b0.y - pad, w, b1.y - b0.y + 2 * pad};
  SDL_SetRenderDrawColor(ui->sdl_renderer, 255, 80, 0, (Uint8)(230 * strength));
  SDL_RenderFillRectF(ui->sdl_renderer, &caret);
}

/* SyncTeX refinement by text.

   SyncTeX locates material only coarsely: records mark where a run of
   characters starts, a caption or the body of an align environment is read
   as a macro argument and typeset at its last line, the first word of a
   typeset line only belongs to the box of the line, which carries the line
   where the paragraph ended, and the records of a TikZ picture are not where
   its content is drawn. Both directions therefore match the text of the
   source against the text of the page around the position SyncTeX found. */

#define SYNC_TEXT_MAX 4096

struct sync_word {
  int start, end;          // characters of the line, [start, end)
  int first, count;        // folded characters, in sync_text.chars
  // Text typeset away from the running text (footnotes, floats, the nodes
  // of a TikZ picture) is a segment of its own, 0 is the running text.
  int segment;
};

#define SYNC_SEGMENT_DEPTH 16

enum sync_segment_kind {
  SYNC_SEGMENT_ARGUMENT,   // \footnote{...}: until the group closes
  SYNC_SEGMENT_FLOAT,      // \begin{figure}: until \end of a float
  SYNC_SEGMENT_NODE,       // node {...} in a TikZ picture
};

// Delimiters of math, see sync_math_line
enum sync_math_kind {
  SYNC_MATH_NONE,
  SYNC_MATH_DOLLAR,     // $...$
  SYNC_MATH_DOLLARS,    // $$...$$
  SYNC_MATH_PAREN,      // \(...\)
  SYNC_MATH_BRACKET,    // \[...\]
  SYNC_MATH_ENV,        // \begin{align}...\end{align}
};

struct sync_macros;

struct sync_text {
  int chars[SYNC_TEXT_MAX];     // folded characters of all words
  // The characters of the line each one stands for, [source, source_end):
  // itself, or the use of the macro that typeset it
  int source[SYNC_TEXT_MAX], source_end[SYNC_TEXT_MAX];
  // Whether material left out (math, macros and their arguments) comes
  // right before the character: the page can have more text there, up to
  // this many characters (0 if none).
  unsigned char gap[SYNC_TEXT_MAX];
  int nchars;
  struct sync_word words[SYNC_TEXT_MAX];
  int nwords;
  // Across lines: brace depth, the open segments with the depth of their
  // group, TikZ pictures (only the text of their nodes is typeset as is)
  // and whether a node is waiting for its text.
  int depth, nsegments, open, tikz;
  bool node;
  int segment[SYNC_SEGMENT_DEPTH], segment_depth[SYNC_SEGMENT_DEPTH];
  enum sync_segment_kind segment_kind[SYNC_SEGMENT_DEPTH];
  // The verbatim environment the lines are in, if any, and whether its
  // text is typeset at all (not for comment)
  char verbatim[32];
  bool hidden;
  // The math the lines are in, if any, with the name of its environment
  enum sync_math_kind math;
  char math_env[32];
  // Math left out at the end of the last line (the number of an equation
  // or of a row): the gap before the next character
  int pending_gap;
  // Macros of the document, expanded in math (not reset)
  const struct sync_macros *macros;
};

static void sync_text_reset(struct sync_text *t)
{
  t->nchars = t->nwords = 0;
  t->depth = t->nsegments = t->open = t->tikz = 0;
  t->node = false;
  t->verbatim[0] = 0;
  t->math = SYNC_MATH_NONE;
  t->math_env[0] = 0;
  t->pending_gap = 0;
}

// Append the folded character f, standing for [start, end) of the line, to
// the word being read (if *in_word) or to a new one.
static void sync_add_char(struct sync_text *t, bool *in_word, int start,
                          int end, int f, int gap)
{
  if (t->nchars == SYNC_TEXT_MAX || (!*in_word && t->nwords == SYNC_TEXT_MAX))
    return;
  if (!*in_word)
  {
    struct sync_word *w = &t->words[t->nwords++];
    w->start = start;
    w->end = end;
    w->first = t->nchars;
    w->count = 0;
    w->segment = t->nsegments ? t->segment[t->nsegments - 1] : 0;
    *in_word = true;
  }
  struct sync_word *w = &t->words[t->nwords - 1];
  t->chars[t->nchars] = f;
  t->source[t->nchars] = start;
  t->source_end[t->nchars] = end;
  t->gap[t->nchars] = gap;
  t->nchars++;
  w->count++;
  if (start < w->start)
    w->start = start;
  if (end > w->end)
    w->end = end;
}

// Whether line[i] starts the string s.
static bool sync_looking_at(const int *line, int n, int i, const char *s)
{
  for (; *s; s++, i++)
    if (i >= n || line[i] != *s)
      return false;
  return true;
}

// Environments whose text is typeset as it is.
static bool sync_is_verbatim(const char *env)
{
  static const char *names[] = {
    "verbatim", "verbatim*", "Verbatim", "Verbatim*", "BVerbatim", "LVerbatim",
    "lstlisting", "minted", "comment", NULL
  };
  for (const char **n = names; *n; n++)
    if (strcmp(*n, env) == 0)
      return true;
  return false;
}

// Verbatim text from line[i] up to `end` (which closes it), returning the
// index after `end`, or n if the text continues on the next line.
static int sync_verbatim(const int *line, int n, int i, const char *end,
                         bool hidden, struct sync_text *t, bool *closed)
{
  bool in_word = false;
  *closed = false;
  for (; i < n; i++)
  {
    if (sync_looking_at(line, n, i, end))
    {
      *closed = true;
      return i + strlen(end);
    }
    int f = hidden ? 0 : txp_fold_char(line[i]);
    if (f)
      sync_add_char(t, &in_word, i, i + 1, f, 0);
    else
      in_word = false;
  }
  return n;
}

// Verbatim environment lines up to \end{t->verbatim}, leaving it there.
static int sync_verbatim_environment(const int *line, int n, int i,
                                     struct sync_text *t)
{
  char end[48];
  snprintf(end, sizeof(end), "\\end{%s}", t->verbatim);
  bool closed;
  i = sync_verbatim(line, n, i, end, t->hidden, t, &closed);
  if (closed)
    t->verbatim[0] = 0;
  return i;
}

static void sync_segment_open(struct sync_text *t, enum sync_segment_kind kind)
{
  if (t->nsegments == SYNC_SEGMENT_DEPTH)
    return;
  t->segment[t->nsegments] = ++t->open;
  t->segment_depth[t->nsegments] = t->depth + 1;
  t->segment_kind[t->nsegments] = kind;
  t->nsegments++;
}

static bool sync_in_node(struct sync_text *t)
{
  return t->nsegments > 0 &&
         t->segment_kind[t->nsegments - 1] == SYNC_SEGMENT_NODE;
}

// The name of the environment in the group at line[i], as in {figure}.
static void sync_environment(const int *line, int n, int i, char *name, int size)
{
  int k = 0;
  if (i < n && line[i] == '{')
    for (i++; i < n && line[i] != '}' && k < size - 1; i++)
      name[k++] = line[i];
  name[k] = 0;
}

static bool sync_is_float(const char *env)
{
  static const char *names[] = {
    "figure", "figure*", "table", "table*", "algorithm", "algorithm*",
    "wrapfigure", "wraptable", "marginfigure", "margintable", NULL
  };
  for (const char **n = names; *n; n++)
    if (strcmp(*n, env) == 0)
      return true;
  return false;
}

// Environments whose arguments are not text: \begin{tabular}{p{1in}l}.
// Those of other environments are often titles.
static bool sync_env_skips_arguments(const char *env)
{
  static const char *names[] = {
    "tabular", "tabular*", "tabularx", "tabulary", "array", "longtable",
    "minipage", "wrapfigure", "wraptable", "multicols", "multicols*",
    "minted", "thebibliography", "subfigure", "list", "tikzpicture",
    "alignat", "alignat*", NULL
  };
  for (const char **n = names; *n; n++)
    if (strcmp(*n, env) == 0)
      return true;
  return false;
}

// Macros whose argument is typeset away from the running text.
static bool sync_macro_moves_argument(const char *name)
{
  return strcmp(name, "footnote") == 0 || strcmp(name, "footnotetext") == 0 ||
         strcmp(name, "thanks") == 0 || strcmp(name, "marginpar") == 0;
}

// Headings: before their text, the page has at most a number (A.1.2).
#define SYNC_NUMBER_GAP 8
static bool sync_macro_is_heading(const char *name)
{
  static const char *names[] = {
    "part", "chapter", "section", "subsection", "subsubsection", "paragraph",
    "subparagraph", NULL
  };
  for (const char **p = names; *p; p++)
    if (strcmp(name, *p) == 0)
      return true;
  return false;
}

static bool sync_macro_skips_argument(const char *name)
{
  static const char *names[] = {
    "label", "ref", "autoref", "cref", "Cref", "eqref", "pageref", "nameref",
    "cite", "citep", "citet", "citealp", "citeauthor", "citeyear", "nocite",
    "includegraphics", "begin", "end", "input", "include", "usepackage",
    "documentclass", "bibliographystyle", "bibliography", "textcolor",
    "color", "definecolor", "colorlet", "pgfmathsetmacro", "newcommand",
    "renewcommand", "hypersetup", "href", "tikzset", "vspace", "hspace",
    "setlength", "addtolength", "def", NULL
  };
  for (const char **n = names; *n; n++)
    if (strcmp(*n, name) == 0)
      return true;
  return false;
}

// Skip a balanced group starting at line[i] (which is `open`), returning the
// index after it.
static int sync_skip_group(const int *line, int n, int i, int open, int close)
{
  int depth = 0;
  for (; i < n; i++)
  {
    if (line[i] == '\\')
      i++;
    else if (line[i] == open)
      depth++;
    else if (line[i] == close && --depth == 0)
      return i + 1;
  }
  return n;
}

// The contents of a file of the document, as the editor last sent it.
static fz_buffer *sync_file_data(struct persistent_state *ps, ui_state *ui,
                                 const char *path)
{
  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  return e ? (e->edit_data ? e->edit_data : e->fs_data) : NULL;
}

/* Math. A formula is read as TeX typesets it, to get its letters and digits
   in the order of the page: the nucleus of an atom, then its superscript,
   then its subscript (TeX outputs the superscript first; that of an
   operator with limits comes before the operator), the numerator of a
   fraction before its denominator. The macros of the document are expanded
   (\newcommand{\Din}{{D_\text{in}}}): the characters of their text stand
   for the macro use. Symbols split words; macros that are not known leave a
   gap, which can start anywhere (math has no spaces). */

#define SYNC_MATH_GAP 8
#define SYNC_NEEDLE_MIN 8
#define SYNC_MAX_EXPANSIONS 64

// A character of math: its code point and the characters of the line it
// stands for, [col, end)
struct sync_tok {
  int c, col, end;
};

// A macro of the document: \newcommand{\name}[nargs][default]{body}
struct sync_macro {
  char name[32];
  int nargs;
  int *def, ndef;  // default of the first argument, NULL if it is mandatory
  int *body, len;
};

struct sync_macros {
  struct sync_macro *items;
  int count, cap;
};

// The state of the words while reading math
struct sync_math {
  struct sync_text *t;
  bool *in_word;
  int *gap;
  bool display;    // display style: operators take limits
  int expansions;  // macros expanded (a recursive definition stops)
};

static bool sync_is_letter(int c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '@';
}

// Add g to the gap before the next character: the largest size, and
// anywhere if either is.
static void sync_gap_merge(int *gap, int g)
{
  int a = *gap & ~TXP_TEXT_GAP_ANYWHERE, b = g & ~TXP_TEXT_GAP_ANYWHERE;
  *gap = (a > b ? a : b) | ((*gap | g) & TXP_TEXT_GAP_ANYWHERE);
}

static void sync_math_emit(struct sync_math *m, int c, int col, int end)
{
  int f = txp_fold_char(c);
  if (!f)
  {
    *m->in_word = false;
    return;
  }
  sync_add_char(m->t, m->in_word, col, end, f, *m->gap);
  *m->gap = 0;
}

static void sync_math_split(struct sync_math *m)
{
  *m->in_word = false;
}

static const struct sync_macro *sync_macros_find(const struct sync_macros *ms,
                                                 const char *name)
{
  // The last definition wins (\renewcommand)
  for (int i = ms ? ms->count - 1 : -1; i >= 0; i--)
    if (strcmp(ms->items[i].name, name) == 0)
      return &ms->items[i];
  return NULL;
}

static int sync_tok_spaces(const struct sync_tok *s, int n, int i)
{
  while (i < n && (s[i].c == ' ' || s[i].c == '\t'))
    i++;
  return i;
}

// The name of the control sequence at s[i] (a backslash), returning the
// index after it.
static int sync_tok_name(const struct sync_tok *s, int n, int i, char *name,
                         int size)
{
  int j = i + 1, k = 0;
  if (j < n && !sync_is_letter(s[j].c))
  {
    name[k++] = s[j].c < 128 ? s[j].c : '?';
    name[k] = 0;
    return j + 1;
  }
  for (; j < n && sync_is_letter(s[j].c); j++)
    if (k < size - 1)
      name[k++] = s[j].c;
  name[k] = 0;
  return j;
}

// The index of what closes the group opened at s[i], n if it does not.
static int sync_tok_group_end(const struct sync_tok *s, int n, int i,
                              int open, int close)
{
  int depth = 0;
  for (; i < n; i++)
  {
    if (s[i].c == '\\')
      i++;
    else if (s[i].c == open)
      depth++;
    else if (s[i].c == close && --depth == 0)
      return i;
  }
  return n;
}

// The argument at s[i] (after spaces): the contents of a group, or a single
// token, [*a, *b). Returns the index after it.
static int sync_tok_arg(const struct sync_tok *s, int n, int i, int *a, int *b)
{
  i = sync_tok_spaces(s, n, i);
  *a = *b = i;
  if (i >= n)
    return n;
  if (s[i].c == '{')
  {
    int e = sync_tok_group_end(s, n, i, '{', '}');
    *a = i + 1;
    *b = e;
    return e < n ? e + 1 : n;
  }
  char name[32];
  *b = s[i].c == '\\' ? sync_tok_name(s, n, i, name, sizeof(name)) : i + 1;
  return *b;
}

// The optional argument at s[i] (after spaces), [*a, *b), with the index
// after it. Without one, *a = -1 and i is returned.
static int sync_tok_optional(const struct sync_tok *s, int n, int i, int *a,
                             int *b)
{
  int j = sync_tok_spaces(s, n, i);
  *a = *b = -1;
  if (j >= n || s[j].c != '[')
    return i;
  int e = sync_tok_group_end(s, n, j, '[', ']');
  *a = j + 1;
  *b = e;
  return e < n ? e + 1 : n;
}

// The use of macro d at s[i], whose name ends at j, replaced by its text:
// the tokens of the text (standing for the use) with the arguments (as they
// are), then those after the use. NULL if too long.
static struct sync_tok *sync_expand(const struct sync_macro *d,
                                    const struct sync_tok *s, int n, int i,
                                    int j, int *len)
{
  int a[9], b[9], k = 0;
  if (d->def)
  {
    j = sync_tok_optional(s, n, j, &a[0], &b[0]);
    k = 1;
  }
  for (; k < d->nargs && k < 9; k++)
    j = sync_tok_arg(s, n, j, &a[k], &b[k]);
  int col = s[i].col, end = s[j > i ? j - 1 : i].end;

  int size = n - j;
  for (int p = 0; p < d->len; p++)
  {
    int q = p + 1 < d->len ? d->body[p + 1] - '1' : -1;
    if (d->body[p] == '#' && q >= 0 && q < d->nargs && q < 9)
    {
      size += a[q] >= 0 ? b[q] - a[q] : d->ndef;
      p++;
    }
    else
      size++;
  }
  if (size > SYNC_TEXT_MAX)
    return NULL;

  struct sync_tok *out = malloc(size * sizeof(*out));
  if (!out)
    return NULL;
  int o = 0;
  for (int p = 0; p < d->len; p++)
  {
    int q = p + 1 < d->len ? d->body[p + 1] - '1' : -1;
    if (d->body[p] == '#' && q >= 0 && q < d->nargs && q < 9)
    {
      if (a[q] >= 0)
        for (int r = a[q]; r < b[q]; r++)
          out[o++] = s[r];
      else
        for (int r = 0; r < d->ndef; r++)
          out[o++] = (struct sync_tok){d->def[r], col, end};
      p++;
    }
    else
      out[o++] = (struct sync_tok){d->body[p], col, end};
  }
  memcpy(out + o, s + j, (n - j) * sizeof(*out));
  *len = size;
  return out;
}

enum sync_math_macro_kind {
  SYNC_MM_UNKNOWN,
  SYNC_MM_SYMBOL,     // a symbol or a space: splits words
  SYNC_MM_IGNORE,     // typesets nothing (\displaystyle, \left, \limits)
  SYNC_MM_LETTER,     // a letter (\alpha, \ell)
  SYNC_MM_NAME,       // its name upright (\sin, \log), or "mod"
  SYNC_MM_STYLE,      // the math of its argument (\mathbf{x}, \hat{x})
  SYNC_MM_TEXT,       // the text of its argument (\text{if})
  SYNC_MM_SKIP,       // not its argument (\label{...}, \phantom{...})
  SYNC_MM_REFERENCE,  // text not in the source (\eqref{...}, \tag{...})
  SYNC_MM_DIMEN,      // not the dimension after it (\mkern-9mu)
  SYNC_MM_FRACTION,   // its two arguments, the first above (\frac)
  SYNC_MM_UNDERSET,   // its two arguments, the second above
  SYNC_MM_COLOR,      // the math of its second argument (\textcolor)
  SYNC_MM_ROOT,       // \sqrt[index]{math}
  SYNC_MM_ARROW,      // \xrightarrow[below]{above}
  SYNC_MM_ENV,        // \begin{cases}, \end{cases}
  SYNC_MM_MOD,        // \pmod{x}: "mod" and its argument
};

static const struct {
  const char *name;
  unsigned char kind;
  int value;
} sync_math_macros[] = {
#define L(n, v) {n, SYNC_MM_LETTER, v}
  L("alpha", 0x3B1), L("beta", 0x3B2), L("gamma", 0x3B3), L("delta", 0x3B4),
  L("epsilon", 0x3F5), L("varepsilon", 0x3B5), L("zeta", 0x3B6),
  L("eta", 0x3B7), L("theta", 0x3B8), L("vartheta", 0x3D1), L("iota", 0x3B9),
  L("kappa", 0x3BA), L("varkappa", 0x3F0), L("lambda", 0x3BB), L("mu", 0x3BC),
  L("nu", 0x3BD), L("xi", 0x3BE), L("pi", 0x3C0), L("varpi", 0x3D6),
  L("rho", 0x3C1), L("varrho", 0x3F1), L("sigma", 0x3C3),
  L("varsigma", 0x3C2), L("tau", 0x3C4), L("upsilon", 0x3C5),
  L("phi", 0x3D5), L("varphi", 0x3C6), L("chi", 0x3C7), L("psi", 0x3C8),
  L("omega", 0x3C9), L("Gamma", 0x393), L("Delta", 0x394), L("Theta", 0x398),
  L("Lambda", 0x39B), L("Xi", 0x39E), L("Pi", 0x3A0), L("Sigma", 0x3A3),
  L("Upsilon", 0x3A5), L("Phi", 0x3A6), L("Psi", 0x3A8), L("Omega", 0x3A9),
  L("varGamma", 0x393), L("varDelta", 0x394), L("varTheta", 0x398),
  L("varLambda", 0x39B), L("varXi", 0x39E), L("varPi", 0x3A0),
  L("varSigma", 0x3A3), L("varUpsilon", 0x3A5), L("varPhi", 0x3A6),
  L("varPsi", 0x3A8), L("varOmega", 0x3A9),
  L("ell", 'l'), L("imath", 'i'), L("jmath", 'j'), L("hbar", 'h'),
  L("hslash", 'h'), L("Re", 'r'), L("Im", 'i'),
#undef L
#define N(n) {n, SYNC_MM_NAME, 0}
  N("arccos"), N("arcsin"), N("arctan"), N("arg"), N("cos"), N("cosh"),
  N("cot"), N("coth"), N("csc"), N("deg"), N("det"), N("dim"), N("exp"),
  N("gcd"), N("hom"), N("inf"), N("ker"), N("lg"), N("lim"), N("liminf"),
  N("limsup"), N("ln"), N("log"), N("max"), N("min"), N("Pr"), N("sec"),
  N("sin"), N("sinh"), N("sup"), N("tan"), N("tanh"), N("injlim"),
  N("projlim"), {"bmod", SYNC_MM_NAME, 1}, {"mod", SYNC_MM_NAME, 1},
#undef N
#define S(n, k) {n, SYNC_MM_##k, 0}
  S("mathrm", STYLE), S("mathit", STYLE), S("mathbf", STYLE),
  S("mathsf", STYLE), S("mathtt", STYLE), S("mathcal", STYLE),
  S("mathbb", STYLE), S("mathfrak", STYLE), S("mathscr", STYLE),
  S("mathnormal", STYLE), S("mathbfit", STYLE), S("boldsymbol", STYLE),
  S("bm", STYLE), S("pmb", STYLE), S("mathop", STYLE), S("mathbin", STYLE),
  S("mathrel", STYLE), S("mathord", STYLE), S("mathopen", STYLE),
  S("mathclose", STYLE), S("mathpunct", STYLE), S("mathinner", STYLE),
  S("operatorname", STYLE), S("hat", STYLE), S("widehat", STYLE),
  S("tilde", STYLE), S("widetilde", STYLE), S("bar", STYLE),
  S("overline", STYLE), S("underline", STYLE), S("vec", STYLE),
  S("dot", STYLE), S("ddot", STYLE), S("dddot", STYLE), S("breve", STYLE),
  S("check", STYLE), S("acute", STYLE), S("grave", STYLE),
  S("mathring", STYLE), S("overbrace", STYLE), S("underbrace", STYLE),
  S("overrightarrow", STYLE), S("overleftarrow", STYLE),
  S("overleftrightarrow", STYLE), S("underrightarrow", STYLE),
  S("underleftarrow", STYLE), S("boxed", STYLE), S("smash", STYLE),
  S("cancel", STYLE), S("bcancel", STYLE), S("xcancel", STYLE),
  S("ensuremath", STYLE), S("substack", STYLE), S("lefteqn", STYLE),
  S("mathclap", STYLE), S("mathllap", STYLE), S("mathrlap", STYLE),
  S("clap", STYLE), S("llap", STYLE), S("rlap", STYLE), S("pod", STYLE),
  S("text", TEXT), S("textrm", TEXT), S("textit", TEXT), S("textbf", TEXT),
  S("textsf", TEXT), S("texttt", TEXT), S("textup", TEXT), S("textsl", TEXT),
  S("textsc", TEXT), S("textnormal", TEXT), S("textmd", TEXT),
  S("mbox", TEXT), S("hbox", TEXT), S("emph", TEXT), S("fbox", TEXT),
  S("intertext", TEXT), S("shortintertext", TEXT),
  S("label", SKIP), S("phantom", SKIP), S("hphantom", SKIP),
  S("vphantom", SKIP), S("hspace", SKIP), S("vspace", SKIP), S("color", SKIP),
  S("tag", REFERENCE), S("eqref", REFERENCE), S("ref", REFERENCE),
  S("autoref", REFERENCE), S("cref", REFERENCE), S("Cref", REFERENCE),
  S("pageref", REFERENCE), S("cite", REFERENCE), S("citep", REFERENCE),
  S("citet", REFERENCE),
  S("kern", DIMEN), S("mkern", DIMEN), S("hskip", DIMEN), S("mskip", DIMEN),
  S("frac", FRACTION), S("dfrac", FRACTION), S("tfrac", FRACTION),
  S("cfrac", FRACTION), S("binom", FRACTION), S("dbinom", FRACTION),
  S("tbinom", FRACTION), S("overset", FRACTION), S("stackrel", FRACTION),
  S("underset", UNDERSET), S("textcolor", COLOR), S("sqrt", ROOT),
  S("xrightarrow", ARROW), S("xleftarrow", ARROW), S("xRightarrow", ARROW),
  S("xLeftarrow", ARROW), S("xleftrightarrow", ARROW),
  S("xLeftrightarrow", ARROW), S("xmapsto", ARROW),
  S("xhookrightarrow", ARROW), S("xhookleftarrow", ARROW),
  S("begin", ENV), S("end", ENV), S("pmod", MOD),
  S("displaystyle", IGNORE), S("textstyle", IGNORE),
  S("scriptstyle", IGNORE), S("scriptscriptstyle", IGNORE),
  S("left", IGNORE), S("right", IGNORE), S("middle", IGNORE),
  S("big", IGNORE), S("Big", IGNORE), S("bigg", IGNORE), S("Bigg", IGNORE),
  S("bigl", IGNORE), S("bigr", IGNORE), S("bigm", IGNORE),
  S("Bigl", IGNORE), S("Bigr", IGNORE), S("Bigm", IGNORE),
  S("biggl", IGNORE), S("biggr", IGNORE), S("biggm", IGNORE),
  S("Biggl", IGNORE), S("Biggr", IGNORE), S("Biggm", IGNORE),
  S("limits", IGNORE), S("nolimits", IGNORE), S("displaylimits", IGNORE),
  S("nonumber", IGNORE), S("notag", IGNORE), S("mathstrut", IGNORE),
  S("strut", IGNORE), S("allowbreak", IGNORE), S("nobreak", IGNORE),
  S("relax", IGNORE), S("protect", IGNORE), S("displaybreak", IGNORE),
  S("not", IGNORE), S("hline", IGNORE), S("hfill", IGNORE),
  S("boldmath", IGNORE), S("unboldmath", IGNORE), S("rm", IGNORE),
  S("bf", IGNORE), S("it", IGNORE), S("sf", IGNORE), S("tt", IGNORE),
  S("cal", IGNORE),
#undef S
};

// Symbols that are known not to typeset letters (other macros leave a gap).
static const char *sync_math_symbols[] = {
  "times", "cdot", "cdots", "ldots", "dots", "dotsc", "dotsb", "dotsm",
  "vdots", "ddots", "in", "notin", "ni", "subset", "subseteq", "supset",
  "supseteq", "cup", "cap", "bigcup", "bigcap", "sum", "prod", "coprod",
  "int", "iint", "iiint", "oint", "infty", "partial", "nabla", "pm", "mp",
  "leq", "le", "geq", "ge", "neq", "ne", "approx", "sim", "simeq", "cong",
  "equiv", "propto", "to", "gets", "rightarrow", "leftarrow", "Rightarrow",
  "Leftarrow", "leftrightarrow", "Leftrightarrow", "longrightarrow",
  "longleftarrow", "Longrightarrow", "Longleftarrow", "mapsto", "longmapsto",
  "implies", "impliedby", "iff", "uparrow", "downarrow", "Uparrow",
  "Downarrow", "forall", "exists", "nexists", "neg", "lnot", "land", "lor",
  "wedge", "vee", "bigwedge", "bigvee", "oplus", "otimes", "odot", "ominus",
  "bigoplus", "bigotimes", "bigodot", "circ", "bullet", "star", "ast",
  "dagger", "ddagger", "langle", "rangle", "lfloor", "rfloor", "lceil",
  "rceil", "lvert", "rvert", "lVert", "rVert", "vert", "Vert", "mid", "nmid",
  "parallel", "perp", "emptyset", "varnothing", "setminus", "backslash",
  "prime", "quad", "qquad", "colon", "top", "bot", "angle", "triangle",
  "square", "ll", "gg", "prec", "succ", "preceq", "succeq", "lesssim",
  "gtrsim", "hookrightarrow", "hookleftarrow", "div", "diamond", "cdotp",
  "ldotp", "triangleq", "coloneqq", "eqqcolon", "coloneq", "doteq",
  "models", "vdash", "dashv", "sqcup", "sqcap", "uplus", "amalg", "lhd",
  "rhd", "aleph", "surd", "lbrace", "rbrace", "lbrack", "rbrack", "over",
  "choose", "atop", "cr", "enspace", "thinspace", "medspace", "thickspace",
  "negthinspace", "negmedspace", "negthickspace", "leqslant", "geqslant",
  "subsetneq", "supsetneq", "rightleftharpoons", "rightharpoonup",
  "leftharpoonup", "circledast", "sphericalangle", "measuredangle",
  "checkmark", "dag", "ddag", "lozenge", "blacksquare", "Box", "Diamond",
  "flat", "sharp", "natural", "clubsuit", "heartsuit", "diamondsuit",
  "spadesuit", "wp", "mho", "complement", "therefore", "because",
  "leadsto", "nearrow", "searrow", "swarrow", "nwarrow", "updownarrow",
  "oslash", "bigtriangleup", "bigtriangledown", "wr", "asymp", "bowtie",
  "smile", "frown", "vartriangle", "trianglelefteq", "trianglerighteq",
};

static int sync_math_macro_kind(const char *name, int *value)
{
  *value = 0;
  for (size_t i = 0; i < sizeof(sync_math_macros) / sizeof(sync_math_macros[0]); i++)
    if (strcmp(sync_math_macros[i].name, name) == 0)
    {
      *value = sync_math_macros[i].value;
      return sync_math_macros[i].kind;
    }
  for (size_t i = 0; i < sizeof(sync_math_symbols) / sizeof(sync_math_symbols[0]); i++)
    if (strcmp(sync_math_symbols[i], name) == 0)
      return SYNC_MM_SYMBOL;
  return SYNC_MM_UNKNOWN;
}

// Operators whose scripts are limits in display style: a superscript is
// above them, and typeset first.
static bool sync_math_has_limits(const char *name, bool display)
{
  static const char *names[] = {
    "lim", "liminf", "limsup", "max", "min", "sup", "inf", "det", "Pr", "gcd",
    "injlim", "projlim", NULL
  };
  if (strcmp(name, "overbrace") == 0 || strcmp(name, "underbrace") == 0)
    return true;
  for (const char **p = names; display && *p; p++)
    if (strcmp(name, *p) == 0)
      return true;
  return false;
}

static void sync_math_list(struct sync_math *m, const struct sync_tok *s, int n);

// Text in math, as in \text{...}: spaces split words, $...$ is math.
static void sync_math_text(struct sync_math *m, const struct sync_tok *s, int n)
{
  sync_math_split(m);
  for (int i = 0; i < n;)
  {
    int c = s[i].c;
    if (c == '$')
    {
      int e = i + 1;
      while (e < n && s[e].c != '$')
        e += s[e].c == '\\' ? 2 : 1;
      if (e > n)
        e = n;
      bool display = m->display;
      m->display = false;
      sync_math_list(m, s + i + 1, e - i - 1);
      m->display = display;
      sync_math_split(m);
      i = e + 1;
      continue;
    }
    if (c == '\\')
    {
      char name[32];
      int j = sync_tok_name(s, n, i, name, sizeof(name));
      int value, kind = sync_math_macro_kind(name, &value);
      if (j < n && sync_is_letter(s[i + 1].c))
      {
        // The text of font macros is read on (their argument is a group).
        if (kind == SYNC_MM_SKIP || kind == SYNC_MM_REFERENCE)
        {
          int a, b;
          j = sync_tok_arg(s, n, j, &a, &b);
        }
        if (kind == SYNC_MM_REFERENCE || (kind != SYNC_MM_TEXT &&
                                          kind != SYNC_MM_IGNORE &&
                                          kind != SYNC_MM_SKIP))
        {
          sync_math_split(m);
          sync_gap_merge(m->gap, SYNC_MATH_GAP);
        }
        j = sync_tok_spaces(s, n, j);
      }
      else if (!name[0] || !strchr("\"'`^~=.", name[0]))
        // Control symbols other than accents (na\"ive) split words
        sync_math_split(m);
      i = j;
      continue;
    }
    if (c == '~')
      sync_math_split(m);
    else if (c != '{' && c != '}')
      sync_math_emit(m, c, s[i].col, s[i].end);
    i++;
  }
  sync_math_split(m);
}

// The atom at s[i]: a character, a group or a control sequence with its
// arguments. Returns the index after it, and when `process`, appends its
// characters. *name is the name of a control word, else empty.
static int sync_math_atom(struct sync_math *m, const struct sync_tok *s, int n,
                          int i, bool process, char *name)
{
  name[0] = 0;
  int c = s[i].c;
  if (c == '{')
  {
    int e = sync_tok_group_end(s, n, i, '{', '}');
    if (process)
      sync_math_list(m, s + i + 1, e - i - 1);
    return e < n ? e + 1 : n;
  }
  if (c != '\\')
  {
    if (process)
    {
      if (c == '&' || c == '~')
        sync_math_split(m);
      else if (c != '}' && c != '$' && c != '#')
        sync_math_emit(m, c, s[i].col, s[i].end);
    }
    return i + 1;
  }

  int j = sync_tok_name(s, n, i, name, 32);
  if (j == i + 1)
    return j;
  if (!sync_is_letter(s[i + 1].c))
  {
    // Control symbols: spaces and symbols. After \\, the number of the row
    // can come.
    bool row = name[0] == '\\';
    name[0] = 0;
    if (process)
    {
      sync_math_split(m);
      if (row)
        sync_gap_merge(m->gap, SYNC_MATH_GAP);
    }
    if (row)
    {
      int a, b;
      if (j < n && s[j].c == '*')
        j++;
      j = sync_tok_optional(s, n, j, &a, &b);
    }
    return j;
  }

  int a, b, a2, b2, oa, ob;
  const struct sync_macro *d = sync_macros_find(m->t->macros, name);
  if (d)
  {
    // Only its extent: sync_math_list expands it.
    if (d->def)
      j = sync_tok_optional(s, n, j, &oa, &ob);
    for (int k = d->def ? 1 : 0; k < d->nargs; k++)
      j = sync_tok_arg(s, n, j, &a, &b);
    if (process)
    {
      sync_math_split(m);
      sync_gap_merge(m->gap, SYNC_MATH_GAP | TXP_TEXT_GAP_ANYWHERE);
    }
    return j;
  }

  int value, kind = sync_math_macro_kind(name, &value);
  int col = s[i].col, end = s[j - 1].end;
  switch (kind)
  {
    case SYNC_MM_UNKNOWN:
      if (process)
      {
        sync_math_split(m);
        sync_gap_merge(m->gap, SYNC_MATH_GAP | TXP_TEXT_GAP_ANYWHERE);
      }
      return j;
    case SYNC_MM_SYMBOL:
      if (process)
        sync_math_split(m);
      return j;
    case SYNC_MM_IGNORE:
      return j;
    case SYNC_MM_LETTER:
      if (process)
        sync_math_emit(m, value, col, end);
      return j;
    case SYNC_MM_NAME:
      if (process)
      {
        sync_math_split(m);
        for (const char *p = value ? "mod" : name; *p; p++)
          sync_math_emit(m, *p, col, end);
        sync_math_split(m);
      }
      return j;
    case SYNC_MM_DIMEN:
    {
      // \mkern-9mu, \hskip 2pt
      j = sync_tok_spaces(s, n, j);
      int k = j;
      while (k < n && (s[k].c == '-' || s[k].c == '+' || s[k].c == '.' ||
                       (s[k].c >= '0' && s[k].c <= '9')))
        k++;
      if (k > j && k + 1 < n && sync_is_letter(s[k].c) &&
          sync_is_letter(s[k + 1].c))
        k += 2;
      if (process)
        sync_math_split(m);
      return k;
    }
    case SYNC_MM_ENV:
    {
      // Inner environments: \begin{cases}, \begin{array}{cc}, \begin{aligned}[t]
      char env[32] = "";
      j = sync_tok_arg(s, n, j, &a, &b);
      for (int k = a; k < b && k - a < 31; k++)
        env[k - a] = s[k].c, env[k - a + 1] = 0;
      if (strcmp(name, "begin") == 0)
      {
        j = sync_tok_optional(s, n, j, &oa, &ob);
        if (strncmp(env, "array", 5) == 0 || strncmp(env, "subarray", 8) == 0 ||
            strncmp(env, "alignedat", 9) == 0 || strncmp(env, "tabular", 7) == 0)
          j = sync_tok_arg(s, n, j, &a, &b);
      }
      if (process)
        sync_math_split(m);
      return j;
    }
    case SYNC_MM_ROOT:
    case SYNC_MM_ARROW:
      j = sync_tok_optional(s, n, j, &oa, &ob);
      j = sync_tok_arg(s, n, j, &a, &b);
      if (process)
      {
        // The index of a root comes first, what is below an arrow last.
        if (kind == SYNC_MM_ROOT && oa >= 0)
          sync_math_list(m, s + oa, ob - oa);
        sync_math_list(m, s + a, b - a);
        if (kind == SYNC_MM_ARROW && oa >= 0)
          sync_math_list(m, s + oa, ob - oa);
      }
      return j;
  }

  // Macros with arguments, after a star and optional arguments
  if (j < n && s[j].c == '*')
    j++;
  while ((j = sync_tok_optional(s, n, j, &oa, &ob)), oa >= 0)
    ;
  j = sync_tok_arg(s, n, j, &a, &b);
  a2 = b2 = 0;
  if (kind == SYNC_MM_FRACTION || kind == SYNC_MM_UNDERSET || kind == SYNC_MM_COLOR)
    j = sync_tok_arg(s, n, j, &a2, &b2);
  if (!process)
    return j;
  switch (kind)
  {
    case SYNC_MM_STYLE:
      sync_math_list(m, s + a, b - a);
      break;
    case SYNC_MM_TEXT:
      sync_math_text(m, s + a, b - a);
      break;
    case SYNC_MM_REFERENCE:
      sync_math_split(m);
      sync_gap_merge(m->gap, SYNC_MATH_GAP | TXP_TEXT_GAP_ANYWHERE);
      break;
    case SYNC_MM_FRACTION:
      sync_math_list(m, s + a, b - a);
      sync_math_list(m, s + a2, b2 - a2);
      break;
    case SYNC_MM_UNDERSET:
      sync_math_list(m, s + a2, b2 - a2);
      sync_math_list(m, s + a, b - a);
      break;
    case SYNC_MM_COLOR:
      sync_math_list(m, s + a2, b2 - a2);
      break;
    case SYNC_MM_MOD:
      sync_math_split(m);
      for (const char *p = "mod"; *p; p++)
        sync_math_emit(m, *p, col, end);
      sync_math_split(m);
      sync_math_list(m, s + a, b - a);
      break;
  }
  return j;
}

// Append the characters of the math s[0...n): atoms with their scripts.
static void sync_math_list(struct sync_math *m, const struct sync_tok *s, int n)
{
  struct sync_tok *own = NULL;
  int i = 0;
  while (i < n)
  {
    i = sync_tok_spaces(s, n, i);
    if (i >= n)
      break;
    char name[32];

    // A macro of the document: read on in its text, in place of the use.
    if (s[i].c == '\\' && i + 1 < n && sync_is_letter(s[i + 1].c))
    {
      int j = sync_tok_name(s, n, i, name, sizeof(name));
      const struct sync_macro *d = sync_macros_find(m->t->macros, name);
      int len;
      struct sync_tok *e = d && m->expansions < SYNC_MAX_EXPANSIONS
                               ? sync_expand(d, s, n, i, j, &len) : NULL;
      if (e)
      {
        m->expansions++;
        free(own);
        own = e;
        s = e;
        n = len;
        i = 0;
        continue;
      }
    }

    // The nucleus (none in {}^{14}C or at the start of a script)
    int start = i, nucleus = i, nucleus_end = i;
    name[0] = 0;
    if (s[i].c != '^' && s[i].c != '_')
      nucleus_end = i = sync_math_atom(m, s, n, i, false, name);
    bool limits = name[0] && sync_math_has_limits(name, m->display);

    // Its scripts
    int sup = -1, sup_end = 0, sub = -1, sub_end = 0;
    bool prime = false;
    for (;;)
    {
      int j = sync_tok_spaces(s, n, i);
      if (j >= n)
        break;
      if (s[j].c == '\'')
      {
        prime = true;
        i = j + 1;
        continue;
      }
      if (s[j].c == '^' || s[j].c == '_')
      {
        char script[32];
        int a = sync_tok_spaces(s, n, j + 1);
        int b = a < n ? sync_math_atom(m, s, n, a, false, script) : n;
        if (s[j].c == '^')
          sup = a, sup_end = b;
        else
          sub = a, sub_end = b;
        i = b;
        continue;
      }
      if (s[j].c == '\\')
      {
        char word[32];
        int k = sync_tok_name(s, n, j, word, sizeof(word));
        if (strcmp(word, "limits") == 0 || strcmp(word, "nolimits") == 0 ||
            strcmp(word, "displaylimits") == 0)
        {
          if (word[0] == 'l')
            limits = true;
          else if (word[0] == 'n')
            limits = false;
          i = k;
          continue;
        }
      }
      break;
    }

    if (limits && sup >= 0)
      sync_math_list(m, s + sup, sup_end - sup);
    if (nucleus_end > nucleus)
      sync_math_atom(m, s, nucleus_end, nucleus, true, name);
    if (prime)
      sync_math_split(m);
    if (!limits && sup >= 0)
      sync_math_list(m, s + sup, sup_end - sup);
    if (sub >= 0)
      sync_math_list(m, s + sub, sub_end - sub);
    if (i == start)
      i++;
  }
  free(own);
}

static bool sync_is_math_env(const char *env)
{
  static const char *names[] = {
    "equation", "equation*", "align", "align*", "gather", "gather*",
    "multline", "multline*", "flalign", "flalign*", "alignat", "alignat*",
    "eqnarray", "eqnarray*", "displaymath", "math", "dmath", "dmath*", NULL
  };
  for (const char **p = names; *p; p++)
    if (strcmp(*p, env) == 0)
      return true;
  return false;
}

// Where the math of t->math closes in line[i...]: the index of its closing
// delimiter, with the index after it in *after, else where the line ends
// (at a comment) with *after = -1.
static int sync_math_close(const int *line, int n, int i,
                           const struct sync_text *t, int *after)
{
  int depth = 0;
  *after = -1;
  for (; i < n; i++)
  {
    int c = line[i];
    if (c == '%')
      return i;
    if (c == '{')
      depth++;
    else if (c == '}')
      depth = depth > 0 ? depth - 1 : 0;
    else if (c == '$' && depth == 0 && t->math == SYNC_MATH_DOLLAR)
    {
      *after = i + 1;
      return i;
    }
    else if (c == '$' && t->math == SYNC_MATH_DOLLARS && i + 1 < n &&
             line[i + 1] == '$')
    {
      *after = i + 2;
      return i;
    }
    else if (c == '\\' && i + 1 < n)
    {
      int d = line[i + 1];
      if ((d == ')' && t->math == SYNC_MATH_PAREN) ||
          (d == ']' && t->math == SYNC_MATH_BRACKET))
      {
        *after = i + 2;
        return i;
      }
      if (t->math == SYNC_MATH_ENV && sync_looking_at(line, n, i, "\\end{"))
      {
        char env[32];
        sync_environment(line, n, i + 4, env, sizeof(env));
        if (strcmp(env, t->math_env) == 0)
        {
          *after = i + 6 + strlen(env);
          return i;
        }
      }
      i++;
    }
  }
  return n;
}

// The math of line[i...] (t->math is open): its characters are appended to
// t up to where it closes. Returns the index after it, or where the line
// ends.
static int sync_math_line(const int *line, int n, int i, struct sync_text *t,
                          bool *in_word, int *gap)
{
  static struct sync_tok toks[SYNC_TEXT_MAX];
  int after, end = sync_math_close(line, n, i, t, &after);
  int len = 0;
  for (int j = i; j < end; j++)
    toks[len++] = (struct sync_tok){line[j], j, j + 1};
  struct sync_math m = {
    .t = t, .in_word = in_word, .gap = gap,
    .display = t->math != SYNC_MATH_DOLLAR && t->math != SYNC_MATH_PAREN,
  };
  sync_math_list(&m, toks, len);
  if (after < 0)
    return end;
  *in_word = false;
  if (t->math == SYNC_MATH_ENV)
    // The number of the equation
    sync_gap_merge(gap, SYNC_MATH_GAP);
  t->math = SYNC_MATH_NONE;
  return after;
}

/* The macros of the document */

// The code points of a file without its comments, line breaks as spaces.
// The caller frees the array.
static int *sync_decode_file(fz_buffer *data, int *len)
{
  const char *p = (const char *)data->data, *end = p + data->len;
  int *out = malloc((data->len + 1) * sizeof(int)), n = 0;
  if (!out)
    return NULL;
  while (p < end)
  {
    int c;
    p += fz_chartorune(&c, p);
    if (c == '%' && !(n > 0 && out[n - 1] == '\\'))
    {
      while (p < end && *p != '\n')
        p++;
      for (p++; p < end && (*p == ' ' || *p == '\t'); p++)
        ;
      continue;
    }
    out[n++] = c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
  }
  *len = n;
  return out;
}

static int sync_skip_blank(const int *s, int n, int i)
{
  while (i < n && s[i] == ' ')
    i++;
  return i;
}

// The name of the control word at s[i] ("\name" or "{\name}"), returning the
// index after it (i if there is none).
static int sync_def_name(const int *s, int n, int i, char *name)
{
  int j = sync_skip_blank(s, n, i), k = 0;
  bool braced = j < n && s[j] == '{';
  if (braced)
    j = sync_skip_blank(s, n, j + 1);
  if (j >= n || s[j] != '\\')
    return i;
  for (j++; j < n && sync_is_letter(s[j]); j++)
    if (k < 31)
      name[k++] = s[j];
  name[k] = 0;
  if (braced)
  {
    j = sync_skip_blank(s, n, j);
    if (j >= n || s[j] != '}')
      return i;
    j++;
  }
  return k ? j : i;
}

// The group at s[i] (after spaces): its contents [*a, *b), and the index
// after it (i if there is none).
static int sync_def_group(const int *s, int n, int i, int open, int close,
                          int *a, int *b)
{
  int j = sync_skip_blank(s, n, i), depth = 0;
  if (j >= n || s[j] != open)
    return i;
  for (int k = j; k < n; k++)
  {
    if (s[k] == '\\')
      k++;
    else if (s[k] == open)
      depth++;
    else if (s[k] == close && --depth == 0)
    {
      *a = j + 1;
      *b = k;
      return k + 1;
    }
  }
  return i;
}

static int *sync_copy_ints(const int *s, int n)
{
  int *out = malloc((n > 0 ? n : 1) * sizeof(int));
  if (out && n > 0)
    memcpy(out, s, n * sizeof(int));
  return out;
}

static void sync_macros_add(struct sync_macros *ms, const char *name,
                            int nargs, const int *def, int ndef,
                            const int *body, int len)
{
  if (ms->count == ms->cap)
  {
    int cap = ms->cap ? ms->cap * 2 : 64;
    struct sync_macro *items = realloc(ms->items, cap * sizeof(*items));
    if (!items)
      return;
    ms->items = items;
    ms->cap = cap;
  }
  struct sync_macro *d = &ms->items[ms->count++];
  snprintf(d->name, sizeof(d->name), "%s", name);
  d->nargs = nargs;
  d->def = def ? sync_copy_ints(def, ndef) : NULL;
  d->ndef = ndef;
  d->body = sync_copy_ints(body, len);
  d->len = len;
}

// Read the definitions of a file, and of the files it inputs.
static void sync_macros_read(struct persistent_state *ps, ui_state *ui,
                             struct sync_macros *ms, const char *path,
                             int depth)
{
  fz_buffer *data = sync_file_data(ps, ui, path);
  int n;
  int *s = data ? sync_decode_file(data, &n) : NULL;
  if (!s)
    return;
  for (int i = 0; i < n; i++)
  {
    if (s[i] != '\\')
      continue;
    char cmd[32], name[32];
    int j = i + 1, k = 0;
    for (; j < n && sync_is_letter(s[j]); j++)
      if (k < 31)
        cmd[k++] = s[j];
    cmd[k] = 0;
    if (k == 0)
    {
      // A control symbol: \\, \%
      i = j;
      continue;
    }
    int a, b, da = -1, db = -1, nargs = 0;
    if (strcmp(cmd, "newcommand") == 0 || strcmp(cmd, "renewcommand") == 0 ||
        strcmp(cmd, "providecommand") == 0 ||
        strcmp(cmd, "DeclareRobustCommand") == 0)
    {
      // \newcommand*{\name}[nargs][default]{body}
      if (j < n && s[j] == '*')
        j++;
      int e = sync_def_name(s, n, j, name);
      if (e == j)
        continue;
      j = e;
      e = sync_def_group(s, n, j, '[', ']', &a, &b);
      if (e != j)
      {
        for (int q = a; q < b; q++)
          if (s[q] >= '0' && s[q] <= '9')
            nargs = nargs * 10 + s[q] - '0';
        j = sync_def_group(s, n, e, '[', ']', &da, &db);
      }
      e = sync_def_group(s, n, j, '{', '}', &a, &b);
      if (e == j)
        continue;
      sync_macros_add(ms, name, nargs, da >= 0 ? s + da : NULL,
                      da >= 0 ? db - da : 0, s + a, b - a);
      i = e - 1;
    }
    else if (strcmp(cmd, "def") == 0 || strcmp(cmd, "gdef") == 0)
    {
      // \def\name#1#2{body}; not with delimited parameters
      int e = sync_def_name(s, n, j, name);
      if (e == j)
        continue;
      j = e;
      bool plain = true;
      while (j < n && s[j] != '{')
      {
        if (s[j] == '#' && j + 1 < n && s[j + 1] >= '1' && s[j + 1] <= '9')
          nargs++, j += 2;
        else
          plain = false, j++;
      }
      e = sync_def_group(s, n, j, '{', '}', &a, &b);
      if (e == j)
        continue;
      if (plain)
        sync_macros_add(ms, name, nargs, NULL, 0, s + a, b - a);
      i = e - 1;
    }
    else if (strcmp(cmd, "DeclareMathOperator") == 0)
    {
      // \DeclareMathOperator*{\name}{text}: \operatorname{text}
      if (j < n && s[j] == '*')
        j++;
      int e = sync_def_name(s, n, j, name);
      if (e == j)
        continue;
      j = e;
      e = sync_def_group(s, n, j, '{', '}', &a, &b);
      if (e == j)
        continue;
      static const char op[] = "\\operatorname{";
      int len = (int)strlen(op), *body = malloc((len + b - a + 1) * sizeof(int));
      if (body)
      {
        for (int q = 0; q < len; q++)
          body[q] = op[q];
        memcpy(body + len, s + a, (b - a) * sizeof(int));
        body[len + b - a] = '}';
        sync_macros_add(ms, name, 0, NULL, 0, body, len + b - a + 1);
        free(body);
      }
      i = e - 1;
    }
    else if ((strcmp(cmd, "input") == 0 || strcmp(cmd, "include") == 0) &&
             depth < 2)
    {
      int e = sync_def_group(s, n, j, '{', '}', &a, &b);
      if (e == j || b - a >= 250)
        continue;
      char file[256 + 4];
      int len = 0;
      for (int q = a; q < b; q++)
        file[len++] = s[q] < 128 ? s[q] : '_';
      file[len] = 0;
      if (!strchr(file, '.'))
        strcat(file, ".tex");
      sync_macros_read(ps, ui, ms, file, depth + 1);
      i = e - 1;
    }
  }
  free(s);
}

static void sync_macros_free(struct sync_macros *ms)
{
  for (int i = 0; i < ms->count; i++)
  {
    free(ms->items[i].def);
    free(ms->items[i].body);
  }
  free(ms->items);
  ms->items = NULL;
  ms->count = ms->cap = 0;
}

// The macros of the document, read from its main file (with the files it
// inputs) as the editor last sent them.
static void sync_macros_load(struct persistent_state *ps, ui_state *ui,
                             struct sync_macros *ms)
{
  sync_macros_free(ms);
  sync_macros_read(ps, ui, ms, ps->doc_name, 0);
}

// Split a source line into the words likely to be typeset as text: macro
// names, optional arguments, and the arguments of macros that do not
// typeset text are left out, math is read as it is typeset (sync_math_line).
// The words are appended to t, which also keeps track of the segments (see
// sync_word) and of math from line to line.
static void sync_source_words(const int *line, int n, struct sync_text *t)
{
  bool in_word = false;
  int gap = t->pending_gap;
  // The characters and the gap after the last math of the line whose gap
  // carries over to the next line
  int math_nchars = -1, math_gap = 0;
  // Brackets right after a macro hold an optional argument (\item[...],
  // \\[2pt]); elsewhere they are text.
  bool after_macro = false;
  int i = 0;
  if (t->verbatim[0])
    i = sync_verbatim_environment(line, n, 0, t);
  while (i < n && t->nchars < SYNC_TEXT_MAX && t->nwords < SYNC_TEXT_MAX)
  {
    int c = line[i];
    if (c == '%')
      break;
    if (t->math && t->tikz && !sync_in_node(t))
    {
      // Coordinates of a drawing: ($(a)!0.5!(b)$)
      int after, end = sync_math_close(line, n, i, t, &after);
      if (after >= 0)
        t->math = SYNC_MATH_NONE;
      i = after >= 0 ? after : end;
      continue;
    }
    if (t->math)
    {
      bool env = t->math == SYNC_MATH_ENV;
      i = sync_math_line(line, n, i, t, &in_word, &gap);
      if (env || t->math)
        math_nchars = t->nchars, math_gap = gap;
      continue;
    }
    if (c == '\\' && i + 1 < n)
    {
      int d = line[i + 1];
      if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') || d == '@')
      {
        char name[32];
        int j = i + 1, k = 0;
        while (j < n && ((line[j] >= 'a' && line[j] <= 'z') ||
                         (line[j] >= 'A' && line[j] <= 'Z') || line[j] == '@'))
        {
          if (k < 31)
            name[k++] = line[j];
          j++;
        }
        name[k] = 0;
        in_word = false;
        gap = sync_macro_is_heading(name) ? SYNC_NUMBER_GAP : TXP_TEXT_MAX_GAP;
        i = j;
        if (t->tikz && !sync_in_node(t) && strcmp(name, "node") == 0)
          t->node = true;
        bool mint = strcmp(name, "mintinline") == 0;
        if ((mint || strcmp(name, "verb") == 0 ||
                      strcmp(name, "lstinline") == 0))
        {
          // \verb|code|, \mintinline{lang}{code}, \lstinline[opts]|code|
          if (i < n && line[i] == '*')
            i++;
          if (i < n && line[i] == '[')
            i = sync_skip_group(line, n, i, '[', ']');
          if (mint && i < n && line[i] == '{')
            i = sync_skip_group(line, n, i, '{', '}');
          if (i < n)
          {
            char end[2] = {line[i] == '{' ? '}' : (char)line[i], 0};
            bool closed;
            i = sync_verbatim(line, n, i + 1, end, false, t, &closed);
          }
          continue;
        }
        if (sync_macro_moves_argument(name))
        {
          while (i < n && line[i] == ' ')
            i++;
          if (i < n && line[i] == '[')
            i = sync_skip_group(line, n, i, '[', ']');
          if (i < n && line[i] == '{')
            sync_segment_open(t, SYNC_SEGMENT_ARGUMENT);
        }
        else if (sync_macro_skips_argument(name))
        {
          if (strcmp(name, "def") == 0)
            // \def\name{...}
            while (i < n && line[i] != '{')
              i++;
          while (i < n && line[i] == ' ')
            i++;
          if (i < n && line[i] == '[')
            i = sync_skip_group(line, n, i, '[', ']');
          bool begin = strcmp(name, "begin") == 0;
          if (begin || strcmp(name, "end") == 0)
          {
            char env[32];
            sync_environment(line, n, i, env, sizeof(env));
            if (sync_is_float(env) && begin)
              sync_segment_open(t, SYNC_SEGMENT_FLOAT);
            else if (sync_is_float(env))
            {
              while (t->nsegments > 0 &&
                     t->segment_kind[--t->nsegments] != SYNC_SEGMENT_FLOAT)
                ;
            }
            else if (strcmp(env, "tikzpicture") == 0)
              t->tikz += begin ? 1 : t->tikz > 0 ? -1 : 0;
            if (i < n && line[i] == '{')
              i = sync_skip_group(line, n, i, '{', '}');
            // The arguments of an environment follow its name:
            // \begin{tabular}{p{1in}l}, \begin{figure}[t].
            bool args = begin && sync_env_skips_arguments(env);
            while (i < n && (line[i] == '[' || (args && line[i] == '{')))
              i = line[i] == '[' ? sync_skip_group(line, n, i, '[', ']')
                                 : sync_skip_group(line, n, i, '{', '}');
            if (begin && sync_is_verbatim(env))
            {
              strcpy(t->verbatim, env);
              t->hidden = strcmp(env, "comment") == 0;
              i = sync_verbatim_environment(line, n, i, t);
            }
            else if (begin && sync_is_math_env(env))
            {
              t->math = SYNC_MATH_ENV;
              strcpy(t->math_env, env);
              in_word = false;
              gap = 0;
            }
            continue;
          }
          if (i < n && line[i] == '{')
            i = sync_skip_group(line, n, i, '{', '}');
          while (i < n && line[i] == '[')
            i = sync_skip_group(line, n, i, '[', ']');
        }
        else
          after_macro = true;
        continue;
      }
      if (d == '(' || d == '[')
        t->math = d == '(' ? SYNC_MATH_PAREN : SYNC_MATH_BRACKET, gap = 0;
      if (d == '(' || d == '[' || d == ')' || d == ']')
        in_word = false;
      // Other control symbols (accents, \_, \&, ...) do not split words.
      after_macro = d == '\\';
      i += 2;
      continue;
    }
    if (c == '$')
    {
      bool dollars = i + 1 < n && line[i + 1] == '$';
      t->math = dollars ? SYNC_MATH_DOLLARS : SYNC_MATH_DOLLAR;
      in_word = after_macro = false;
      gap = 0;
      i += dollars ? 2 : 1;
      continue;
    }
    if (c == '[' && after_macro)
    {
      in_word = after_macro = false;
      i = sync_skip_group(line, n, i, '[', ']');
      continue;
    }
    if (t->tikz && !sync_in_node(t) && c != '{' && c != '}')
    {
      // Drawing commands: only the text of nodes is typeset. The options and
      // the name or position of a node come before its text.
      in_word = false;
      if (c == '[' || c == '(')
      {
        i = sync_skip_group(line, n, i, c, c == '[' ? ']' : ')');
        continue;
      }
      int j = i;
      while (j < n && ((line[j] >= 'a' && line[j] <= 'z') ||
                       (line[j] >= 'A' && line[j] <= 'Z')))
        j++;
      if (j == i + 4 && line[i] == 'n' && line[i + 1] == 'o' &&
          line[i + 2] == 'd' && line[i + 3] == 'e')
        t->node = true;
      i = j > i ? j : i + 1;
      continue;
    }
    if (c != ' ' && c != '*')
      after_macro = false;
    if (c == '{')
    {
      if (t->tikz && t->node && !sync_in_node(t))
        sync_segment_open(t, SYNC_SEGMENT_NODE);
      t->node = false;
      t->depth++;
    }
    else if (c == '}')
    {
      if (t->nsegments > 0 &&
          t->segment_kind[t->nsegments - 1] != SYNC_SEGMENT_FLOAT &&
          t->segment_depth[t->nsegments - 1] == t->depth)
      {
        // Back to the text around the footnote, after its mark.
        t->nsegments--;
        in_word = false;
        gap = TXP_TEXT_MAX_GAP;
      }
      t->depth--;
    }
    int f = txp_fold_char(c);
    if (f)
    {
      sync_add_char(t, &in_word, i, i + 1, f, gap);
      gap = 0;
    }
    else if (c != '{' && c != '}')
      in_word = false;
    i++;
  }
  // Other gaps at the end of a line stand for markup (\label, \hline).
  t->pending_gap = t->nchars == math_nchars ? math_gap : 0;
}

// The start of a line (1-based) of a file, NULL past its end.
static const char *sync_line_start(fz_buffer *data, int line)
{
  const char *p = (const char *)data->data, *end = p + data->len;
  for (int l = 1; l < line && p < end; l++)
  {
    const char *nl = memchr(p, '\n', end - p);
    p = nl ? nl + 1 : end;
  }
  return p < end ? p : NULL;
}

// Decode the line starting at p into code points (at most SYNC_TEXT_MAX),
// returning their number, and the start of the next line in *next.
static int sync_decode_line(fz_buffer *data, const char *p, int *line,
                            const char **next)
{
  const char *end = (const char *)data->data + data->len;
  int n = 0;
  while (p < end && *p != '\n')
  {
    int c;
    p += fz_chartorune(&c, p);
    if (n < SYNC_TEXT_MAX)
      line[n++] = c;
  }
  *next = p < end ? p + 1 : NULL;
  return n;
}

// Start reading the lines of a file from `first` into t: the lines before it
// (at most SYNC_CONTEXT_LINES) tell which floats, footnotes and TikZ
// pictures are open. Returns the start of line `first`.
#define SYNC_CONTEXT_LINES 300

static const char *sync_text_begin(fz_buffer *data, int first,
                                   struct sync_text *t)
{
  static int line[SYNC_TEXT_MAX];
  int l = first > SYNC_CONTEXT_LINES ? first - SYNC_CONTEXT_LINES : 1;
  const char *p = sync_line_start(data, l);
  sync_text_reset(t);
  for (; p && l < first; l++)
  {
    int n = sync_decode_line(data, p, line, &p);
    sync_source_words(line, n, t);
    t->nchars = t->nwords = 0;
  }
  return p;
}

// A cursor inside the name of a macro that typeset characters of words
// [w, t->nwords) (\al|pha, \D|in) stands before the macro.
static int sync_macro_column(const int *line, int n, int column,
                             const struct sync_text *t, int w)
{
  if (column <= 0 || column >= n || !sync_is_letter(line[column]))
    return column;
  int i = column;
  while (i > 0 && sync_is_letter(line[i - 1]))
    i--;
  int slashes = 0;
  for (int j = i - 1; j >= 0 && line[j] == '\\'; j--)
    slashes++;
  if (slashes % 2 == 0)
    return column;
  for (int k = w < t->nwords ? t->words[w].first : t->nchars; k < t->nchars; k++)
    if (t->source[k] == i - 1)
      return i - 1;
  return column;
}

// Forward: the position of the editor cursor (ui->sync_target) in the text
// of the displayed page, with the length of the matched text (0 if not
// found; only text of at least min_len characters is looked for, *max_len is
// the longest). With `nearest`, SyncTeX found the target line
// with a column: take the occurrence of the words around the cursor
// nearest to the anchor. Otherwise, prefer an occurrence in `region`, then
// the first one after the anchor in reading order.
#define SYNC_NEAREST_MAX 300

static int sync_refine_by_text(struct persistent_state *ps, ui_state *ui,
                               fz_point anchor, fz_rect region, bool region_only,
                               bool nearest, int min_len, int *max_len,
                               fz_point *out, fz_rect *out_line)
{
  *max_len = 0;
  int column = ui->sync_target.column;
  if (column < 0 || !ui->sync_target.path[0])
    return 0;

  int target = ui->sync_target.line;
  fz_buffer *data = sync_file_data(ps, ui, ui->sync_target.path);
  static struct sync_text t;
  static struct sync_macros macros;
  sync_macros_load(ps, ui, &macros);
  t.macros = &macros;
  int first = target > 2 ? target - 2 : 1;
  const char *p = data ? sync_text_begin(data, first, &t) : NULL;
  if (!p)
    return 0;

  // The words of the target line, [w0, w1), with those of the lines around
  // it (two before, at least three words after): the text flows across
  // source lines, and lines without words (markup, blank lines) do not
  // count.
  static int line[SYNC_TEXT_MAX];
  int w0 = 0, w1 = 0;
  for (int l = first; p && (l <= target + 1 ||
                            (l <= target + 8 && t.nwords - w1 < 3)); l++)
  {
    int n = sync_decode_line(data, p, line, &p);
    if (l == target)
      w0 = t.nwords;
    sync_source_words(line, n, &t);
    if (l == target)
    {
      w1 = t.nwords;
      column = sync_macro_column(line, n, column, &t, w0);
    }
  }
  if (getenv("TXP_SYNC_DEBUG"))
    fprintf(stderr, "[synctex forward] words %d-%d of %d, math %d, tikz %d\n",
            w0, w1, t.nwords, t.math, t.tikz);
  if (w0 == w1)
    return 0;

  // The word under the cursor (or right before it), else the next one, else
  // the last one.
  int k = -1;
  for (int i = w0; i < w1; i++)
    if (t.words[i].start <= column && column <= t.words[i].end)
    {
      k = i;
      break;
    }
  if (k == -1)
    for (int i = w0; i < w1 && k == -1; i++)
      if (t.words[i].start > column)
        k = i;
  if (k == -1)
    k = w1 - 1;

  // The character of word k at the cursor: the cursor is at its left edge
  // when it stands right before it, else at the right edge of the one it
  // follows directly (at the end of the word, or before markup inside it,
  // as in d|\_in), else at the left edge of the next one. The characters of
  // math are not in the order of the source.
  const int *src = t.source + t.words[k].first;
  const int *src_end = t.source_end + t.words[k].first;
  int count = t.words[k].count, in_word = -1;
  bool after = false;
  for (int i = 0; i < count && in_word < 0; i++)
    if (src[i] == column)
      in_word = i;
  for (int i = count - 1; i >= 0 && in_word < 0; i--)
    if (src_end[i] == column)
      in_word = i, after = true;
  for (int i = 0; i < count; i++)
    if (src[i] > column && (in_word < 0 || (!after && src[i] < src[in_word])))
      in_word = i;
  if (in_word < 0)
  {
    in_word = 0;
    for (int i = 1; i < count; i++)
      if (src_end[i] > src_end[in_word])
        in_word = i;
    after = true;
  }

  // The words typeset with word k: those of its segment.
  static int seq[SYNC_TEXT_MAX];
  int nseq = 0, kk = 0;
  for (int i = 0; i < t.nwords; i++)
    if (t.words[i].segment == t.words[k].segment)
    {
      if (i == k)
        kk = nseq;
      seq[nseq++] = i;
    }

  // Try the longest needles first: a single short word matches anywhere.
  // Spans are in words, grown to SYNC_NEEDLE_MIN characters.
  static const int spans[][2] = {{0, 2}, {-1, 1}, {0, 1}, {-1, 0}, {-2, 0}, {0, 0}};
  static int needle[SYNC_TEXT_MAX];
  static unsigned char gap[SYNC_TEXT_MAX];
  int tried[sizeof(spans) / sizeof(spans[0])][2];
  for (int s = 0; s < (int)(sizeof(spans) / sizeof(spans[0])); s++)
  {
    int a = kk + spans[s][0], b = kk + spans[s][1];
    tried[s][0] = tried[s][1] = -1;
    if (a < 0 || b >= nseq)
      continue;
    // Words of math are single letters: take more of them, on the sides
    // the span extends to, until the needle is as long as a few words of
    // text.
    int total = 0;
    for (int m = a; m <= b; m++)
      total += t.words[seq[m]].count;
    while (total < SYNC_NEEDLE_MIN)
    {
      bool left = spans[s][0] < 0 && a > 0, right = spans[s][1] > 0 && b + 1 < nseq;
      if (left && (!right || kk - a <= b - kk))
        total += t.words[seq[--a]].count;
      else if (right)
        total += t.words[seq[++b]].count;
      else
        break;
    }
    bool seen = false;
    for (int r = 0; r < s; r++)
      seen |= tried[r][0] == a && tried[r][1] == b;
    tried[s][0] = a, tried[s][1] = b;
    if (seen)
      continue;
    int len = 0, offset = 0;
    for (int m = a; m <= b; m++)
    {
      struct sync_word *w = &t.words[seq[m]];
      if (m == kk)
        offset = len + in_word;
      memcpy(needle + len, t.chars + w->first, w->count * sizeof(int));
      memcpy(gap + len, t.gap + w->first, w->count);
      // Words of other segments in between: the page has a mark there.
      if (m > a && seq[m] != seq[m - 1] + 1)
        gap[len] = TXP_TEXT_MAX_GAP;
      len += w->count;
    }
    // A single word of four letters only as a last resort, near the anchor
    // or inside the region.
    if (len < 4 || (a == b && len < (nearest || region_only ? 4 : 5)))
      continue;
    if (len > *max_len)
      *max_len = len;
    if (len < min_len)
      continue;
    fz_point pt;
    fz_rect lb;
    bool matched = txp_renderer_find_text(ps->ctx, ui->doc_renderer, needle,
                                          gap, len, offset, after, anchor, region,
                                          nearest ? SYNC_NEAREST_MAX : -1, &pt, &lb);
    if (getenv("TXP_SYNC_DEBUG"))
    {
      fprintf(stderr, "[synctex forward] needle ");
      for (int i = 0; i < len; i++)
      {
        char utf8[FZ_UTFMAX + 1];
        utf8[fz_runetochar(utf8, needle[i])] = 0;
        fprintf(stderr, "%s%s%s", i == offset ? "^" : "", gap[i] ? "|" : "", utf8);
      }
      if (matched)
        fprintf(stderr, ": (%.2f, %.2f)\n", pt.x, pt.y);
      else
        fprintf(stderr, ": no match\n");
    }
    if (!matched)
      continue;
    if (region_only && !fz_is_point_inside_rect(pt, fz_expand_rect(region, 2)))
      // Matched outside the picture only: not trustworthy.
      return 0;
    *out = pt;
    *out_line = lb;
    return len;
  }
  return 0;
}

// Backward: the source position of the text at `pt` on the displayed page.
// The characters around pt are aligned with the text of the source lines
// around the SyncTeX candidate (*line, 1-based, and *column, the cursor
// position before the material or -1): the longest match wins, ties go to
// the nearest one. Text of the page that is not in the source (math,
// references, citations, macros) is skipped where the source has material
// left out; a click on such text goes to that material. On success, *line
// and *column are updated.
#define SYNC_BACK_RADIUS 80
#define SYNC_BACK_LINES_BEFORE 120
#define SYNC_BACK_LINES_AFTER 30
#define SYNC_BACK_MIN_MATCH 5

struct sync_char {
  int c, line;
  int column, column_end;  // the characters it stands for
  bool gap;        // material left out before this character
  int gap_column;  // where it starts
  int segment, order;
};

static int sync_char_compare(const void *a, const void *b)
{
  const struct sync_char *x = a, *y = b;
  if (x->segment != y->segment)
    return x->segment < y->segment ? -1 : 1;
  return x->order - y->order;
}

// Whether src[s...] matches rendered[r...] in direction dir, up to the next
// gap, over at most TXP_TEXT_LOOKAHEAD characters.
static bool sync_match_ahead(const struct sync_char *src, int ns, int s,
                             const int *rendered, int nr, int r, int dir)
{
  for (int m = 0; m < TXP_TEXT_LOOKAHEAD; m++, s += dir, r += dir)
  {
    if (s < 0 || s >= ns)
      return true;
    if (m > 0 && (dir > 0 ? src[s].gap : src[s + 1].gap))
      return true;
    if (r < 0 || r >= nr || src[s].c != rendered[r])
      return false;
  }
  return true;
}

// Number of characters of src from s matching those of rendered from r, in
// direction dir. At a gap of the source, at most TXP_TEXT_MAX_GAP rendered
// characters are skipped: the fewest after which the source goes on
// matching.
static int sync_match_run(const struct sync_char *src, int ns, int s,
                          const int *rendered, int nr, int r, int dir)
{
  int count = 0;
  for (; s >= 0 && s < ns && r >= 0 && r < nr; s += dir, r += dir, count++)
  {
    // The gap between src[s] and the character matched before it.
    bool gap = dir > 0 ? src[s].gap : src[s + 1].gap;
    if (!gap)
    {
      if (src[s].c != rendered[r])
        break;
      continue;
    }
    int k = 0;
    while (k <= TXP_TEXT_MAX_GAP &&
           !sync_match_ahead(src, ns, s, rendered, nr, r + dir * k, dir))
      k++;
    if (k > TXP_TEXT_MAX_GAP)
      break;
    r += dir * k;
    if (r < 0 || r >= nr)
      break;
  }
  return count;
}

static bool sync_backward_by_text(struct persistent_state *ps, ui_state *ui,
                                  fz_point pt, const char *path,
                                  int *line, int *column)
{
  int rendered[2 * SYNC_BACK_RADIUS + 1], at;
  bool after;
  int nr = txp_renderer_text_at(ps->ctx, ui->doc_renderer, pt,
                                SYNC_BACK_RADIUS, rendered, &at, &after);
  if (nr == 0)
    return false;

  fz_buffer *data = sync_file_data(ps, ui, path);
  int first_line = *line > SYNC_BACK_LINES_BEFORE ? *line - SYNC_BACK_LINES_BEFORE : 1;
  static struct sync_text t;
  static struct sync_macros macros;
  sync_macros_load(ps, ui, &macros);
  t.macros = &macros;
  const char *p = data ? sync_text_begin(data, first_line, &t) : NULL;
  if (!p)
    return false;

  // Folded characters of the source lines, with their line and column.
  struct sync_char *src = NULL;
  int ns = 0, cap = 0;
  static int text[SYNC_TEXT_MAX];
  for (int l = first_line; p && l <= *line + SYNC_BACK_LINES_AFTER; l++)
  {
    int n = sync_decode_line(data, p, text, &p);
    t.nchars = t.nwords = 0;
    sync_source_words(text, n, &t);
    if (ns + t.nchars > cap)
    {
      cap = (ns + t.nchars) * 2;
      src = fz_realloc(ps->ctx, src, cap * sizeof(*src));
    }
    for (int i = 0; i < t.nchars; i++, ns++)
    {
      src[ns].c = t.chars[i];
      src[ns].line = l;
      src[ns].column = t.source[i];
      src[ns].column_end = t.source_end[i];
      src[ns].gap = t.gap[i];
      // Material left out starts after the previous word of the line.
      int g = i == 0 ? 0 : t.source_end[i - 1] < t.source[i]
                                ? t.source_end[i - 1] : t.source[i];
      while (g < t.source[i] && (text[g] == ' ' || text[g] == '\t'))
        g++;
      src[ns].gap_column = g;
      src[ns].order = ns;
    }
    for (int w = 0; w < t.nwords; w++)
      for (int i = 0; i < t.words[w].count; i++)
        src[ns - t.nchars + t.words[w].first + i].segment = t.words[w].segment;
  }

  // The running text first, then each footnote: the text of each is
  // contiguous on the page, with a mark where a footnote was.
  qsort(src, ns, sizeof(*src), sync_char_compare);
  for (int s = 1; s < ns; s++)
    if (src[s].order != src[s - 1].order + 1)
      src[s].gap = true;
  if (getenv("TXP_SYNC_DEBUG"))
  {
    for (int s = 0; s < ns; s++)
    {
      if (s == 0 || src[s].segment != src[s - 1].segment)
        fprintf(stderr, "\n[%d] %d:", src[s].segment, src[s].line);
      fprintf(stderr, "%s%c", src[s].gap ? "|" : "", src[s].c);
    }
    fprintf(stderr, "\nrendered: ");
    for (int r = 0; r < nr; r++)
      fprintf(stderr, r == at ? "[%c]" : "%c", rendered[r]);
    fprintf(stderr, "\n");
  }

  // Candidates: the clicked character is src[s], or it is part of the text
  // that the gap before src[s] stands for.
  int best = -1, best_score = 0;
  bool best_in_gap = false;
  long best_dist = 0;
  for (int s = 0; s < ns; s++)
  {
    int score = 0;
    bool in_gap = false;
    if (src[s].c == rendered[at])
      score = 1 + sync_match_run(src, ns, s + 1, rendered, nr, at + 1, 1) +
              sync_match_run(src, ns, s - 1, rendered, nr, at - 1, -1);
    if (src[s].gap)
    {
      // The first match of src[s...] after the click and of src[...s-1]
      // before it, around at most TXP_TEXT_MAX_GAP characters.
      int r1 = at + 1, r0 = at - 1;
      while (r1 < nr && r1 - at <= TXP_TEXT_MAX_GAP &&
             !sync_match_ahead(src, ns, s, rendered, nr, r1, 1))
        r1++;
      while (r0 >= 0 && at - r0 <= TXP_TEXT_MAX_GAP &&
             !sync_match_ahead(src, ns, s - 1, rendered, nr, r0, -1))
        r0--;
      if (r1 < nr && r0 >= 0 && r1 - r0 - 1 <= TXP_TEXT_MAX_GAP && s > 0)
      {
        int gs = sync_match_run(src, ns, s, rendered, nr, r1, 1) +
                 sync_match_run(src, ns, s - 1, rendered, nr, r0, -1);
        if (gs > score)
          score = gs, in_gap = true;
      }
    }
    if (score == 0)
      continue;
    long dist = labs((long)src[s].line - *line) * 100000;
    if (src[s].line == *line && *column >= 0)
      dist += labs((long)src[s].column - *column);
    if (score > best_score || (score == best_score && dist < best_dist))
    {
      best = s;
      best_score = score;
      best_dist = dist;
      best_in_gap = in_gap;
    }
  }

  bool found = best >= 0 && best_score >= SYNC_BACK_MIN_MATCH;
  if (found)
  {
    *line = src[best].line;
    *column = best_in_gap ? src[best].gap_column
              : after     ? src[best].column_end
                          : src[best].column;
    fprintf(stderr, "[synctex backward] refined by text: line %d column %d "
            "(%d characters match%s)\n",
            *line, *column, best_score, best_in_gap ? ", in a gap" : "");
  }
  fz_free(ps->ctx, src);
  return found;
}

// Move the spring back to rest when the gesture no longer holds it (right
// after turning the page, the new page comes in from the side of the old
// one), and move the page
static void overscroll_step(fz_context *ctx, ui_state *ui, uint32_t now)
{
  float t = ui->overscroll.tension, v = ui->overscroll.velocity;
  bool held = !ui->overscroll.flipped &&
              (ui->overscroll.touching ||
               now - ui->overscroll.last_wheel <= WHEEL_GESTURE_GAP_MS);
  if ((t != 0 || v != 0) && !held)
  {
    // No frames while nothing moves: do not jump after a pause
    float dt = fminf(now - ui->last_frame, 2 * OVERSCROLL_FRAME_MS);
    // Critically damped: back as fast as it can without oscillating
    float w = 1.0f / OVERSCROLL_SPRING_MS, a = v + w * t, e = expf(-w * dt);
    t = (t + a * dt) * e;
    v = (v - w * a * dt) * e;
    if (fabsf(t) < 0.5f && fabsf(v) * OVERSCROLL_FRAME_MS < 0.5f)
      t = v = 0;
    ui->overscroll.tension = t;
    ui->overscroll.velocity = v;
  }
  // The page moves less and less, up to OVERSCROLL_STRETCH
  float s = OVERSCROLL_STRETCH * get_scale_factor(ui->window).y;
  txp_renderer_get_config(ctx, ui->doc_renderer)->overscroll =
    s * t / (fabsf(t) + s);
}

static void render(fz_context *ctx, ui_state *ui)
{
  uint32_t now = SDL_GetTicks();
  overscroll_step(ctx, ui, now);
  ui->last_frame = now;
  SDL_SetRenderDrawColor(ui->sdl_renderer, 0, 0, 0, 255);
  SDL_RenderClear(ui->sdl_renderer);
  txp_renderer_render(ctx, ui->doc_renderer);
  render_sync_mark(ctx, ui);
  SDL_RenderPresent(ui->sdl_renderer);
}

struct repaint_on_resize_env
{
  fz_context *ctx;
  ui_state *ui;
};

static int repaint_on_resize(void *data, SDL_Event *event)
{
  struct repaint_on_resize_env *env = data;
  if (event->type == SDL_WINDOWEVENT &&
      event->window.event == SDL_WINDOWEVENT_RESIZED &&
      SDL_GetWindowFromID(event->window.windowID) == env->ui->window)
  {
    render(env->ctx, env->ui);
  }
  return 0;
}

/* Document processing */

static bool need_advance(fz_context *ctx, ui_state *ui)
{
  if (send(get_status, ui->eng) != DOC_RUNNING)
    return false;

  if (send(is_finishing, ui->eng))
    return true;

  int need = send(page_count, ui->eng) <= ui->page;

  if (!need)
  {
    fz_buffer *buf;
    synctex_t *stx = send(synctex, ui->eng, &buf);
    need =
      (ui->need_synctex && synctex_page_count(stx) <= ui->page) ||
      synctex_has_target(stx);
  }

  return need;
}

static bool advance_engine(fz_context *ctx, ui_state *ui)
{
  bool need = need_advance(ctx, ui);
  if (!need && ui->advancing)
    editor_flush();
  ui->advancing = need;
  if (!need)
    return false;

  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  int steps = 10;
  while (need)
  {
    if (!send(step, ui->eng, ctx, false))
      break;

    steps -= 1;
    need = need_advance(ctx, ui);

    if (steps == 0)
    {
      steps = 10;

      struct timespec curr;
      clock_gettime(CLOCK_MONOTONIC, &curr);

      int delta =
        (curr.tv_sec - start.tv_sec) * 1000 * 1000 * 1000 +
        (curr.tv_nsec - start.tv_nsec);

      if (delta > 5000000)
        break;
    }
  }
  return need;
}

static fz_point get_scale_factor(SDL_Window *window)
{
  int ww, wh, pw, ph;
  SDL_GetWindowSize(window, &ww, &wh);

#if SDL_VERSION_ATLEAST(2, 0, 26)
  SDL_GetWindowSizeInPixels(window, &pw, &ph);
#else
  SDL_GetRendererOutputSize(SDL_GetRenderer(window), &pw, &ph);
#endif

  return fz_make_point(ww != 0 ? (float)pw / ww : 1,
                       wh != 0 ? (float)ph / wh : 1);
}

/* UI events */

// Mouse event coordinates are window points with SDL2 proper, but sdl2-compat
// (the SDL2 API implemented over SDL3) converts them to renderer coordinates,
// which are pixels on high-DPI displays. Scaling those by the pixel density
// again put every click at twice its distance from the window origin.
// SDL_GetMouseState reports points in both cases, so take the position from it.
static void mouse_position_in_points(int *x, int *y)
{
  SDL_GetMouseState(x, y);
}

/* Hyperlinks */

static void display_page(struct persistent_state *ps, ui_state *ui);

static const char *relative_path(const char *path, const char *dir, int *go_up);

/* Glyph sources. The engine writes the source position (input tag, line and
   column, as in the SyncTeX records) of the glyphs of TeX fonts into the DVI
   (txp: specials, see txp_glyph_src in xetex-shipout.c). They are exact:
   characters of macros stand for the macro use, ligatures for all their
   characters, the hyphens of hyphenation for none. Text in OpenType fonts
   and pictures have no glyph source; there the text matching above
   applies. */

// Whether the input file with a tag is the file at path (relative to the
// document). A few tags are remembered.
struct sync_tags {
  int tag[16];
  bool is[16];
  int count;
};

static bool sync_tag_is(struct persistent_state *ps, synctex_t *stx,
                        fz_buffer *buf, struct sync_tags *tags, int tag,
                        const char *path)
{
  for (int i = 0; i < tags->count; i++)
    if (tags->tag[i] == tag)
      return tags->is[i];
  const char *name;
  int len = synctex_input_name(stx, buf, tag, &name);
  bool is = false;
  if (len > 0 && len < 1024)
  {
    char full[1024];
    memcpy(full, name, len);
    full[len] = 0;
    const char *rel = full;
    int go_up = 0;
    if (rel[0] == '/')
      rel = relative_path(rel, ps->doc_path, &go_up);
    while (rel[0] == '.' && rel[1] == '/')
      rel += 2;
    is = go_up == 0 && strcmp(rel, path) == 0;
  }
  if (tags->count < 16)
  {
    tags->tag[tags->count] = tag;
    tags->is[tags->count] = is;
    tags->count++;
  }
  return is;
}

// The end of the control word at a column of a line, or the column if there
// is none
static int sync_control_word_end(const int *line, int n, int column)
{
  if (column < 0 || column >= n || line[column] != '\\')
    return column;
  int i = column + 1;
  while (i < n && sync_is_letter(line[i]))
    i++;
  return i == column + 1 && i < n ? i + 1 : i;
}

// Whether characters [from, to) of a line have one that prints (not
// spaces, braces, math shifts, scripts, alignments, comments or the names
// of macros): typeset text without a glyph source (in an OpenType font).
static bool sync_prints_between(const int *line, int n, int from, int to)
{
  if (to > n)
    to = n;
  for (int i = 0; i < to;)
  {
    int c = line[i];
    if (c == '%')
      return false;
    if (c == '\\')
    {
      if (i + 1 < n && sync_is_letter(line[i + 1]))
      {
        i++;
        while (i < n && sync_is_letter(line[i]))
          i++;
        continue;
      }
      // Control symbols: \% \& \$ \_ \{ \} \# print their character
      if (i + 1 < n && i + 1 >= from && line[i + 1] < 128 &&
          strchr("%&$_{}#", line[i + 1]))
        return true;
      i += 2;
      continue;
    }
    if (i >= from && !(c > 0 && c < 128 && strchr(" \t{}$^_&~", c)))
      return true;
    i++;
  }
  return false;
}

// A line of a file (1-based) decoded into `line`, returning its length
// (0 if unknown)
static int sync_source_line(struct persistent_state *ps, ui_state *ui,
                            const char *path, int number, int *line)
{
  fz_buffer *data = sync_file_data(ps, ui, path);
  const char *p = data ? sync_line_start(data, number) : NULL;
  if (!p)
    return 0;
  const char *next;
  return sync_decode_line(data, p, line, &next);
}

// The end of the arguments of the macro whose name starts at `column`:
// the groups in brackets or braces right after its name.
static int sync_macro_end(const int *line, int n, int column)
{
  int i = sync_control_word_end(line, n, column);
  if (i < n && line[i] == '*')
    i++;
  while (i < n && (line[i] == '{' || line[i] == '['))
  {
    int close = line[i] == '{' ? '}' : ']', depth = 0;
    for (; i < n; i++)
    {
      if (line[i] == '\\')
        i++;
      else if (line[i] == '{')
        depth++;
      else if (line[i] == '}')
        depth--;
      if (depth == 0 && line[i] == close)
        break;
    }
    i++;
  }
  return i < n ? i : n;
}

// Whether the text before glyph g on its line in the displayed page has
// glyph sources (or g starts the line). Text without glyph sources (in an
// OpenType font, or typeset from another file, as by minted) can be what
// the cursor is on, instead of what the glyph stands for.
static bool sync_sourced_before(struct persistent_state *ps, ui_state *ui,
                                dvi_glyph_src g)
{
  fz_point center = fz_make_point((g.box.x0 + g.box.x1) / 2,
                                  (g.box.y0 + g.box.y1) / 2);
  fz_rect cb;
  if (!txp_renderer_char_before(ps->ctx, ui->doc_renderer, center, &cb))
    return true;
  fz_point c = fz_make_point((cb.x0 + cb.x1) / 2, (cb.y0 + cb.y1) / 2);
  const dvi_glyph_src *gs;
  int n = send(glyph_srcs, ui->eng, ps->ctx, ui->page, &gs);
  for (int i = 0; i < n; i++)
    if (fz_is_point_inside_rect(c, gs[i].box))
      return true;
  return false;
}

// Forward: the glyph at the editor cursor (ui->sync_target), looking on the
// page SyncTeX found and the pages around it. The caret goes before the
// first glyph of the column of the cursor, else after the last glyph of the
// nearest column before it (inside a ligature, between its characters); a
// cursor inside the name of a macro goes before the glyphs of the macro.
// Returns whether a glyph was found, with its page and the caret on its
// baseline. The caret is only a guess (*weak) when text without glyph
// sources can be at the cursor.
static bool sync_forward_by_glyphs(struct persistent_state *ps, ui_state *ui,
                                   synctex_t *stx, fz_buffer *buf, int page,
                                   int *out_page, fz_point *out, bool *weak)
{
  *weak = false;
  const char *path = ui->sync_target.path;
  int target = ui->sync_target.line, column = ui->sync_target.column;
  if (!path[0] || target <= 0 || !stx || !buf)
    return false;
  if (column < 0)
    column = 0;
  struct sync_tags tags = {0};

  // The glyphs of the nearest column at or before the cursor (the first and
  // the last one), the first glyph of the nearest column after it, and the
  // last glyph that ends at the cursor
  dvi_glyph_src first = {0}, last = {0}, next = {0}, ending = {0};
  int last_page = -1, next_page = -1, ending_page = -1;
  int pages = send(page_count, ui->eng);
  int order[3] = {page - 1, page, page + 1};
  for (int k = 0; k < 3; k++)
  {
    int pg = order[k];
    if (pg < 0 || pg >= pages)
      continue;
    const dvi_glyph_src *g;
    int n = send(glyph_srcs, ui->eng, ps->ctx, pg, &g);
    for (int i = 0; i < n; i++)
    {
      if (g[i].line != target ||
          !sync_tag_is(ps, stx, buf, &tags, g[i].tag, path))
        continue;
      if (getenv("TXP_SYNC_DEBUG_GLYPHS"))
        fprintf(stderr, "[synctex forward] glyph candidate: page %d #%d tag %d "
                "column %d length %d at (%.2f, %.2f)-(%.2f, %.2f)\n", pg, i,
                g[i].tag, g[i].column, g[i].length, g[i].origin.x,
                g[i].origin.y, g[i].end.x, g[i].end.y);
      int c = g[i].column;
      if (g[i].length > 0 && c + g[i].length == column)
      {
        ending = g[i];
        ending_page = pg;
      }
      if (c <= column)
      {
        if (last_page < 0 || c > last.column)
        {
          first = last = g[i];
          last_page = pg;
        }
        else if (c == last.column)
        {
          // All glyphs of a macro have its column: the last one ends it.
          // The first one that stands for characters (not a hyphen) starts
          // it.
          last = g[i];
          last_page = pg;
          if (first.length == 0 && g[i].length > 0)
            first = g[i];
        }
      }
      else if (next_page < 0 || c < next.column ||
               (c == next.column && next.length == 0 && g[i].length > 0))
      {
        next = g[i];
        next_page = pg;
      }
    }
  }

  // Text in between without glyph sources: leave it to text matching
  static int line[SYNC_TEXT_MAX];
  int n = sync_source_line(ps, ui, path, target, line);
  if (last_page >= 0 &&
      sync_prints_between(line, n, last.column + fz_maxi(last.length, 1), column))
    last_page = -1;
  if (next_page >= 0 && sync_prints_between(line, n, column, next.column))
    next_page = -1;

  fz_point pt;
  dvi_glyph_src g;
  int page_of_g;
  if (last_page >= 0)
  {
    int c = last.column;
    if (column == c && ending_page >= 0 && column < n && line[column] == '\\')
      // Between text and a macro (a footnote mark): after the text
      g = ending, pt = ending.end, last_page = ending_page;
    else if (column == c)
      g = first, pt = first.origin;
    else if (column < sync_control_word_end(line, n, c))
      // Inside the name of the macro: before its first glyph
      g = first, pt = first.origin;
    else if (column >= c + last.length)
    {
      g = last, pt = last.end;
      // At text without glyph sources, or in the arguments of a macro (the
      // text typeset from them has no glyph sources of its own)
      if (sync_prints_between(line, n, column, column + 1) ||
          (line[c] == '\\' && column < sync_macro_end(line, n, c)))
        *weak = true;
    }
    else
    {
      // Inside a ligature
      float t = (float)(column - c) / last.length;
      g = last;
      pt = fz_make_point(g.origin.x + t * (g.end.x - g.origin.x),
                         g.origin.y + t * (g.end.y - g.origin.y));
    }
    page_of_g = last_page;
    // The glyphs of a macro need not start what it typesets
    if (column < sync_control_word_end(line, n, c) && line[c] == '\\' &&
        pt.x == first.origin.x && pt.y == first.origin.y &&
        last_page == ui->page && !sync_sourced_before(ps, ui, first))
      *weak = true;
  }
  else if (next_page >= 0)
  {
    g = next;
    pt = next.origin;
    page_of_g = next_page;
    // At text without glyph sources or a space (after what comes before)
    if (sync_prints_between(line, n, column, column + 1) ||
        (column < n && (line[column] == ' ' || line[column] == '\t')) ||
        (next_page == ui->page && !sync_sourced_before(ps, ui, next)))
      *weak = true;
  }
  else
    return false;

  if (getenv("TXP_SYNC_DEBUG"))
    fprintf(stderr, "[synctex forward] glyph: page %d tag %d line %d column %d "
            "length %d for column %d%s\n", page_of_g, g.tag, g.line,
            g.column, g.length, column, *weak ? " (weak)" : "");
  *out_page = page_of_g;
  *out = pt;
  return true;
}

// Backward: the source position of the glyph of the displayed page nearest
// to pt (within a line of text vertically, 12pt horizontally), with the
// number of its characters before pt in *after (a point on the second half
// of a glyph is after it; a ligature splits evenly).
#define SYNC_GLYPH_REACH_X 12

static bool sync_backward_by_glyphs(struct persistent_state *ps, ui_state *ui,
                                    fz_point pt, int *tag, int *line,
                                    int *column, int *after)
{
  const dvi_glyph_src *g;
  int n = send(glyph_srcs, ui->eng, ps->ctx, ui->page, &g);
  int best = -1;
  float best_d = INFINITY;
  for (int i = 0; i < n; i++)
  {
    fz_rect b = g[i].box;
    float dx = 0, dy = 0;
    if (pt.x < b.x0)
      dx = b.x0 - pt.x;
    else if (pt.x > b.x1)
      dx = pt.x - b.x1;
    if (pt.y < b.y0)
      dy = b.y0 - pt.y;
    else if (pt.y > b.y1)
      dy = pt.y - b.y1;
    if (dy > 0 || dx > SYNC_GLYPH_REACH_X)
      continue;
    // Adjacent lines can overlap (with math): prefer the glyph whose
    // baseline is nearest
    float d = dx + fabsf(pt.y - g[i].origin.y) / 100;
    if (d < best_d)
    {
      best_d = d;
      best = i;
    }
  }
  if (best < 0)
    return false;

  // The character of the text nearest to pt must be this glyph (its origin
  // on the baseline of the glyph), not text without a glyph source next to
  // it or above it
  fz_rect cb, lb;
  fz_point o;
  if (txp_renderer_nearest_char(ps->ctx, ui->doc_renderer, pt, &cb, &lb, &o))
  {
    fz_point a = g[best].origin, b = g[best].end;
    float ux = b.x - a.x, uy = b.y - a.y, len2 = ux * ux + uy * uy;
    float t = len2 > 0 ? ((o.x - a.x) * ux + (o.y - a.y) * uy) / len2 : 0;
    t = fz_clamp(t, 0, 1);
    float dx = o.x - (a.x + t * ux), dy = o.y - (a.y + t * uy);
    if (dx * dx + dy * dy > 1)
      return false;
    // Where the glyph ends, the next character starts (without a glyph
    // source when it is text of an OpenType font)
    if ((1 - t) * (1 - t) * len2 < 0.25f)
      return false;
  }

  dvi_glyph_src gs = g[best];
  *tag = gs.tag;
  *line = gs.line;
  *column = gs.column;
  *after = 0;
  if (gs.length > 0)
  {
    // Position along the baseline, in characters
    float ux = gs.end.x - gs.origin.x, uy = gs.end.y - gs.origin.y;
    float len2 = ux * ux + uy * uy;
    float t = len2 > 0
      ? ((pt.x - gs.origin.x) * ux + (pt.y - gs.origin.y) * uy) / len2
      : 0;
    *after = (int)(fz_clamp(t, 0, 1) * gs.length + 0.5f);
  }
  return true;
}

// Backward SyncTeX from a point of the displayed page, in document units
static void sync_backward(struct persistent_state *ps, ui_state *ui, fz_point pt)
{
  fz_buffer *buf;
  synctex_t *stx = send(synctex, ui->eng, &buf);
  if (!stx || !buf)
    return;
  float f = 1 / send(scale_factor, ui->eng);
  fprintf(stderr, "click: (%f,%f) mapped:(%f,%f)\n",
          pt.x, pt.y, f * pt.x, f * pt.y);
  const char *name;
  int name_len, line, column, tag, after;
  bool by_glyph = sync_backward_by_glyphs(ps, ui, pt, &tag, &line, &column, &after);
  if (by_glyph)
  {
    name_len = synctex_input_name(stx, buf, tag, &name);
    by_glyph = name_len > 0;
  }
  if (!by_glyph &&
      !synctex_scan(ps->ctx, stx, buf, ui->page, f * pt.x, f * pt.y,
                    &name, &name_len, &line, &column))
    return;

  // Files of the document are known relative to its directory.
  char path[1024];
  snprintf(path, sizeof(path), "%.*s", name_len, name);
  const char *rel = path;
  int go_up = 0;
  if (rel[0] == '/')
    rel = relative_path(rel, ps->doc_path, &go_up);
  while (rel[0] == '.' && rel[1] == '/')
    rel += 2;
  if (by_glyph)
  {
    // A point after a glyph of a macro is still at the macro (its column is
    // the start of its name)
    static int src[SYNC_TEXT_MAX];
    int n = go_up == 0 ? sync_source_line(ps, ui, rel, line, src) : 0;
    if (getenv("TXP_SYNC_DEBUG"))
      fprintf(stderr, "[synctex backward] glyph source: column %d%s\n",
              column, after ? " (after)" : "");
    if (!(column < n && src[column] == '\\'))
      column += after;
    else
    {
      // Text typeset by the macro from its arguments, when they show on
      // the page: where the text is in them
      int tl = line, tc = column;
      if (sync_backward_by_text(ps, ui, pt, rel, &tl, &tc) && tl == line &&
          tc >= sync_control_word_end(src, n, column) &&
          tc <= sync_macro_end(src, n, column))
        column = tc;
    }
    if (getenv("TXP_SYNC_DEBUG"))
      fprintf(stderr, "[synctex backward] glyph: tag %d line %d column %d\n",
              tag, line, column);
  }
  else if (go_up == 0)
    sync_backward_by_text(ps, ui, pt, rel, &line, &column);

  // The editor gets a 1-based column, 0 if unknown.
  editor_synctex(ps->doc_path, name, name_len, line, column >= 0 ? column + 1 : 0);
}

// Link of the displayed page under a screen position (in pixels)
static fz_link *link_at(fz_context *ctx, ui_state *ui, fz_point p)
{
  if (ui->links_page != ui->page)
    return NULL;
  fz_point pt = txp_renderer_screen_to_document(ctx, ui->doc_renderer, p);
  for (fz_link *l = ui->links; l; l = l->next)
    if (fz_is_point_inside_rect(pt, fz_expand_rect(l->rect, 1)))
      return l;
  return NULL;
}

// Show a position of the current page in the upper part of the window and
// flash the marker there.
static void show_position(fz_context *ctx, ui_state *ui, fz_point p)
{
  ui->sync_mark.active = true;
  ui->sync_mark.page = ui->page;
  // p is the top-left corner of the target: put the caret on the first line.
  ui->sync_mark.pt = fz_make_point(p.x, p.y + 8);
  ui->sync_mark.box = fz_empty_rect;
  ui->sync_mark.no_caret = false;
  ui->sync_mark.ticks = SDL_GetTicks();

  int w, h;
  txp_renderer_screen_size(ctx, ui->doc_renderer, &w, &h);
  fz_point sp = txp_renderer_document_to_screen(ctx, ui->doc_renderer, p);
  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
  config->pan.y += h / 4.0 - sp.y;
  schedule_event(RENDER_EVENT);
}

static void go_to(struct persistent_state *ps, ui_state *ui, int page, fz_point pan)
{
  if (page != ui->page)
  {
    synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0, -1);
    ui->page = page;
    display_page(ps, ui);
  }
  txp_renderer_get_config(ps->ctx, ui->doc_renderer)->pan = pan;
  schedule_event(RENDER_EVENT);
}

static void follow_link(struct persistent_state *ps, ui_state *ui, fz_link *link)
{
  fz_context *ctx = ps->ctx;
  int page = -1;
  fz_point pt = fz_make_point(0, 0);
  bool found = false;

  fz_try(ctx)
    found = send(resolve_link, ui->eng, ctx, link->uri, &page, &pt);
  fz_catch(ctx)
    found = false;

  if (!found)
  {
    if (fz_is_external_link(ctx, link->uri))
    {
#if SDL_VERSION_ATLEAST(2, 0, 14)
      fprintf(stderr, "[link] opening %s\n", link->uri);
      if (SDL_OpenURL(link->uri) != 0)
        fprintf(stderr, "[link] cannot open %s: %s\n", link->uri, SDL_GetError());
#else
      fprintf(stderr, "[link] cannot open external links with this SDL: %s\n", link->uri);
#endif
    }
    else
      fprintf(stderr, "[link] destination not found: %s\n", link->uri);
    return;
  }

  if (page < 0 || page >= send(page_count, ui->eng))
    return;
  if (isnan(pt.x)) pt.x = 0;
  if (isnan(pt.y)) pt.y = 0;
  fprintf(stderr, "[link] %s: page %d, (%.02f, %.02f)\n",
          link->uri, page, pt.x, pt.y);

  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
  int n = sizeof(ui->history) / sizeof(ui->history[0]);
  if (ui->history_len == n)
  {
    memmove(&ui->history[0], &ui->history[1], sizeof(ui->history[0]) * (n - 1));
    ui->history_len -= 1;
  }
  ui->history[ui->history_len].page = ui->page;
  ui->history[ui->history_len].pan = config->pan;
  ui->history_len += 1;

  go_to(ps, ui, page, config->pan);
  show_position(ctx, ui, pt);
}

static void go_back(struct persistent_state *ps, ui_state *ui)
{
  if (ui->history_len == 0)
    return;
  ui->history_len -= 1;
  int page = ui->history[ui->history_len].page;
  if (page >= send(page_count, ui->eng))
    return;
  go_to(ps, ui, page, ui->history[ui->history_len].pan);
}

static void update_link_cursor(fz_context *ctx, ui_state *ui, int x, int y)
{
  static SDL_Cursor *hand, *arrow;
  fz_point scale = get_scale_factor(ui->window);
  bool over = link_at(ctx, ui, fz_make_point(scale.x * x, scale.y * y)) != NULL;
  if (over == ui->over_link)
    return;
  ui->over_link = over;
  if (!hand)
  {
    hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
  }
  SDL_SetCursor(over ? hand : arrow);
}

static void ui_mouse_down(struct persistent_state *ps, ui_state *ui, int x, int y, bool ctrl)
{
  fz_point link_pos = get_scale_factor(ui->window);
  link_pos.x *= x;
  link_pos.y *= y;
  fz_link *link = ctrl ? NULL : link_at(ps->ctx, ui, link_pos);
  if (link)
  {
    // Following a link replaces selection and backward sync
    ui->mouse_status = UI_MOUSE_NONE;
    follow_link(ps, ui, link);
    // The link list may have been replaced by the new page's
    update_link_cursor(ps->ctx, ui, x, y);
  }
  else if (ctrl)
    ui->mouse_status = UI_MOUSE_MOVE;
  else
  {
    ui->mouse_status = UI_MOUSE_SELECT;
    fz_point scale = get_scale_factor(ui->window);
    fz_point p = fz_make_point(scale.x * x, scale.y * y);

    uint32_t ticks = SDL_GetTicks();

    bool double_click = ticks - ui->last_click_ticks < 500 &&
                        abs(ui->last_mouse_x - x) < 30 && abs(ui->last_mouse_y - y) < 30;

    bool diff;

    if (double_click)
    {
      diff = txp_renderer_select_word(ps->ctx, ui->doc_renderer, p);
    }
    else
    {
      diff = txp_renderer_start_selection(ps->ctx, ui->doc_renderer, p);
      diff = txp_renderer_select_char(ps->ctx, ui->doc_renderer, p) || diff;
      ui->last_click_ticks = ticks;

      sync_backward(ps, ui,
                    txp_renderer_screen_to_document(ps->ctx, ui->doc_renderer, p));
    }

    if (diff)
      schedule_event(RENDER_EVENT);
  }

  ui->last_mouse_x = x;
  ui->last_mouse_y = y;
}

static void ui_mouse_up(ui_state *ui)
{
  ui->mouse_status = UI_MOUSE_NONE;
}

static void ui_mouse_move(fz_context *ctx, ui_state *ui, int x, int y)
{
  fz_point scale = get_scale_factor(ui->window);
  switch (ui->mouse_status)
  {
    case UI_MOUSE_NONE:
      update_link_cursor(ctx, ui, x, y);
      break;

    case UI_MOUSE_SELECT:
    {
      fz_point p = fz_make_point(scale.x * x, scale.y * y);
      // fprintf(stderr, "drag sel\n");
      if (txp_renderer_drag_selection(ctx, ui->doc_renderer, p))
        schedule_event(RENDER_EVENT);
      break;
    }

    case UI_MOUSE_MOVE:
    {
      txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
      int dx = x - ui->last_mouse_x;
      int dy = y - ui->last_mouse_y;
      if (dx != 0 || dy != 0)
      {
        config->pan.x += scale.x * dx;
        config->pan.y += scale.y * dy;
        ui->last_mouse_x = x;
        ui->last_mouse_y = y;
        schedule_event(RENDER_EVENT);
      }
      break;
    }
  }
}

// Document units panned per wheel unit. macOS trackpads and mice report
// small precise deltas, so this needs to be fairly large to feel responsive.
#define WHEEL_PAN_SPEED 20

// SDL_EVENT_PINCH_UPDATE of SDL 3.4 (trackpad pinch). SDL 2 has no pinch
// events, but sdl2-compat passes this one through, with the SDL 3 payload
// (the float scale change) after the SDL 2 event header.
#define SDL3_PINCH_UPDATE 0x711

static void previous_page(fz_context *ctx, ui_state *ui, bool pan);
static void next_page(fz_context *ctx, ui_state *ui, bool pan);

// Scroll by y pixels (> 0 towards the top of the page), stretching the spring
// at the ends of the page (see OVERSCROLL_FLIP). dt is the time since the
// previous scroll event, in milliseconds.
static void ui_wheel_pan_y(fz_context *ctx, ui_state *ui, float y,
                           enum txp_scroll_phase phase, float dt)
{
  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
  bool wheel = phase == TXP_SCROLL_WHEEL;
  bool momentum = phase == TXP_SCROLL_MOMENTUM;
  if (wheel)
  {
    uint32_t now = SDL_GetTicks();
    if (now - ui->overscroll.last_wheel > WHEEL_GESTURE_GAP_MS)
      ui->overscroll.scrolled = ui->overscroll.flipped = false;
    ui->overscroll.last_wheel = now;
    // The gesture holds the spring (unless it turned the page)
    if (!ui->overscroll.flipped)
      ui->overscroll.velocity = 0;
  }

  // The rest of a gesture that turned the page, and the momentum after the
  // page bounced, are dropped
  if (ui->overscroll.flipped || (momentum && ui->overscroll.bounced) || y == 0)
    return;

  txp_renderer_bounds bounds;
  if (!txp_renderer_page_bounds(ctx, ui->doc_renderer, &bounds))
  {
    config->pan.y += y;
    return;
  }

  // Scrolling back releases the spring first
  float t = ui->overscroll.tension;
  if (t != 0 && (t > 0) != (y > 0))
  {
    if (fabsf(y) < fabsf(t))
    {
      ui->overscroll.tension = t + y;
      return;
    }
    y += t;
    t = 0;
  }

  float range = fmaxf(bounds.pan_interval.y, 0);
  float pan = fz_clamp(config->pan.y, -range, range);
  float moved = fz_clamp(pan + y, -range, range);
  if (moved != pan)
    ui->overscroll.scrolled = true;
  config->pan.y = moved;
  float excess = pan + y - moved;
  if (momentum)
  {
    // The page bounces at the end, at the speed of the scroll
    if (excess != 0)
    {
      ui->overscroll.velocity += y / dt;
      ui->overscroll.bounced = true;
    }
  }
  else if (!wheel || !ui->overscroll.scrolled)
    t += excess;
  ui->overscroll.tension = t;

  float flip = (wheel ? OVERSCROLL_FLIP_WHEEL : OVERSCROLL_FLIP) *
               get_scale_factor(ui->window).y;
  if (momentum || fabsf(t) < flip)
    return;
  bool last = send(get_status, ui->eng) == DOC_TERMINATED &&
              ui->page + 1 >= send(page_count, ui->eng);
  if (t > 0 ? ui->page == 0 : last)
  {
    // No page to turn to
    ui->overscroll.tension = t > 0 ? flip : -flip;
    return;
  }
  if (t > 0)
    previous_page(ctx, ui, true);
  else
    next_page(ctx, ui, true);
  ui->overscroll.tension = t > 0 ? -flip : flip;
  ui->overscroll.velocity = 0;
  ui->overscroll.flipped = true;
}

// Change the zoom level by delta (see zoom_factor), keeping the point under
// the mouse in place
static void ui_zoom_at(fz_context *ctx, ui_state *ui, float delta, int mousex, int mousey)
{
  SDL_FRect rect;
  if (delta == 0 || !txp_renderer_page_position(ctx, ui->doc_renderer, &rect, NULL, NULL))
    return;

  fz_point scale = get_scale_factor(ui->window);
  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
  ui->zoom = fz_maxi(ui->zoom + delta, 0);
  int ww, wh;
  SDL_GetWindowSize(ui->window, &ww, &wh);
  float mx = (mousex - ww / 2.0f) * scale.x;
  float my = (mousey - wh / 2.0f) * scale.y;
  float of = config->zoom, nf = zoom_factor(ui->zoom);
  config->pan.x = mx + nf * ((config->pan.x - mx) / of);
  config->pan.y = my + nf * ((config->pan.y - my) / of);
  config->zoom = nf;
  schedule_event(RENDER_EVENT);
}

// Trackpad pinch: factor is the scale change since the previous update
static void ui_pinch(fz_context *ctx, ui_state *ui, float factor, int mousex, int mousey)
{
  if (ui->mouse_status != UI_MOUSE_NONE || !(factor > 0))
    return;
  ui_zoom_at(ctx, ui, roundf(5000.0f * logf(factor)), mousex, mousey);
}

static void ui_mouse_wheel(fz_context *ctx, ui_state *ui, float dx, float dy, int mousex, int mousey, bool ctrl, enum txp_scroll_phase phase, float dt)
{
  fz_point scale = get_scale_factor(ui->window);

  if (ui->mouse_status != UI_MOUSE_NONE)
    return;

  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);

  if (ctrl)
    ui_zoom_at(ctx, ui, dy * 100, mousex, mousey);
  else
  {
    float x = scale.x * dx * WHEEL_PAN_SPEED;
    float y = scale.y * dy * WHEEL_PAN_SPEED;
    config->pan.x -= x;
    ui_wheel_pan_y(ctx, ui, y, phase, dt);
    // fprintf(stderr, "wheel pan: (%.02f, %.02f) raw:(%.02f, %.02f)\n", x, y, dx, dy);
    schedule_event(RENDER_EVENT);
  }
}

// A scroll event with the phases of the gesture (see scroll.h)
static void ui_scroll(fz_context *ctx, ui_state *ui, const txp_scroll_event *ev)
{
  if (ev->phase == TXP_SCROLL_TOUCH)
  {
    // A new gesture: the fingers hold the spring where it is
    ui->overscroll.flipped = ui->overscroll.bounced = false;
    ui->overscroll.velocity = 0;
  }
  if (ev->phase == TXP_SCROLL_TOUCH || ev->phase == TXP_SCROLL_FINGERS)
    ui->overscroll.touching = true;

  float dt = fz_clamp(ev->ms - ui->overscroll.last_scroll, 4, 50);
  ui->overscroll.last_scroll = ev->ms;
  if (ev->dx != 0 || ev->dy != 0)
  {
    int mx = 0, my = 0;
    mouse_position_in_points(&mx, &my);
    bool ctrl = !!(SDL_GetModState() & KMOD_CTRL);
    ui_mouse_wheel(ctx, ui, ev->dx, ev->dy, mx, my, ctrl, ev->phase, dt);
  }

  if (ev->phase == TXP_SCROLL_RELEASE)
  {
    ui->overscroll.touching = false;
    schedule_event(RENDER_EVENT);
  }
}

/* Stdin polling */

// Write end of the poll thread pipe, for the termination signal handler.
static volatile int quit_signal_fd = -1;

// SDL turns SIGTERM into a quit event only when it next pumps events, and on
// macOS waiting for events does not return on a signal: stopping TeXpresso
// from an editor took until something else woke the window up. Wake the poll
// thread instead, which posts the quit event.
static void signal_quit(int sig)
{
  (void)sig;
  int fd = quit_signal_fd;
  if (fd != -1)
  {
    int saved_errno = errno;
    char c = 't';
    (void)!write(fd, &c, 1);
    errno = saved_errno;
  }
  // Otherwise TeXpresso is already quitting
}

static int SDLCALL poll_stdin_thread_main(void *data)
{
  int *pipes = data;
  char c;
  int n;

  while (1)
  {
    n = read(pipes[0], &c, 1);
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      return 1;
    }

    if (n == 0)
      return 1;

    if (c == 'q')
      return 0;

    if (c == 't')
    {
      SDL_Event quit;
      SDL_zero(quit);
      quit.type = SDL_QUIT;
      SDL_PushEvent(&quit);
      continue;
    }

    if (c != 'c')
      abort();

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLRDNORM;
    fds[0].revents = 0;
    fds[1].fd = pipes[0];
    fds[1].events = POLLRDNORM;
    fds[1].revents = 0;

    while (1)
    {
      n = poll(fds, 2, -1);
      if (n == -1)
      {
        if (errno == EINTR)
          continue;
        return 1;
      }
      if ((fds[0].revents & POLLRDNORM) != 0)
        schedule_event(STDIN_EVENT);
      break;
    }
  }
}

static bool poll_stdin(void)
{
  struct pollfd fd;
  fd.fd = STDIN_FILENO;
  fd.events = POLLRDNORM;
  fd.revents = 0;
  return (poll(&fd, 1, 0) == 1) && ((fd.revents & POLLRDNORM) != 0);
}

static void wakeup_poll_thread(int poll_stdin_pipe[2], char c)
{
  while (1)
  {
    int n = write(poll_stdin_pipe[1], &c, 1);
    if (n == 1)
      break;
    if (n == -1 && errno == EINTR)
      continue;
    perror("write(poll_stdin_pipe, _, _)");
    break;
  }
}

/* Command interpreter */

enum pan_to { PAN_TO_TOP, PAN_TO_BOTTOM };
static void pan_to(fz_context *ctx, ui_state *ui, enum pan_to to)
{
  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);
  txp_renderer_bounds bounds;
  if (txp_renderer_page_bounds(ctx, ui->doc_renderer, &bounds))
    config->pan.y = (to == PAN_TO_TOP) ? bounds.pan_interval.y : -bounds.pan_interval.y;
  // a helper function for other UI actions, so no event scheduled
}

static void previous_page(fz_context *ctx, ui_state *ui, bool pan)
{
  synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0, -1);
  if (ui->page > 0)
  {
    ui->page -= 1;

    int page_count = send(page_count, ui->eng);
    if (page_count > 0 && ui->page >= page_count &&
        send(get_status, ui->eng) == DOC_TERMINATED)
      ui->page = page_count - 1;

    // FIXME: technically, this is slightly incorrect.
    // The new page has not been loaded yet, so we compute the coordinate with
    // respect to the page currently displayed. Most of the time, pages have the
    // same dimension, so this is fine.
    if (pan)
      pan_to(ctx, ui, PAN_TO_BOTTOM);

    schedule_event(RELOAD_EVENT);
  }
}

static void next_page(fz_context *ctx, ui_state *ui, bool pan)
{
  synctex_set_target(send(synctex, ui->eng, NULL), 0, NULL, 0, -1);
  ui->page += 1;
  // FIXME: Same remark as in previous_page.
  if (pan)
    pan_to(ctx, ui, PAN_TO_TOP);
  schedule_event(RELOAD_EVENT);
}

static void ui_pan_x(fz_context *ctx, ui_state *ui, float factor)
{
  fz_point scale = get_scale_factor(ui->window);

  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);

  txp_renderer_bounds bounds;
  if (!txp_renderer_page_bounds(ctx, ui->doc_renderer, &bounds))
    return;

  float delta = bounds.window_size.x * scale.x * factor;

  config->pan.x += delta;
  schedule_event(RENDER_EVENT);
}

static void ui_pan(fz_context *ctx, ui_state *ui, float factor)
{
  fz_point scale = get_scale_factor(ui->window);

  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);

  txp_renderer_bounds bounds;
  if (!txp_renderer_page_bounds(ctx, ui->doc_renderer, &bounds))
    return;

  float delta = bounds.window_size.y * scale.y * factor;
  float range = bounds.pan_interval.y < 0 ? 0 : bounds.pan_interval.y;

  //fprintf(stderr, "ui_pan: factor:%.02f delta:%.02f current:%.02f range:%.02f\n",
  //        factor, delta, config->pan.y, range);

  if (config->pan.y == -range && factor < 0)
  {
    next_page(ctx, ui, 1);
    return;
  }

  if (config->pan.y == range && factor > 0)
  {
    previous_page(ctx, ui, 1);
    return;
  }

  config->pan.y += delta;
  schedule_event(RENDER_EVENT);
}

static const char *relative_path(const char *path, const char *dir, int *go_up)
{
  const char *rel_path = path, *dir_path = dir;

  // Skip common parts
  while (*rel_path && *rel_path == *dir_path)
  {
    if (*rel_path == '/')
    {
      while (*rel_path == '/') rel_path += 1;
      while (*dir_path == '/') dir_path += 1;
    }
    else
    {
      rel_path += 1;
      dir_path += 1;
    }
  }

  // Go back to last directory separator
  if (*rel_path && *dir_path)
  {
    rel_path -= 1;
    dir_path -= 1;
    while (path < rel_path && *rel_path != '/')
    {
      rel_path -= 1;
      dir_path -= 1;
    }
    if (*rel_path == '/')
    {
      if (*dir_path != '/') abort();
      rel_path += 1;
      dir_path += 1;
    }
  }

  // Count number of '../'
  *go_up = 0;
  if (*dir_path)
  {
    *go_up = 1;
    while (*dir_path)
    {
      if (*dir_path == '/')
      {
        *go_up += 1;
        while (*dir_path == '/')
          dir_path += 1;
      }
      else
        dir_path += 1;
    }
  }

  while (*rel_path == '/')
    rel_path += 1;

  return rel_path;
}

static int find_diff(const fz_buffer *buf, const void *data, int size)
{
  const unsigned char *ptr = data;
  int i, len = fz_mini(buf->len, size);
  for (i = 0; i < len && buf->data[i] == ptr[i]; ++i);
  fprintf(stderr, "i:%d len:%d size:%d\n", i, (int)buf->len, size);
  return i;
}

#include "utf_mapping.h"

static void realize_change(struct persistent_state *ps,
                           ui_state *ui,
                           struct editor_change *op)
{
  int go_up = 0;
  const char *path = relative_path(op->path, ps->doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] change %s: file has a different root, skipping\n", path);
    return;
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] change %s: file not found, skipping\n", path);
    return;
  }

  fz_buffer *b = e->edit_data;
  if (!b)
  {
    fprintf(stderr, "[command] change %s: file not opened, skipping\n", path);
    return;
  }

  int offset = op->span.offset, remove = op->span.remove, length = op->length;

  if (op->base == BASE_LINE)
  {
    // Compute byte offsets from line offsets
    int line = offset, count = remove;

    offset = remove = 0;

    uint8_t *p = b->data;
    size_t len = b->len;

    while (line > 0 && offset < len)
    {
      if (p[offset] == '\n')
        line -= 1;
      offset++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change line %s: invalid line number, skipping\n", path);
      return;
    }

    remove = offset;
    while (count > 0 && remove < len)
    {
      if (p[remove] == '\n')
        count -= 1;
      remove++;
    }

    if (count > 1)
    {
      fprintf(stderr, "[command] change line %s: invalid line count, skipping\n", path);
      return;
    }

    remove -= offset;
  }
  else if (op->base == BASE_RANGE)
  {
    // Compute byte offsets from line offsets
    int line = op->range.start_line;
    offset = remove = 0;

    uint8_t *p = b->data;
    size_t len = b->len;

    while (line > 0 && offset < len)
    {
      if (p[offset] == '\n')
        line -= 1;
      offset++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change range %s: invalid start line, skipping\n", path);
      return;
    }

    int start_char_offset = utf16_to_utf8_offset(p + offset, p + len, op->range.start_char);
    if (start_char_offset == -1)
    {
      fprintf(stderr, "[command] change range %s: invalid start char, skipping\n", path);
      return;
    }

    remove = offset;
    offset += start_char_offset;

    line = op->range.end_line - op->range.start_line;
    if (line < 0)
    {
      fprintf(stderr, "[command] change range %s: invalid end line, skipping\n", path);
      return;
    }

    while (line > 0 && remove < len)
    {
      if (p[remove] == '\n')
        line -= 1;
      remove++;
    }

    if (line > 0)
    {
      fprintf(stderr, "[command] change range %s: invalid end line, skipping\n", path);
      return;
    }

    int end_char_offset = utf16_to_utf8_offset(p + remove, p + len, op->range.end_char);
    if (end_char_offset == -1)
    {
      fprintf(stderr, "[command] change range %s: invalid end char, skipping\n", path);
      return;
    }

    remove += end_char_offset;
    remove -= offset;
  }

  if (remove < 0 || offset < 0 || offset + remove > b->len)
  {
    fprintf(stderr, "[command] change %s: invalid range, skipping\n", path);
    return;
  }

  if (b->len - remove + length > b->cap)
    fz_resize_buffer(ps->ctx, b, b->len - remove + length + 128);

  memmove(b->data + offset + length, b->data + offset + remove,
          b->len - offset - remove);

  b->len = b->len - remove + length;

  memmove(b->data + offset, op->data, length);

  fprintf(stderr, "[command] change %s: changed offset %d\n", path, offset);
  send(notify_file_changes, ui->eng, ps->ctx, e, offset);
}

#define BUFFERED_OPS 64
#define BUFFERED_CHARS 4096
#define T_IDLE_MS 500
#define MAX_RERUNS 5

struct {
  char buffer[BUFFERED_CHARS];
  int cursor;
  struct editor_change op[BUFFERED_OPS];
  int count;
} delayed_changes = {0,};

static void flush_changes(struct persistent_state *ps,
                          ui_state *ui)
{
  int count = delayed_changes.count;
  if (count)
  {
    delayed_changes.count = 0;
    delayed_changes.cursor = 0;
    for (int i = 0; i < count; ++i)
    {
      struct editor_change *op = &delayed_changes.op[i];
      realize_change(ps, ui, op);
    }
  }
}

static void interpret_change(struct persistent_state *ps,
                             ui_state *ui,
                             struct editor_change *op)
{
  int plen = strlen(op->path);
  int page_count = send(page_count, ui->eng);
  int cursor = delayed_changes.cursor;

  if ((page_count == ui->page - 2 || page_count == ui->page - 1) &&
      send(get_status, ui->eng) == DOC_RUNNING &&
      delayed_changes.count < BUFFERED_OPS &&
      cursor + plen + 1 + op->length <= BUFFERED_CHARS)
  {
    char *op_path = delayed_changes.buffer + cursor;
    memcpy(op_path, op->path, plen + 1);
    cursor += plen + 1;
    char *op_data = delayed_changes.buffer + cursor;
    memcpy(op_data, op->data, op->length);
    cursor += op->length;
    delayed_changes.cursor = cursor;

    delayed_changes.op[delayed_changes.count] = *op;
    delayed_changes.op[delayed_changes.count].path = op_path;
    delayed_changes.op[delayed_changes.count].data = op_data;
    delayed_changes.count += 1;
  }
  else
  {
    flush_changes(ps, ui);
    realize_change(ps, ui, op);
  }
}

static void interpret_open(struct persistent_state *ps,
                           ui_state *ui,
                           const char *path,
                           const void *data,
                           int size)
{
  if (path[0] == '/')
  {
    int go_up = 0;
    path = relative_path(path, ps->doc_path, &go_up);
    if (go_up > 0)
    {
      fprintf(stderr, "[command] open %s: file has a different root, skipping\n", path);
      return;
    }
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] open %s: file not found, skipping\n", path);
    return;
  }

  flush_changes(ps, ui);

  int changed = -1;
  bool had_edit_data = (e->edit_data != NULL);

  if (e->edit_data)
  {
    fprintf(stderr, "[command] open %s: known file, updating\n", path);
    changed = find_diff(e->edit_data, data, size);
    if (e->edit_data->cap < size)
      fz_resize_buffer(ps->ctx, e->edit_data, size + 128);
    e->edit_data->len = size;
    memcpy(e->edit_data->data, data, size);
  }
  else
  {
    fprintf(stderr, "[command] open %s: new file\n", path);
    e->edit_data = fz_new_buffer_from_copied_data(ps->ctx, data, size);
    if (e->fs_data)
      changed = find_diff(e->fs_data, data, size);
    else if (e->seen >= 0)
      changed = 0;
  }

  if (changed >= 0)
  {
    if (e->promised && !had_edit_data)
      fprintf(stderr, "[command] open %s: resolving deferred query\n", path);
    else
    {
      fprintf(stderr, "[command] open %s: changed offset is %d\n", path, changed);
      send(notify_file_changes, ui->eng, ps->ctx, e, changed);
    }
  }
}

static void interpret_close(struct persistent_state *ps,
                            ui_state *ui,
                            const char *path)
{
  int go_up = 0;
  path = relative_path(path, ps->doc_path, &go_up);
  if (go_up > 0)
  {
    fprintf(stderr, "[command] close %s: file has a different root, skipping\n", path);
    return;
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] close %s: file not found, skipping\n", path);
    return;
  }

  if (!e->edit_data)
  {
    fprintf(stderr, "[command] close %s: file not opened, skipping\n", path);
    return;
  }

  flush_changes(ps, ui);

  int changed = 0;

  if (e->fs_data)
    changed = find_diff(e->fs_data, e->edit_data->data, e->edit_data->len);

  fz_drop_buffer(ps->ctx, e->edit_data);
  e->edit_data = NULL;

  fprintf(stderr, "[command] close %s: closing, changed offset %d\n", path,
          changed);

  send(notify_file_changes, ui->eng, ps->ctx, e, changed);
}

static uint32_t convert_color(fz_context *ctx, vstack *stack, float frgb[3])
{
  uint8_t rgb[3];

  for (int i = 0; i < 3; ++i)
    rgb[i] = fz_clampi(frgb[i] * 255.0, 0, 255) & 0xFF;

  return (rgb[0] << 16) | (rgb[1] << 8) | (rgb[2]);
}

static void display_page(struct persistent_state *ps, ui_state *ui)
{
  fz_display_list *dl = send(render_page, ui->eng, ps->ctx, ui->page);
  txp_renderer_set_contents(ps->ctx, ui->doc_renderer, dl);
  fz_drop_display_list(ps->ctx, dl);

  fz_drop_link(ps->ctx, ui->links);
  ui->links = NULL;
  ui->links_page = ui->page;
  fz_try(ps->ctx)
    ui->links = send(load_links, ui->eng, ps->ctx, ui->page);
  fz_catch(ps->ctx)
  {
    fprintf(stderr, "[link] cannot load links: %s\n", fz_caught_message(ps->ctx));
    ui->links_page = -1;
  }

  schedule_event(RENDER_EVENT);
}

#if !SDL_VERSION_ATLEAST(2, 0, 16)
static void
SDL_SetWindowAlwaysOnTop(SDL_Window *window, SDL_bool state)
{
  (void)window;
  (void)state;
  fprintf(stderr, "[info] stay-on-top feature is not available with "
                  "SDL older than 2.16.0\n");
}
#endif


static void interpret_register(struct persistent_state *ps,
                               ui_state *ui,
                               const char *path)
{
  if (path[0] == '/')
  {
    int go_up = 0;
    path = relative_path(path, ps->doc_path, &go_up);
    if (go_up > 0)
    {
      fprintf(stderr, "[command] register %s: file has a different root, skipping\n", path);
      return;
    }
  }

  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  if (!e)
  {
    fprintf(stderr, "[command] register %s: file not found, skipping\n", path);
    return;
  }

  e->promised = true;
  fprintf(stderr, "[command] register %s: marked as promised\n", path);
}

static void interpret_command(struct persistent_state *ps,
                              ui_state *ui,
                              vstack *stack,
                              val command)
{
  struct editor_command cmd;
  if (!editor_parse(ps->ctx, stack, command, &cmd))
    return;

  switch (cmd.tag)
  {
    case EDIT_OPEN:
      if (cmd.open.base64)
      {
        unsigned char *buf = malloc(cmd.open.length);
        if (!buf) break;
        memcpy(buf, cmd.open.data, cmd.open.length);
        int decoded_len = base64_decode(buf, cmd.open.length);
        if (decoded_len < 0)
        {
          fprintf(stderr, "[command] open-base64: invalid base64 data\n");
          free(buf);
          break;
        }
        interpret_open(ps, ui, cmd.open.path, (const char *)buf, decoded_len);
        free(buf);
      }
      else
        interpret_open(ps, ui, cmd.open.path, cmd.open.data, cmd.open.length);
      break;

    case EDIT_CLOSE:
      interpret_close(ps, ui, cmd.close.path);
      break;

    case EDIT_CHANGE:
      interpret_change(ps, ui, &cmd.change);
      break;

    case EDIT_THEME:
    {
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->background_color = convert_color(ps->ctx, stack, cmd.theme.bg);
      config->foreground_color = convert_color(ps->ctx, stack, cmd.theme.fg);
      config->themed_color = 1;
      schedule_event(RENDER_EVENT);
      fprintf(stderr, "[command] theme %x %x\n",
              config->background_color, config->foreground_color);
    }
    break;

    case EDIT_PREVIOUS_PAGE:
      previous_page(ps->ctx, ui, 0);
      break;

    case EDIT_NEXT_PAGE:
      next_page(ps->ctx, ui, 0);
      break;

    case EDIT_MOVE_WINDOW:
    {
      float x = cmd.move_window.x, y = cmd.move_window.y,
            w = cmd.move_window.w, h = cmd.move_window.h;
      int x0 = x, y0 = y;
      SDL_SetWindowPosition(ui->window, x, y);
      SDL_GetWindowPosition(ui->window, &x0, &y0);
      SDL_SetWindowSize(ui->window, w + x - x0, h + y - y0);
      fprintf(stderr, "[command] move-window %f %f %f %f (pos: %d %d)\n",
              x, y, w, h, x0, y0);
    }
    break;

    case EDIT_MAP_WINDOW:
    {
      float x = cmd.move_window.x, y = cmd.move_window.y,
            w = cmd.move_window.w, h = cmd.move_window.h;
      int x0 = x, y0 = y;
      SDL_SetWindowBordered(ui->window, SDL_FALSE);
      SDL_SetWindowAlwaysOnTop(ui->window, SDL_TRUE);
      SDL_SetWindowPosition(ui->window, x, y);
      SDL_GetWindowPosition(ui->window, &x0, &y0);
      SDL_SetWindowSize(ui->window, w + x - x0, h + y - y0);
      fprintf(stderr, "[command] map-window %f %f %f %f (pos: %d %d)\n",
              x, y, w, h, x0, y0);
    }
    break;

    case EDIT_UNMAP_WINDOW:
    {
      if (!(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_INPUT_FOCUS))
        SDL_SetWindowBordered(ui->window, SDL_TRUE);
      SDL_SetWindowAlwaysOnTop(ui->window, SDL_FALSE);
      fprintf(stderr, "[command] unmap-window\n");
    }
    break;

    case EDIT_RESCAN:
      schedule_event(SCAN_EVENT);
      break;

    case EDIT_STAY_ON_TOP:
      SDL_SetWindowAlwaysOnTop(ui->window, cmd.stay_on_top.status);
      fprintf(stderr, "[command] stay-on-top %d\n", cmd.stay_on_top.status);
      break;

    case EDIT_TEST_CLICK:
    case EDIT_TEST_PAGE_TEXT:
    {
      int page = cmd.tag == EDIT_TEST_CLICK ? cmd.test_click.page
                                            : cmd.test_page_text.page;
      if (page < 0 || page >= send(page_count, ui->eng))
      {
        fprintf(stderr, "[test] page %d is not available\n", page);
        break;
      }
      if (page != ui->page)
      {
        ui->page = page;
        display_page(ps, ui);
      }
      // Replies go to stdout, the [test] markers to stderr: flush stdout
      // before each marker so that a reader sees the reply first.
      if (cmd.tag == EDIT_TEST_CLICK)
      {
        sync_backward(ps, ui, fz_make_point(cmd.test_click.x, cmd.test_click.y));
        fflush(stdout);
        fprintf(stderr, "[test] click done\n");
      }
      else
      {
        FILE *f = fopen(cmd.test_page_text.path, "w");
        if (!f)
          fprintf(stderr, "[test] cannot write %s\n", cmd.test_page_text.path);
        else
        {
          txp_renderer_dump_text(ps->ctx, ui->doc_renderer, f);
          fclose(f);
          fprintf(stderr, "[test] page text written\n");
        }
      }
      fflush(stdout);
    }
    break;

    case EDIT_TEST_WHEEL:
    {
      txp_scroll_event ev = {
          .phase = cmd.test_wheel.phase,
          .dy = cmd.test_wheel.dy,
          .ms = SDL_GetTicks(),
      };
      if (ev.phase == TXP_SCROLL_WHEEL)
        ui_mouse_wheel(ps->ctx, ui, 0, ev.dy, 0, 0, false, ev.phase,
                       OVERSCROLL_FRAME_MS);
      else
        ui_scroll(ps->ctx, ui, &ev);
      txp_renderer_config *config =
        txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      fprintf(stderr, "[test] wheel: page %d pan %.2f tension %.2f velocity %.2f%s%s\n",
              ui->page, config->pan.y, ui->overscroll.tension,
              ui->overscroll.velocity,
              ui->overscroll.flipped ? " flipped" : "",
              ui->overscroll.bounced ? " bounced" : "");
    }
    break;

    case EDIT_SYNCTEX_FORWARD:
    {
      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      int go_up = 0;
      const char *path = relative_path(cmd.synctex_forward.path, ps->doc_path, &go_up);
      if (go_up > 0)
      {
        fprintf(stderr,
                "[command] synctex-forward %s: file has a different root, skipping\n",
                path);
      }
      else
      {
        synctex_set_target(stx, ui->page, path, cmd.synctex_forward.line,
                           cmd.synctex_forward.column);
        snprintf(ui->sync_target.path, sizeof(ui->sync_target.path), "%s", path);
        ui->sync_target.line = cmd.synctex_forward.line;
        ui->sync_target.column = cmd.synctex_forward.column;
        schedule_event(STDIN_EVENT);
      }
    }
    break;

    case EDIT_CROP:
    {
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->crop = !config->crop;
      schedule_event(RENDER_EVENT);
    }
    break;
    case EDIT_INVERT:
    {
      txp_renderer_config *config =
          txp_renderer_get_config(ps->ctx, ui->doc_renderer);
      config->invert_color = !config->invert_color;
      schedule_event(RENDER_EVENT);
    }
    break;

    case EDIT_REGISTER:
      interpret_register(ps, ui, cmd.reg.path);
      break;

    case EDIT_PAUSE:
      ps->paused = true;
      fprintf(stderr, "[command] pause: engine stepping suspended\n");
      break;

    case EDIT_RESUME:
      ps->paused = false;
      fprintf(stderr, "[command] resume: engine stepping enabled\n");
      // Spawn the worker if -stream started paused (no-op otherwise)
      send(step, ui->eng, ps->ctx, true);
      schedule_event(SCAN_EVENT);
      break;

    case EDIT_RERUN:
      ps->rerun_enabled = cmd.rerun.status;
      fprintf(stderr, "[command] rerun %s\n",
              ps->rerun_enabled ? "enabled" : "disabled");
      break;

    case EDIT_RERUN_ONCE:
      ps->rerun_once_pending = true;
      fprintf(stderr, "[command] rerun-once: pending immediate pass\n");
      schedule_event(SCAN_EVENT);
      break;
  }
}

static void sync_fullscreen_state(struct fullscreen_state *fs,
                                  txp_renderer_config *config,
                                  SDL_Window *win)
{
  bool cur_fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
  if (cur_fs == fs->prev_fs) return;

  if (cur_fs) {
    if (!fs->has_backup) {
      fs->windowed_backup.crop = config->crop;
      fs->windowed_backup.fit  = config->fit;
      fs->windowed_backup.zoom = config->zoom;
      fs->has_backup = true;
    }
    config->crop = false;
    config->fit  = FIT_PAGE;
    config->zoom = 1.0;
  } else if (fs->has_backup) {
    config->crop = fs->windowed_backup.crop;
    config->fit  = fs->windowed_backup.fit;
    config->zoom = fs->windowed_backup.zoom;
    fs->has_backup = false;
  }
  fs->prev_fs = cur_fs;
}

/* Entry point */

bool texpresso_main(struct persistent_state *ps)
{
  editor_set_protocol(ps->protocol);
  editor_set_line_output(ps->line_output);
  pstate = ps;

  struct fullscreen_state fs = {
    .has_backup = false,
    .prev_fs = (SDL_GetWindowFlags(ps->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0,
  };

  ui_state raw_ui, *ui = &raw_ui;

  ui->window = ps->window;
  ui->links = NULL;
  ui->links_page = -1;
  ui->over_link = false;
  ui->history_len = 0;

  bool using_texlive = 0;

  if (ps->use_texlive)
  {
    if (!texlive_available())
    {
      fprintf(stderr,
              "[fatal] cannot find kpsewhich command for texlive integration "
              "(please make sure it is installed and visible in PATH)\n");
      return 0;
    }
    using_texlive = 1;
  }
  else if (ps->use_tectonic)
  {
    if (!tectonic_available())
    {
      fprintf(stderr,
              "[fatal] cannot find tectonic "
              "(please make sure it is installed and visible in PATH)\n");
      return 0;
    }
  }
  else if (!(using_texlive = texlive_available()) && !tectonic_available())
  {
    fprintf(stderr,
            "[fatal] cannot find tectonic nor kpsewhich (texlive)"
            "(please make sure at least one of them is installed and visible in PATH)\n");
    return 0;
  }

  const char *doc_ext = NULL;

  for (const char *ptr = ps->doc_name; *ptr; ptr++)
    if (*ptr == '.')
      doc_ext = ptr + 1;

  char engine_path[4096];
  find_engine(engine_path, ps->exe_path);
  fprintf(stderr, "[info] engine path: %s\n", engine_path);

  if (doc_ext && strcmp(doc_ext, "pdf") == 0)
    ui->eng = txp_create_pdf_engine(ps->ctx, ps->doc_name);
  else
  {
    dvi_reshooks hooks;
    if (using_texlive)
      hooks = dvi_texlive_hooks(ps->ctx, ps->doc_path);
    else
      hooks = dvi_tectonic_hooks(ps->ctx, ps->doc_path);

    if (doc_ext && (strcmp(doc_ext, "dvi") == 0 || strcmp(doc_ext, "xdv") == 0))
      ui->eng = txp_create_dvi_engine(ps->ctx, ps->doc_name, hooks);
    else
      ui->eng = txp_create_tex_engine(ps->ctx, engine_path, using_texlive,
                                      ps->stream_mode, ps->inclusion_path,
                                      ps->doc_name, hooks);
  }

  ui->sdl_renderer = ps->renderer;
  ui->doc_renderer = txp_renderer_new(ps->ctx, ui->sdl_renderer);

  if (ps->initial.initialized)
  {
    ui->page = ps->initial.page;
    ui->zoom = ps->initial.zoom;
    ui->need_synctex = ps->initial.need_synctex;
    *txp_renderer_get_config(ps->ctx, ui->doc_renderer) = ps->initial.config;
    txp_renderer_set_contents(ps->ctx, ui->doc_renderer,
                              ps->initial.display_list);
    editor_reset_sync();
  }
  else
  {
    ui->page = 0;
    ui->zoom = 0;
    ui->need_synctex = 1;
  }

  ui->mouse_status = UI_MOUSE_NONE;
  ui->last_mouse_x = -1000;
  ui->last_mouse_y = -1000;
  ui->last_click_ticks = SDL_GetTicks() - 200000000;
  ui->overscroll.tension = ui->overscroll.velocity = 0;
  ui->overscroll.last_wheel = SDL_GetTicks() - 200000000;
  ui->overscroll.last_scroll = 0;
  ui->overscroll.touching = ui->overscroll.scrolled = false;
  ui->overscroll.flipped = ui->overscroll.bounced = false;
  ui->last_frame = SDL_GetTicks();
  txp_renderer_get_config(ps->ctx, ui->doc_renderer)->overscroll = 0;

  bool quit = 0, reload = 0;
  if (!ps->paused)
    send(step, ui->eng, ps->ctx, true);
  render(ps->ctx, ui);
  schedule_event(RELOAD_EVENT);

  struct repaint_on_resize_env repaint_on_resize_env = {.ctx = ps->ctx, .ui = ui};
  SDL_AddEventWatch(repaint_on_resize, &repaint_on_resize_env);
  txp_scroll_start(scroll_wakeup);

  vstack *cmd_stack = vstack_new(ps->ctx);
  prot_parser cmd_parser;
  prot_initialize(&cmd_parser, (ps->protocol == EDITOR_JSON));

  // Start watching stdin
  int poll_stdin_pipe[2];
  if (pipe(poll_stdin_pipe) == -1)
  {
    perror("pipe");
    abort();
  }

  SDL_Thread *poll_stdin_thread =
    SDL_CreateThread(poll_stdin_thread_main, "poll_stdin_thread", poll_stdin_pipe);

  quit_signal_fd = poll_stdin_pipe[1];
  signal(SIGTERM, signal_quit);
  signal(SIGINT, signal_quit);
  signal(SIGHUP, signal_quit);
  bool stdin_eof = 0;
  int rerun_count = 0;

  while (!quit)
  {
    SDL_Event e;
    bool has_event = SDL_PollEvent(&e);

    // Process stdin
    send(begin_changes, ui->eng, ps->ctx);
    char buffer[4096];
    int n = -1;
    while (!stdin_eof && poll_stdin() && (n = read(STDIN_FILENO, buffer, 4096)) != 0)
    {
      if (n == -1)
      {
        if (errno == EINTR)
          continue;
        perror("poll stdin");
        break;
      }

      fprintf(stderr, "stdin: %.*s\n", n, buffer);

      const char *ptr = buffer, *lim = buffer + n;
      fz_try(ps->ctx)
      {
        while ((ptr = prot_parse(ps->ctx, &cmd_parser, cmd_stack, ptr, lim)))
        {
          val cmds = vstack_get_values(ps->ctx, cmd_stack);
          int n_cmds = val_array_length(ps->ctx, cmd_stack, cmds);
          for (int i = 0; i < n_cmds; i++)
          {
            val cmd = val_array_get(ps->ctx, cmd_stack, cmds, i);
            interpret_command(ps, ui, cmd_stack, cmd);
          }
        }
      }
      fz_catch(ps->ctx)
      {
        fprintf(stderr, "error while reading stdin commands: %s\n",
                fz_caught_message(ps->ctx));
        vstack_reset(ps->ctx, cmd_stack);
        prot_reinitialize(&cmd_parser);
      }
    }
    if (n == 0) stdin_eof = 1;

    if (send(end_changes, ui->eng, ps->ctx))
    {
      if (!ps->paused)
        send(step, ui->eng, ps->ctx, true);
      schedule_event(RELOAD_EVENT);
      rerun_count = 0;
    }

    // Process document
    {
      int before_page_count = send(page_count, ui->eng);
      bool advance = !ps->paused && advance_engine(ps->ctx, ui);
      send(finish_convergence, ui->eng, ps->ctx);
      int after_page_count = send(page_count, ui->eng);
      fflush(stdout);

      if (ui->page >= before_page_count && ui->page < after_page_count)
        schedule_event(RELOAD_EVENT);

      // Fire an on-demand rerun as soon as the engine is ready, regardless of
      // idle state. Runs in the main body of the loop (not inside the idle
      // wait) so it triggers even during active compilation cycles.
      bool aux_ready = !send(is_finishing, ui->eng)
                       && send(aux_dirty, ui->eng);
      if (ps->rerun_once_pending && aux_ready)
      {
        ps->rerun_once_pending = false;
        fprintf(stderr, "[rerun] on-demand: finishing pass\n");
        send(start_finishing, ui->eng);
        schedule_event(RELOAD_EVENT);
        continue;
      }

      if (!has_event)
      {
        uint32_t frame_ms = animation_frame_ms(ui);
        uint32_t since = SDL_GetTicks() - ui->last_frame;
        bool animating = frame_ms != 0;
        if (advance)
        {
          // Keep animations going while the engine works (on a page that
          // was just turned to)
          if (animating && since >= frame_ms)
            render(ps->ctx, ui);
          continue;
        }
        if (!stdin_eof)
          wakeup_poll_thread(poll_stdin_pipe, 'c');

        bool rerun_eligible = ps->rerun_enabled
                              && rerun_count < MAX_RERUNS
                              && aux_ready;
        if (animating)
          has_event = SDL_WaitEventTimeout(
              &e, since >= frame_ms ? 1 : frame_ms - since);
        else if (rerun_eligible)
          has_event = SDL_WaitEventTimeout(&e, T_IDLE_MS);
        else
          has_event = SDL_WaitEvent(&e);
        if (!has_event && animating)
        {
          // Next frame of the animations
          render(ps->ctx, ui);
          continue;
        }
        if (!has_event)
        {
          if (rerun_eligible)
          {
            rerun_count++;
            fprintf(stderr, "[rerun] idle %dms: finishing pass %d/%d\n",
                    T_IDLE_MS, rerun_count, MAX_RERUNS);
            send(start_finishing, ui->eng);
            schedule_event(RELOAD_EVENT);
            continue;
          }
          fprintf(stderr, "SDL_WaitEvent error: %s\n", SDL_GetError());
          break;
        }
      }

      fz_buffer *buf;
      synctex_t *stx = send(synctex, ui->eng, &buf);
      int page = -1, x = -1, y = -1;
      fz_irect box = fz_empty_irect;
      if (synctex_find_target(ps->ctx, stx, buf, &page, &x, &y, &box))
      {
        fprintf(stderr, "[synctex forward] sync: hit page %d, coordinates (%d, %d), "
                "line box (%d, %d)-(%d, %d)\n",
                page, x, y, box.x0, box.y0, box.x1, box.y1);

        if (page != ui->page &&
            page >= 0 && page < send(page_count, ui->eng))
        {
          ui->page = page;
          display_page(ps, ui);
        }

        // FIXME: Scroll to point
        float f = send(scale_factor, ui->eng);
        fz_point p = fz_make_point(f * x, f * y);
        fz_rect mark_box = fz_is_empty_irect(box)
          ? fz_empty_rect
          : fz_make_rect(f * box.x0, f * box.y0, f * box.x1, f * box.y1);
        bool no_caret = false;

        int precision = synctex_candidate_imprecise(stx);
        int glyph_page;
        fz_point gp;
        bool weak;
        bool by_glyph = sync_forward_by_glyphs(ps, ui, stx, buf, page,
                                               &glyph_page, &gp, &weak);
        fz_point tp;
        fz_rect tl;
        int max_len;
        if (by_glyph && weak && ui->page != glyph_page)
        {
          ui->page = glyph_page;
          display_page(ps, ui);
        }
        if (by_glyph && weak &&
            sync_refine_by_text(ps, ui, gp, fz_empty_rect, false, true, 0,
                                &max_len, &tp, &tl))
        {
          // Text matching around a guess from the glyphs
          fprintf(stderr, "[synctex forward] refined by text: (%.02f, %.02f)\n",
                  tp.x, tp.y);
          page = glyph_page;
          p = tp;
          mark_box = tl;
          precision = 0;
        }
        else if (by_glyph)
        {
          page = glyph_page;
          if (ui->page != page)
          {
            ui->page = page;
            display_page(ps, ui);
          }
          p = gp;
          // The line of text holding the glyph
          fz_rect cb;
          if (!txp_renderer_nearest_char(ps->ctx, ui->doc_renderer, p, &cb, &mark_box, NULL) ||
              !(mark_box.y0 - 2 <= p.y && p.y <= mark_box.y1 + 2))
            mark_box = fz_empty_rect;
          precision = 0;
        }
        else if (page == ui->page)
        {
          bool floating = precision & SYNCTEX_FLOATING;
          // Only trust matches inside the picture when the target line
          // itself was found there.
          bool in_picture = floating && !(precision & SYNCTEX_OTHER_LINE);
          fz_point tp;
          fz_rect tl;
          int max_len;
          int found = sync_refine_by_text(
              ps, ui, p, floating ? mark_box : fz_empty_rect, in_picture,
              !precision, 0, &max_len, &tp, &tl);
          // Only a part of the text around the cursor is on the page: a
          // paragraph can go on on the next page (or start on the previous
          // one), where SyncTeX has no record of the target line, and the
          // last lines of a paragraph have the number of the line after it.
          // Unless SyncTeX found the line itself (then the text continues
          // past a page break, or repeats), take a longer match on the next
          // page (its first occurrence), else on the previous page (its last
          // one).
          for (int d = 1; found < max_len && !floating && (precision || !found) &&
                          d >= -1; d -= 2)
          {
            int other = page + d;
            if (other < 0 || other >= send(page_count, ui->eng))
              continue;
            ui->page = other;
            display_page(ps, ui);
            fz_point start = d > 0 ? fz_make_point(0, 0)
                                   : fz_make_point(INFINITY, INFINITY);
            fz_point op;
            fz_rect ol;
            int other_max;
            int len = sync_refine_by_text(ps, ui, start, fz_empty_rect, false,
                                          false, found + 1, &other_max, &op, &ol);
            if (len > found)
            {
              found = len;
              page = other;
              tp = op;
              tl = ol;
            }
          }
          bool refined = found > 0;
          if (ui->page != page)
          {
            ui->page = page;
            display_page(ps, ui);
          }
          if (refined)
          {
            fprintf(stderr, "[synctex forward] refined by text: (%.02f, %.02f)\n",
                    tp.x, tp.y);
            p = tp;
            mark_box = tl;
          }
          else
            // Coordinates inside a TikZ picture are not where the material
            // is drawn: only highlight the picture.
            no_caret = floating;
        }
        fprintf(stderr, "[synctex forward] mark: page %d at (%.2f, %.2f) "
                "line (%.2f, %.2f)-(%.2f, %.2f) caret %d precision %d\n",
                page, p.x, p.y, mark_box.x0, mark_box.y0, mark_box.x1, mark_box.y1,
                !no_caret, precision);
        fz_point pt = txp_renderer_document_to_screen(ps->ctx, ui->doc_renderer, p);

        ui->sync_mark.active = true;
        ui->sync_mark.page = page;
        ui->sync_mark.pt = p;
        ui->sync_mark.box = mark_box;
        ui->sync_mark.no_caret = no_caret;
        ui->sync_mark.ticks = SDL_GetTicks();
        schedule_event(RENDER_EVENT);
        fprintf(stderr, "[synctex forward] position on screen: (%.02f, %.02f)\n",
                pt.x, pt.y);
        int w, h;
        txp_renderer_screen_size(ps->ctx, ui->doc_renderer, &w, &h);
        float margin_lo = h / 4.0;
        float margin_hi = h / 3.0;

        txp_renderer_config *config =
            txp_renderer_get_config(ps->ctx, ui->doc_renderer);

        float delta = 0.0;
        if (pt.y < margin_lo)
          delta = - pt.y + margin_hi;
        else if (pt.y >= h - margin_lo)
          delta = h - pt.y - margin_hi;
        fprintf(stderr, "[synctex forward] pan.y = %.02f + %.02f = %.02f\n",
                config->pan.y, delta, config->pan.y + delta);
        config->pan.y += delta;
        if (delta != 0.0)
          schedule_event(RENDER_EVENT);
      }
    }

    txp_renderer_set_scale_factor(ps->ctx, ui->doc_renderer,
                                  get_scale_factor(ui->window));
    txp_renderer_config *config =
        txp_renderer_get_config(ps->ctx, ui->doc_renderer);

    // Process event
    switch (e.type)
    {
      case SDL_QUIT:
        quit = 1;
        break;

      case SDL_KEYDOWN:
        switch (e.key.keysym.sym)
        {
          case SDLK_LEFT:
          case SDLK_PAGEUP:
            previous_page(ps->ctx, ui, 0);
            break;

          case SDLK_UP:
            ui_pan(ps->ctx, ui, 2.0/3.0);
            break;

          case SDLK_DOWN:
            ui_pan(ps->ctx, ui, -2.0/3.0);
            break;

          case SDLK_RIGHT:
          case SDLK_PAGEDOWN:
            next_page(ps->ctx, ui, 0);
            break;

          case SDLK_PLUS:
          case SDLK_KP_PLUS:
          case SDLK_EQUALS: // Handles standard '=' key (often sharing '+' on keyboards)
            config->zoom *= 1.1;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_MINUS:
          case SDLK_KP_MINUS:
            config->zoom /= 1.1;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_h:
            if (SDL_GetModState() & KMOD_SHIFT)
              ui_pan_x(ps->ctx, ui, 1.0/5.0);  // Large scroll left
            else
              ui_pan_x(ps->ctx, ui, 1.0/25.0);
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_l:
            if (SDL_GetModState() & KMOD_SHIFT)
              ui_pan_x(ps->ctx, ui, -1.0/5.0);  // Large scroll right
            else
              ui_pan_x(ps->ctx, ui, -1.0/25.0);
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_j:
            if (SDL_GetModState() & KMOD_SHIFT)
              next_page(ps->ctx, ui, 0);
            else
              ui_pan(ps->ctx, ui, -1.0/25.0); // Fine line down-pan
            break;

          case SDLK_k:
            if (SDL_GetModState() & KMOD_SHIFT)
              previous_page(ps->ctx, ui, 0);
            else
              ui_pan(ps->ctx, ui, 1.0/25.0);  // Fine line up-pan
            break;

          case SDLK_d:
            ui_pan(ps->ctx, ui, -1.0/5.0); // Medium down-pan
            break;

          case SDLK_u:
            ui_pan(ps->ctx, ui, 1.0/5.0);  // Medium up-pan
            break;

          case SDLK_SPACE:
            ui_pan(ps->ctx, ui, -2.0/3.0); // Page down (matching down arrow)
            break;

          case SDLK_b:
            if (SDL_GetModState() & KMOD_SHIFT)
            {
              // Shift + B: Toggle Window Border
              SDL_SetWindowBordered(
                  ui->window,
                  !!(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_BORDERLESS));
            }
            else
            {
              ui_pan(ps->ctx, ui, 2.0/3.0); // Lowercase b: Page up (matching up arrow)
            }
            break;

          case SDLK_w:
            config->fit = FIT_WIDTH;
            config->zoom = 1;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_p:
            config->fit = FIT_PAGE;
            config->zoom = 1;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_t:
            SDL_SetWindowAlwaysOnTop(
                ui->window,
                !(SDL_GetWindowFlags(ui->window) & SDL_WINDOW_ALWAYS_ON_TOP));
            break;

          case SDLK_c:
            config->crop = !config->crop;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_i:
            if ((SDL_GetModState() & KMOD_SHIFT))
              config->themed_color = !config->themed_color;
            else
              config->invert_color = !config->invert_color;
            schedule_event(RENDER_EVENT);
            break;

          case SDLK_ESCAPE:
            SDL_SetWindowFullscreen(ui->window, 0);
            break;

          // Return to where a link was followed from
          case SDLK_BACKSPACE:
            go_back(ps, ui);
            break;

          // Toggle Fullscreen
          case SDLK_f:
          case SDLK_F5:
          case SDLK_F11:
            SDL_SetWindowFullscreen(ui->window,
              (SDL_GetWindowFlags(ui->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ?
                0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            schedule_event(RENDER_EVENT);
            break;

          // case SDLK_r:
          //   reload = 1;
          case SDLK_q:
            quit = 1;
            break;
        }
        break;

      case SDL_MOUSEWHEEL:
        {
           int mx = 0, my = 0;
           float px = 0, py = 0;
           mouse_position_in_points(&mx, &my);
#if SDL_VERSION_ATLEAST(2, 0, 18)
          px = e.wheel.preciseX;
          py = e.wheel.preciseY;
#else
          px = e.wheel.x;
          py = e.wheel.y;
#endif
          bool ctrl = !!(SDL_GetModState() & KMOD_CTRL);
          ui_mouse_wheel(ps->ctx, ui, px, py, mx, my, ctrl, TXP_SCROLL_WHEEL,
                         OVERSCROLL_FRAME_MS);
        }
        break;

      case SDL3_PINCH_UPDATE:
      {
        float factor;
        memcpy(&factor, (char *)&e + sizeof(SDL_CommonEvent), sizeof(factor));
        int mx = 0, my = 0;
        mouse_position_in_points(&mx, &my);
        ui_pinch(ps->ctx, ui, factor, mx, my);
        break;
      }

      case SDL_MOUSEBUTTONDOWN:
      {
        int mx, my;
        mouse_position_in_points(&mx, &my);
        ui_mouse_down(ps, ui, mx, my, SDL_GetModState() & KMOD_CTRL);
        break;
      }

      case SDL_MOUSEBUTTONUP:
        ui_mouse_up(ui);
        break;

      case SDL_MOUSEMOTION:
      {
        int mx, my;
        mouse_position_in_points(&mx, &my);
        ui_mouse_move(ps->ctx, ui, mx, my);
        break;
      }

      case SDL_WINDOWEVENT:
        switch (e.window.event)
        {
          case SDL_WINDOWEVENT_SIZE_CHANGED:
          case SDL_WINDOWEVENT_RESIZED:
          case SDL_WINDOWEVENT_EXPOSED:
            sync_fullscreen_state(&fs, config, ui->window);
            schedule_event(RENDER_EVENT);
            break;
        }
        break;
    }

    if (e.type == ps->custom_event)
    {
      int page_count;
      *(char *)e.user.data1 = 0;
      switch (e.user.code)
      {
        case SCAN_EVENT:
          if (should_reload_binary())
          {
            quit = reload = 1;
            continue;
          }
          send(begin_changes, ui->eng, ps->ctx);
          flush_changes(ps, ui);
          send(detect_changes, ui->eng, ps->ctx);
          if (send(end_changes, ui->eng, ps->ctx))
          {
            if (!ps->paused)
              send(step, ui->eng, ps->ctx, true);
            schedule_event(RELOAD_EVENT);
          }
          break;

        case RENDER_EVENT:
          render(ps->ctx, ui);
          send(begin_changes, ui->eng, ps->ctx);
          flush_changes(ps, ui);
          if (send(end_changes, ui->eng, ps->ctx))
          {
            if (!ps->paused)
              send(step, ui->eng, ps->ctx, true);
            schedule_event(RELOAD_EVENT);
          }
          break;

        case RELOAD_EVENT:
          page_count = send(page_count, ui->eng);
          if (ui->page >= page_count &&
              send(get_status, ui->eng) == DOC_TERMINATED)
          {
            // Past the end of the document: at the end of the last page
            if (page_count > 0)
            {
              ui->page = page_count - 1;
              pan_to(ps->ctx, ui, PAN_TO_BOTTOM);
            }
          }
          if (ui->page < page_count)
            display_page(ps, ui);
          break;

        case STDIN_EVENT:
          break;

        case SCROLL_EVENT:
        {
          txp_scroll_event ev;
          while (txp_scroll_next(&ev))
            ui_scroll(ps->ctx, ui, &ev);
          break;
        }
      }
    }
    if (ps->initialize_only &&
        (send(page_count, ui->eng) > 0 ||
         (send(get_status, ui->eng) == DOC_TERMINATED && stdin_eof)))
    {
      fprintf(stderr, "[info] Initialize mode: terminating engine process\n");
      quit = 1;
    }
  }

  {
    int status = 0;
    quit_signal_fd = -1;
    wakeup_poll_thread(poll_stdin_pipe, 'q');
    SDL_WaitThread(poll_stdin_thread, &status);
    close(poll_stdin_pipe[0]);
    close(poll_stdin_pipe[1]);
  }

  SDL_DelEventWatch(repaint_on_resize, &repaint_on_resize_env);
  txp_scroll_stop();

  if (ps->initial.initialized && ps->initial.display_list)
    fz_drop_display_list(ps->ctx, ps->initial.display_list);
  ps->initial.initialized = 1;
  ps->initial.page = ui->page;
  ps->initial.need_synctex = ui->need_synctex;
  ps->initial.zoom = ui->zoom;
  ps->initial.config = *txp_renderer_get_config(ps->ctx, ui->doc_renderer);
  ps->initial.display_list = txp_renderer_get_contents(ps->ctx, ui->doc_renderer);
  if (ps->initial.display_list)
    fz_keep_display_list(ps->ctx, ps->initial.display_list);

  fz_drop_link(ps->ctx, ui->links);
  txp_renderer_free(ps->ctx, ui->doc_renderer);
  send(destroy, ui->eng, ps->ctx);

  return reload;
}
