#!/usr/bin/env python3
"""Measure forward and backward SyncTeX accuracy of TeXpresso on a document.

The viewer runs headless (SDL dummy video driver). Ground truth comes from
the document itself: words of the source are aligned with words of the
rendered pages (text extracted by the viewer, with character boxes), keeping
words whose surrounding five-word context is unique in both, and words in
runs of at least three that match in order (the rest of a sentence around a
citation or a reference, whose rendering is not in the source). Each such
word gives

- forward cases: the editor cursor before each checked character of the word
  (and after its last one) must put the viewer's mark on that page, on that
  text line, at the left edge of the character (right edge after the last);
- backward cases: a click in the middle of each checked character must send
  the editor to the word's source line, with the cursor just before or just
  after the character.

Usage:
  synctex-check.py [--texpresso BIN] [-I DIR]... [--chars all|ends]
                   [--jobs N] [--json OUT] [--show N] document.tex

The cases are split between --jobs viewers running side by side, each
checking a contiguous part of the document.

Requires the viewer's test commands (test-click, test-page-text) and its
"[synctex forward] mark:" trace. The column TeXpresso sends in `synctex`
messages is taken as 1-based (the column of the character after the
cursor), the convention of the VS Code extension.
"""

import argparse
import difflib
import json
import os
import queue
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BIN = os.path.join(HERE, '..', 'build', 'texpresso')

# Commands whose braced arguments are not typeset as text.
SKIP_ARGS = {
    'label', 'ref', 'autoref', 'eqref', 'pageref', 'cref', 'Cref', 'cite',
    'citep', 'citet', 'citealp', 'citeauthor', 'citeyear', 'nocite',
    'begin', 'end', 'usepackage', 'documentclass', 'input', 'include',
    'includegraphics', 'bibliography', 'bibliographystyle', 'newcommand',
    'renewcommand', 'providecommand', 'def', 'url', 'href', 'hypersetup',
    'tikzset', 'usetikzlibrary', 'definecolor', 'setlength', 'addtolength',
    'setcounter', 'vspace', 'hspace', 'color', 'textcolor', 'pgfmathsetmacro',
    'newenvironment', 'renewenvironment', 'DeclareMathOperator', 'fontsize',
    'iclrfinalcopy', 'graphicspath', 'algrenewcommand', 'algnewcommand',
}
# Commands whose argument is typeset but elsewhere or differently: the
# words inside are tagged with the command, to report them separately.
CONTEXT_CMDS = {
    'caption', 'footnote', 'section', 'subsection', 'subsubsection',
    'paragraph', 'title', 'author', 'State', 'Require', 'Ensure', 'Comment',
    'node',
}
MATH_ENVS = {
    'equation', 'equation*', 'align', 'align*', 'gather', 'gather*',
    'multline', 'multline*', 'eqnarray', 'eqnarray*', 'displaymath', 'math',
}
# Environments typeset as they are (comment: not at all).
VERBATIM_ENVS = {
    'verbatim', 'verbatim*', 'Verbatim', 'Verbatim*', 'BVerbatim', 'LVerbatim',
    'lstlisting', 'minted', 'comment',
}
CONTEXT_ENVS = {
    'tikzpicture', 'algorithmic', 'algorithm', 'figure', 'table', 'tabular',
    'abstract', 'itemize', 'enumerate', 'description', 'thebibliography',
}


def fold(word):
    return word.casefold()


# ---------------------------------------------------------------------------
# Source side

class SourceWord:
    __slots__ = ('line', 'start', 'end', 'text', 'context')

    def __init__(self, line, start, end, text, context):
        self.line, self.start, self.end = line, start, end
        self.text, self.context = text, context


