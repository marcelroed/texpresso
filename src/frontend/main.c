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

struct persistent_state *pstate;

static void schedule_event(enum custom_events ev)
{
  pstate->schedule_event(ev);
}

static bool should_reload_binary(void)
{
  return pstate->should_reload_binary();
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
    uint32_t ticks, last_frame;
  } sync_mark;

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

/* UI rendering */

static float zoom_factor(int count)
{
  return expf((float)count / 5000.0f);
}

static fz_point get_scale_factor(SDL_Window *window);

static void render_sync_mark(fz_context *ctx, ui_state *ui)
{
  ui->sync_mark.last_frame = SDL_GetTicks();
  if (!sync_mark_active(ui) || ui->sync_mark.page != ui->page)
    return;

  uint32_t elapsed = ui->sync_mark.last_frame - ui->sync_mark.ticks;
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

struct sync_text {
  int chars[SYNC_TEXT_MAX];     // folded characters of all words
  int source[SYNC_TEXT_MAX];    // their index in the line
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
};

static void sync_text_reset(struct sync_text *t)
{
  t->nchars = t->nwords = 0;
  t->depth = t->nsegments = t->open = t->tikz = 0;
  t->node = false;
  t->verbatim[0] = 0;
}

// Append the folded character f, line[i], to the word being read (if
// *in_word) or to a new one.
static void sync_add_char(struct sync_text *t, bool *in_word, int i, int f,
                          int gap)
{
  if (t->nchars == SYNC_TEXT_MAX || (!*in_word && t->nwords == SYNC_TEXT_MAX))
    return;
  if (!*in_word)
  {
    struct sync_word *w = &t->words[t->nwords++];
    w->start = i;
    w->first = t->nchars;
    w->count = 0;
    w->segment = t->nsegments ? t->segment[t->nsegments - 1] : 0;
    *in_word = true;
  }
  struct sync_word *w = &t->words[t->nwords - 1];
  t->chars[t->nchars] = f;
  t->source[t->nchars] = i;
  t->gap[t->nchars] = gap;
  t->nchars++;
  w->count++;
  w->end = i + 1;
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
      sync_add_char(t, &in_word, i, f, 0);
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
    "minted", "thebibliography", "subfigure", "list", "tikzpicture", NULL
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

// Split a source line into the words likely to be typeset as text: macro
// names, math, optional arguments, and the arguments of macros that do not
// typeset text are left out. The words are appended to t, which also keeps
// track of the segments (see sync_word) from line to line.
static void sync_source_words(const int *line, int n, struct sync_text *t)
{
  bool math = false, in_word = false;
  int gap = 0;
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
        if (!math && (mint || strcmp(name, "verb") == 0 ||
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
        if (!math && sync_macro_moves_argument(name))
        {
          while (i < n && line[i] == ' ')
            i++;
          if (i < n && line[i] == '[')
            i = sync_skip_group(line, n, i, '[', ']');
          if (i < n && line[i] == '{')
            sync_segment_open(t, SYNC_SEGMENT_ARGUMENT);
        }
        else if (!math && sync_macro_skips_argument(name))
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
        math = true, in_word = false, gap = TXP_TEXT_MAX_GAP;
      else if (d == ')' || d == ']')
        math = false, in_word = false;
      // Other control symbols (accents, \_, \&, ...) do not split words.
      after_macro = d == '\\';
      i += 2;
      continue;
    }
    if (c == '$')
    {
      math = !math;
      in_word = false;
      gap = TXP_TEXT_MAX_GAP;
      i++;
      continue;
    }
    if (math)
    {
      i++;
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
      sync_add_char(t, &in_word, i, f, gap);
      gap = 0;
    }
    else if (c != '{' && c != '}')
      in_word = false;
    i++;
  }
}

// The contents of a file of the document, as the editor last sent it.
static fz_buffer *sync_file_data(struct persistent_state *ps, ui_state *ui,
                                 const char *path)
{
  fileentry_t *e = send(find_file, ui->eng, ps->ctx, path);
  return e ? (e->edit_data ? e->edit_data : e->fs_data) : NULL;
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
      w1 = t.nwords;
  }
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

  // Number of folded characters of word k before the cursor. The cursor is
  // at the left edge of the next one, or at the right edge of the previous
  // one when it follows it directly (at the end of the word, or before
  // markup inside it, as in d|\_in).
  int in_word = 0;
  const int *src = t.source + t.words[k].first;
  while (in_word < t.words[k].count && src[in_word] < column)
    in_word++;
  bool after = in_word == t.words[k].count ||
               (in_word > 0 && src[in_word - 1] == column - 1 &&
                src[in_word] > column);

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
  static const int spans[][2] = {{0, 2}, {-1, 1}, {0, 1}, {-1, 0}, {-2, 0}, {0, 0}};
  static int needle[SYNC_TEXT_MAX];
  static unsigned char gap[SYNC_TEXT_MAX];
  for (int s = 0; s < (int)(sizeof(spans) / sizeof(spans[0])); s++)
  {
    int a = kk + spans[s][0], b = kk + spans[s][1];
    if (a < 0 || b >= nseq)
      continue;
    int len = 0, offset = 0;
    for (int m = a; m <= b; m++)
    {
      struct sync_word *w = &t.words[seq[m]];
      if (m == kk)
        offset = len + in_word - (after ? 1 : 0);
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
  int c, line, column;
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
      src[ns].gap = t.gap[i];
      // Material left out starts after the previous word of the line.
      int g = i > 0 ? t.source[i - 1] + 1 : 0;
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
                          : src[best].column + (after ? 1 : 0);
    fprintf(stderr, "[synctex backward] refined by text: line %d column %d "
            "(%d characters match%s)\n",
            *line, *column, best_score, best_in_gap ? ", in a gap" : "");
  }
  fz_free(ps->ctx, src);
  return found;
}

static void render(fz_context *ctx, ui_state *ui)
{
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
  int name_len, line, column;
  if (!synctex_scan(ps->ctx, stx, buf, ui->page, f * pt.x, f * pt.y,
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
  if (go_up == 0)
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

static void ui_mouse_wheel(fz_context *ctx, ui_state *ui, float dx, float dy, int mousex, int mousey, bool ctrl, int timestamp)
{
  fz_point scale = get_scale_factor(ui->window);

  if (ui->mouse_status != UI_MOUSE_NONE)
    return;

  txp_renderer_config *config = txp_renderer_get_config(ctx, ui->doc_renderer);

  if (ctrl)
  {
    SDL_FRect rect;
    if (dy != 0 && txp_renderer_page_position(ctx, ui->doc_renderer, &rect, NULL, NULL))
    {
      ui->zoom = fz_maxi(ui->zoom + dy * 100, 0);
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
  }
  else
  {
    (void)timestamp;
    float x = scale.x * dx * WHEEL_PAN_SPEED;
    float y = scale.y * dy * WHEEL_PAN_SPEED;
    config->pan.x -= x;
    config->pan.y += y;
    // fprintf(stderr, "wheel pan: (%.02f, %.02f) raw:(%.02f, %.02f)\n", x, y, dx, dy);
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

  bool quit = 0, reload = 0;
  if (!ps->paused)
    send(step, ui->eng, ps->ctx, true);
  render(ps->ctx, ui);
  schedule_event(RELOAD_EVENT);

  struct repaint_on_resize_env repaint_on_resize_env = {.ctx = ps->ctx, .ui = ui};
  SDL_AddEventWatch(repaint_on_resize, &repaint_on_resize_env);

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
        if (advance)
          continue;
        if (!stdin_eof)
          wakeup_poll_thread(poll_stdin_pipe, 'c');

        bool rerun_eligible = ps->rerun_enabled
                              && rerun_count < MAX_RERUNS
                              && aux_ready;
        bool animating = sync_mark_active(ui);
        if (animating)
        {
          uint32_t since = SDL_GetTicks() - ui->sync_mark.last_frame;
          has_event = SDL_WaitEventTimeout(
              &e, since >= SYNC_MARK_FRAME_MS ? 1 : SYNC_MARK_FRAME_MS - since);
        }
        else if (rerun_eligible)
          has_event = SDL_WaitEventTimeout(&e, T_IDLE_MS);
        else
          has_event = SDL_WaitEvent(&e);
        if (!has_event && animating)
        {
          // Next frame of the sync marker fade-out.
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
        if (page == ui->page)
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
              ui_pan(ps->ctx, ui, -1.0/5.0); // Medium down-pan
            else
              ui_pan(ps->ctx, ui, -1.0/25.0); // Fine line down-pan
            break;

          case SDLK_k:
            if (SDL_GetModState() & KMOD_SHIFT)
              ui_pan(ps->ctx, ui, 1.0/5.0);  // Medium up-pan
            else
              ui_pan(ps->ctx, ui, 1.0/25.0);  // Fine line up-pan
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
          ui_mouse_wheel(ps->ctx, ui, px, py, mx, my, ctrl, e.wheel.timestamp);
        }
        break;

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
            if (page_count > 0)
              ui->page = page_count - 1;
          }
          if (ui->page < page_count)
            display_page(ps, ui);
          break;

        case STDIN_EVENT:
          break;
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