def source_words(text):
    """Words of a LaTeX source with their positions (line 1-based, columns
    0-based, in characters) and a context tag."""
    words = []
    env_stack = []        # environment names
    brace_stack = []      # command that opened each group, or None
    math = None           # closing delimiter of the current inline math
    pending_cmd = None    # command whose argument may follow
    skip_depth = None     # brace depth below which words are skipped
    verbatim = None       # the verbatim environment the lines are in

    def literal(lineno, line, i, j):
        """Words of verbatim text line[i:j]."""
        if verbatim == 'comment':
            return
        for m in re.finditer(r'[^\W\d_]+', line[i:j]):
            words.append(SourceWord(lineno, i + m.start(), i + m.end(),
                                    m.group(0), 'verbatim'))

    def context():
        for name in reversed(brace_stack):
            if name in CONTEXT_CMDS:
                return name
        for env in reversed(env_stack):
            if env in CONTEXT_ENVS:
                return env
        return 'text'

    for lineno, line in enumerate(text.split('\n'), 1):
        i, n = 0, len(line)
        # \State, \node and friends take the rest of the line (or up to ';')
        line_ctx = None
        while i < n or verbatim:
            if verbatim:
                end = line.find('\\end{%s}' % verbatim, i)
                literal(lineno, line, i, n if end < 0 else end)
                if end < 0:
                    break
                i = end + len('\\end{%s}' % verbatim)
                verbatim = None
                continue
            c = line[i]
            if c == '%':
                break
            if c == '\\':
                m = re.match(r'\\([A-Za-z@]+\*?)', line[i:])
                if m:
                    name = m.group(1)
                    i += len(m.group(0))
                    if name in ('begin', 'end'):
                        m2 = re.match(r'\s*\{([^}]*)\}', line[i:])
                        if m2:
                            env = m2.group(1)
                            i += len(m2.group(0))
                            if name == 'begin':
                                env_stack.append(env)
                            elif env_stack and env_stack[-1] == env:
                                env_stack.pop()
                            if name == 'begin' and env in VERBATIM_ENVS:
                                m3 = re.match(r'(\[[^]]*\])?(\{[^}]*\})?' if env == 'minted'
                                              else r'(\[[^]]*\])?', line[i:])
                                i += len(m3.group(0))
                                env_stack.pop()
                                verbatim = env
                        continue
                    if name in ('verb', 'lstinline', 'mintinline'):
                        m2 = re.match(r'\*?(\[[^]]*\])?' +
                                      (r'\{[^}]*\}' if name == 'mintinline' else ''), line[i:])
                        i += len(m2.group(0))
                        if i < n:
                            close = '}' if line[i] == '{' else line[i]
                            end = line.find(close, i + 1)
                            end = n if end < 0 else end
                            words.extend(SourceWord(lineno, i + 1 + m.start(), i + 1 + m.end(),
                                                    m.group(0), 'verbatim')
                                         for m in re.finditer(r'[^\W\d_]+', line[i + 1:end]))
                            i = end + 1
                        pending_cmd = None
                        continue
                    if name in ('(', '['):
                        pass
                    if name in ('State', 'Require', 'Ensure', 'node', 'Comment'):
                        line_ctx = name
                    pending_cmd = name
                    continue
                # \( \) \[ \] and escaped characters
                if line[i:i + 2] in ('\\(', '\\['):
                    math = '\\)' if line[i + 1] == '(' else '\\]'
                elif math and line[i:i + 2] == math:
                    math = None
                i += 2
                pending_cmd = None
                continue
            if c == '[' and pending_cmd:
                # optional argument: skip it
                depth, j = 0, i
                while j < n:
                    if line[j] == '[':
                        depth += 1
                    elif line[j] == ']':
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                i = j + 1
                continue
            if c == '{':
                brace_stack.append(pending_cmd)
                if pending_cmd in SKIP_ARGS and skip_depth is None:
                    skip_depth = len(brace_stack)
                pending_cmd = None
                i += 1
                continue
            if c == '}':
                if skip_depth is not None and len(brace_stack) == skip_depth:
                    skip_depth = None
                if brace_stack:
                    brace_stack.pop()
                pending_cmd = None
                i += 1
                continue
            if c == '$':
                if math == '$':
                    math = None
                elif math is None:
                    math = '$'
                i += 1
                continue
            if c.isalpha():
                j = i
                while j < n and line[j].isalpha():
                    j += 1
                in_math = math is not None or any(e in MATH_ENVS for e in env_stack)
                if skip_depth is None and not in_math:
                    ctx = context()
                    if ctx == 'text' and line_ctx:
                        ctx = line_ctx
                    words.append(SourceWord(lineno, i, j, line[i:j], ctx))
                i = j
                pending_cmd = None
                continue
            if not c.isspace():
                pending_cmd = None
            i += 1
    return words


# ---------------------------------------------------------------------------
# Rendered side

LIGATURES = {0xFB00: 'ff', 0xFB01: 'fi', 0xFB02: 'fl', 0xFB03: 'ffi',
             0xFB04: 'ffl', 0xFB05: 'st', 0xFB06: 'st'}


class RenderedChar:
    __slots__ = ('c', 'x0', 'y0', 'x1', 'y1', 'ox', 'oy', 'line')

    def __init__(self, c, x0, y0, x1, y1, ox, oy, line):
        self.c, self.x0, self.y0, self.x1, self.y1 = c, x0, y0, x1, y1
        self.ox, self.oy, self.line = ox, oy, line


class RenderedWord:
    __slots__ = ('page', 'chars', 'text')

    def __init__(self, page, chars):
        self.page, self.chars = page, chars
        self.text = ''.join(ch.c for ch in chars)


def expand_chars(raw, line_box):
    out = []
    for c, x0, y0, x1, y1, ox, oy in raw:
        if c in LIGATURES:
            s = LIGATURES[c]
            w = (x1 - x0) / len(s)
            for k, ch in enumerate(s):
                out.append(RenderedChar(ch, x0 + k * w, y0, x0 + (k + 1) * w, y1,
                                        ox + k * w, oy, line_box))
        else:
            out.append(RenderedChar(chr(c), x0, y0, x1, y1, ox, oy, line_box))
    return out


def is_letter(ch):
    return unicodedata.category(ch).startswith('L')


def rendered_words(page, lines):
    """Words of a page from the viewer's text dump. A word hyphenated at the
    end of a line is joined with its continuation."""
    words = []
    carry = None  # chars of a word hyphenated at the end of the previous line
    for line in lines:
        chars = expand_chars(line['chars'], tuple(line['bbox']))
        i, n = 0, len(chars)
        first = True
        while i < n:
            if not is_letter(chars[i].c):
                i += 1
                first = False
                continue
            j = i
            while j < n and is_letter(chars[j].c):
                j += 1
            run = chars[i:j]
            if carry is not None and first and i == 0:
                run = carry + run
            carry = None
            hyphen = (j == n - 1 and chars[j].c in '-\u2010\u00ad')
            if hyphen:
                carry = run
            else:
                words.append(RenderedWord(page, run))
            first = False
            i = j
        if carry is not None and n and not chars[-1].c in '-\u2010\u00ad':
            words.append(RenderedWord(page, carry))
            carry = None
    if carry is not None:
        words.append(RenderedWord(page, carry))
    return words


# ---------------------------------------------------------------------------
# Alignment

def ngram_keys(words, radius):
    keys = []
    texts = [fold(w.text) for w in words]
    for i in range(len(words)):
        lo, hi = i - radius, i + radius + 1
        if lo < 0 or hi > len(words):
            keys.append(None)
        else:
            keys.append(tuple(texts[lo:hi]))
    return keys


def align(src, ren, radius=2, min_run=3):
    skeys, rkeys = ngram_keys(src, radius), ngram_keys(ren, radius)
    scount, rcount = Counter(k for k in skeys if k), Counter(k for k in rkeys if k)
    rindex = {k: i for i, k in enumerate(rkeys) if k and rcount[k] == 1}
    match = {}
    for i, k in enumerate(skeys):
        if k and scount[k] == 1 and k in rindex:
            match[i] = rindex[k]
    # Runs matching in order, between the unique contexts.
    sm = difflib.SequenceMatcher(None, [fold(w.text) for w in src],
                                 [fold(w.text) for w in ren], autojunk=False)
    stexts, rtexts = sm.a, sm.b
    for a, b, size in sm.get_matching_blocks():
        if size < min_run:
            continue
        # Drop the words at either end that could as well match the word
        # next to the run on the other side (source "A vector" against the
        # page's "a A vector", where the first a is math).
        while size and a > 0 and b > 0 and (stexts[a] == rtexts[b - 1] or
                                            stexts[a - 1] == rtexts[b]):
            a, b, size = a + 1, b + 1, size - 1
        while size and a + size < len(stexts) and b + size < len(rtexts) and (
                stexts[a + size - 1] == rtexts[b + size] or
                stexts[a + size] == rtexts[b + size - 1]):
            size -= 1
        for d in range(size):
            match.setdefault(a + d, b + d)
    return [(src[i], ren[j]) for i, j in sorted(match.items())
            if len(src[i].text) == len(ren[j].chars)]


# ---------------------------------------------------------------------------
# Viewer

class Viewer:
    def __init__(self, binary, tex, includes, env_extra=None):
        self.tex = os.path.abspath(tex)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy')
        if env_extra:
            env.update(env_extra)
        cmd = [binary, '-json', '-lines']
        for d in includes:
            cmd += ['-I', os.path.abspath(d)]
        cmd.append(self.tex)
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, cwd=os.path.dirname(self.tex), env=env)
        self.lines = queue.Queue()
        self.log = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.proc.stdout:
            line = raw.decode(errors='replace').rstrip('\n')
            self.log.append(line)
            self.lines.put(line)
        self.lines.put(None)

    def send(self, command):
        self.proc.stdin.write((json.dumps(command) + '\n').encode())
        self.proc.stdin.flush()

    def drain(self):
        while True:
            try:
                self.lines.get_nowait()
            except queue.Empty:
                return

    def wait_for(self, predicate, timeout):
        """Lines until one satisfies predicate (returned last), or None."""
        seen = []
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                return seen, None
            try:
                line = self.lines.get(timeout=left)
            except queue.Empty:
                return seen, None
            if line is None:
                raise RuntimeError('texpresso exited')
            if predicate(line):
                return seen, line
            seen.append(line)

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(5)
        except Exception:
            self.proc.kill()


def compile_document(v, timeout):
    """Open the document and let the rerun passes typeset all of it."""
    source = open(v.tex, encoding='utf-8').read()
    v.send(['open', v.tex, source])
    v.send(['rerun', True])
    # Each pass runs in a new process and ends with "Document generation:
    # ...". A document that does not converge (errors, changing labels) gets
    # up to MAX_RERUNS finishing passes, each one after T_IDLE_MS (500 ms) of
    # idleness: it is done when no pass starts within a few times that after
    # the last one ended.
    deadline = time.time() + timeout
    running = True
    while time.time() < deadline:
        left = deadline - time.time()
        _, line = v.wait_for(
            lambda l: ('convergence reached' in l or
                       l.startswith('[process] launched') or
                       l.startswith('Document generation')),
            max(0.1, left if running else min(2, left)))
        if line is None:
            if not running:
                return source
            continue
        if 'convergence reached' in line:
            return source
        running = not line.startswith('Document generation')
    raise RuntimeError('document did not finish compiling')


def page_texts(v):
    pages = []
    tmp = tempfile.mkdtemp(prefix='synctex-check-')
    for page in range(1000):
        path = os.path.join(tmp, 'page%d.json' % page)
        v.send(['test-page-text', page, path])
        _, line = v.wait_for(lambda l: l.startswith('[test] page'), 10)
        if line is None or 'not available' in line:
            break
        pages.append(json.load(open(path)))
    return pages


MARK = re.compile(r'\[synctex forward\] mark: page (-?\d+) at \(([-\d.]+), ([-\d.]+)\) '
                  r'line \(([-\d.inf]+), ([-\d.inf]+)\)-\(([-\d.inf]+), ([-\d.inf]+)\) '
                  r'caret (\d) precision (\d+)')


def forward(v, line, column, timeout=10):
    # Each request ends with exactly one mark or "no match" line: waiting for
    # it keeps replies and requests in step.
    v.drain()
    v.send(['synctex-forward', v.tex, line, column])
    _, l = v.wait_for(lambda l: l.startswith('[synctex forward] mark:') or
                      l == '[synctex forward] no match', timeout)
    if l is None:
        raise RuntimeError('no reply to synctex-forward %d %d' % (line, column))
    if not l.startswith('[synctex forward] mark:'):
        return None
    m = MARK.match(l)
    return {'page': int(m.group(1)), 'x': float(m.group(2)), 'y': float(m.group(3)),
            'caret': m.group(8) == '1', 'precision': int(m.group(9))}


def backward(v, page, x, y):
    v.drain()
    v.send(['test-click', page, x, y])
    seen, done = v.wait_for(lambda l: l == '[test] click done', 5)
    for l in seen:
        if l.startswith('["synctex"'):
            msg = json.loads(l)
            return {'path': msg[1], 'line': msg[2], 'column': msg[3]}
    return None


# ---------------------------------------------------------------------------
# Checks

def char_positions(word, mode):
    n = len(word.text)
    if mode == 'all':
        return list(range(n + 1))
    return sorted({0, n // 2, n})


def check_forward(v, pairs, mode):
    results = []
    for s, r in pairs:
        for k in char_positions(s, mode):
            col = s.start + k
            if k < len(r.chars):
                ch = r.chars[k]
                ex, ey = ch.x0, ch.oy
            else:
                ch = r.chars[-1]
                ex, ey = ch.x1, ch.oy
            got = forward(v, s.line, col)
            res = {'line': s.line, 'column': col, 'word': s.text, 'k': k,
                   'context': s.context, 'page': r.page, 'x': ex, 'y': ey,
                   'got': got}
            if got is None:
                res['verdict'] = 'none'
            elif got['page'] != r.page:
                res['verdict'] = 'page'
            elif abs(got['y'] - ey) > 2:
                res['verdict'] = 'line'
            else:
                res['dx'] = got['x'] - ex
                res['verdict'] = 'ok' if abs(res['dx']) <= 1.5 else 'x'
            results.append(res)
    return results


def check_backward(v, pairs, mode):
    results = []
    for s, r in pairs:
        ks = range(len(r.chars)) if mode == 'all' else sorted({0, len(r.chars) // 2, len(r.chars) - 1})
        for k in ks:
            ch = r.chars[k]
            x, y = (ch.x0 + ch.x1) / 2, (ch.y0 + ch.y1) / 2
            got = backward(v, r.page, x, y)
            res = {'line': s.line, 'column': s.start + k, 'word': s.text, 'k': k,
                   'context': s.context, 'page': r.page, 'x': x, 'y': y, 'got': got}
            if got is None:
                res['verdict'] = 'none'
            elif os.path.abspath(got['path']) != os.path.abspath(v.tex):
                res['verdict'] = 'file'
            elif got['line'] != s.line:
                res['verdict'] = 'line'
            else:
                # 1-based column of the character after the cursor: a click
                # on character k may put the cursor before or after it.
                cursor = got['column'] - 1
                lo, hi = s.start + k, s.start + k + 1
                res['dcol'] = 0 if lo <= cursor <= hi else (cursor - lo if cursor < lo else cursor - hi)
                res['verdict'] = 'ok' if res['dcol'] == 0 else 'column'
            results.append(res)
    return results


def summarize(name, results, key):
    by_ctx = defaultdict(list)
    for r in results:
        by_ctx[r['context']].append(r)
    print('\n%s (%d cases)' % (name, len(results)))
    print('  %-14s %6s %6s %6s %6s %6s %6s   %s' %
          ('context', 'cases', 'ok', 'near', 'line', 'page', 'none', 'median |error|'))
    for ctx in sorted(by_ctx, key=lambda c: -len(by_ctx[c])):
        rs = by_ctx[ctx]
        c = Counter(r['verdict'] for r in rs)
        errs = [abs(r[key]) for r in rs if key in r]
        near = c['x'] + c['column']
        print('  %-14s %6d %6d %6d %6d %6d %6d   %s' %
              (ctx, len(rs), c['ok'], near, c['line'] + c['file'], c['page'],
               c['none'], ('%.2f' % statistics.median(errs)) if errs else '-'))


def show_failures(title, results, source_lines, n):
    bad = [r for r in results if r['verdict'] != 'ok']
    if not bad or n <= 0:
        return
    print('\n%s: first %d of %d failures' % (title, min(n, len(bad)), len(bad)))
    for r in bad[:n]:
        text = source_lines[r['line'] - 1]
        c = r['column']
        snippet = text[max(0, c - 30):c] + '|' + text[c:c + 30]
        print('  %s line %d col %d [%s] %r' % (r['verdict'], r['line'], c, r['context'], snippet))
        print('      expected page %d (%.1f, %.1f), got %s%s' %
              (r['page'], r['x'], r['y'], r['got'],
               (', dx %.1f' % r['dx']) if 'dx' in r else
               (', dcol %d' % r['dcol']) if 'dcol' in r else ''))


def in_parallel(viewers, pairs, check, mode):
    """Split pairs into contiguous chunks, one per viewer."""
    n = len(viewers)
    size = (len(pairs) + n - 1) // n
    chunks = [pairs[i * size:(i + 1) * size] for i in range(n)]
    with ThreadPoolExecutor(n) as ex:
        parts = ex.map(lambda vc: check(vc[0], vc[1], mode), zip(viewers, chunks))
        return [r for part in parts for r in part]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('document')
    ap.add_argument('--texpresso', default=DEFAULT_BIN)
    ap.add_argument('-I', dest='includes', action='append', default=[])
    ap.add_argument('--chars', choices=('all', 'ends'), default='ends',
                    help='check every character of a word, or its first, middle and last')
    ap.add_argument('--direction', choices=('both', 'forward', 'backward'), default='both')
    ap.add_argument('--lines', help='only check source lines A-B')
    ap.add_argument('--json', help='write every case to this file')
    ap.add_argument('--show', type=int, default=15, help='failures to print per direction')
    ap.add_argument('--timeout', type=float, default=120)
    ap.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1),
                    help='viewers checking cases in parallel')
    args = ap.parse_args()

    viewers = []
    try:
        t0 = time.time()
        for _ in range(max(1, args.jobs)):
            viewers.append(Viewer(os.path.abspath(args.texpresso), args.document,
                                  args.includes))
        with ThreadPoolExecutor(len(viewers)) as ex:
            source = list(ex.map(lambda v: compile_document(v, args.timeout), viewers))[0]
        pages = page_texts(viewers[0])
        print('compiled %d pages in %.1fs (%d viewers)' %
              (len(pages), time.time() - t0, len(viewers)))

        src = source_words(source)
        ren = [w for p, lines in enumerate(pages) for w in rendered_words(p, lines)]
        pairs = align(src, ren)
        if args.lines:
            a, b = map(int, args.lines.split('-'))
            pairs = [(s, r) for s, r in pairs if a <= s.line <= b]
        print('%d source words, %d rendered words, %d aligned' %
              (len(src), len(ren), len(pairs)))

        out = {}
        lines = source.split('\n')
        t0 = time.time()
        if args.direction in ('both', 'forward'):
            out['forward'] = in_parallel(viewers, pairs, check_forward, args.chars)
            summarize('forward: editor cursor -> viewer mark', out['forward'], 'dx')
        if args.direction in ('both', 'backward'):
            out['backward'] = in_parallel(viewers, pairs, check_backward, args.chars)
            summarize('backward: click -> editor cursor', out['backward'], 'dcol')
        print('\nchecked %d cases in %.1fs' %
              (sum(len(rs) for rs in out.values()), time.time() - t0))
        for name, rs in out.items():
            show_failures(name, rs, lines, args.show)
        if args.json:
            with open(args.json, 'w') as f:
                json.dump(out, f, indent=1)
    finally:
        for v in viewers:
            v.close()


if __name__ == '__main__':
    main()
