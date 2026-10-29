#!/usr/bin/env python3
"""Update latency of TeXpresso: the time from a change to the screen.

Runs TeXpresso headless with TXP_TIMING=1, opens a document, shows the page
of a source line (forward sync), then inserts characters on that line. For
each change, the latency is the time from its arrival to the first frame
that shows new contents of the page after the change was applied (applying
it drops the page from the output, so the next contents include it).

  test/update-timing.py --bin build/texpresso -I dir --line 548 doc.tex

With --interval 0 (the default), each change waits until the previous
update is on screen and TeXpresso is idle; otherwise changes come every
--interval ms, as when typing. --sync also requests a forward sync to the
cursor after each change, as the VS Code extension does. --view-top first
scrolls the page to that height in the window (when the viewer can).
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import threading
import time


class Run:
    def __init__(self, args):
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', TXP_TIMING='1')
        cmd = [args.bin, '-json', '-lines']
        if args.include:
            cmd += ['-I', args.include]
        cmd.append(args.doc)
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=env)
        self.events = []  # (process ms, kind, rest), in order
        self.wheels = []  # pan after each test-wheel
        self.last_event = time.monotonic()
        self.cond = threading.Condition()
        self.log = open(args.log, 'wb') if args.log else None
        self.reader = threading.Thread(target=self.read_stderr, daemon=True)
        self.reader.start()
        threading.Thread(target=self.drain, args=(self.proc.stdout,),
                         daemon=True).start()

    def drain(self, f):
        while f.read(65536):
            pass

    def read_stderr(self):
        for line in self.proc.stderr:
            if self.log:
                self.log.write(line)
            if line.startswith(b'[test] wheel: '):
                words = line.split()
                with self.cond:
                    self.wheels.append(float(words[words.index(b'pan') + 1]))
                    self.cond.notify_all()
                continue
            i = line.find(b'[timing] ')
            if i < 0:
                continue
            parts = line[i + 9:].decode(errors='replace').split(None, 2)
            if len(parts) < 2:
                continue
            with self.cond:
                self.events.append((float(parts[0]), parts[1],
                                    parts[2].strip() if len(parts) > 2 else ''))
                self.last_event = time.monotonic()
                self.cond.notify_all()

    def send(self, *cmd):
        self.proc.stdin.write((json.dumps(list(cmd)) + '\n').encode())
        self.proc.stdin.flush()

    def quiet(self, seconds, timeout=120):
        """Wait until no timing event came for `seconds`."""
        start = time.monotonic()
        with self.cond:
            while time.monotonic() - self.last_event < seconds:
                if time.monotonic() - start > timeout:
                    raise TimeoutError('TeXpresso does not settle')
                self.cond.wait(seconds)

    def wait_for(self, pred, timeout=60):
        """Wait for an event after the current ones that satisfies pred."""
        with self.cond:
            n = len(self.events)
        start = time.monotonic()
        with self.cond:
            while True:
                for e in self.events[n:]:
                    if pred(e):
                        return e
                n = len(self.events)
                left = timeout - (time.monotonic() - start)
                if left <= 0:
                    raise TimeoutError('no matching event')
                self.cond.wait(left)

    def wheel(self, dy, phase):
        """Scroll by dy wheel units; returns the pan after it."""
        with self.cond:
            n = len(self.wheels)
            self.send('test-wheel', dy, phase)
            while len(self.wheels) == n:
                if not self.cond.wait(10):
                    raise TimeoutError('no reply to test-wheel')
            return self.wheels[n]

    def current_page(self):
        with self.cond:
            for t, kind, rest in reversed(self.events):
                if kind == 'present':
                    return int(rest.split()[1])
        return None

    def stop(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            self.proc.wait(10)
        self.reader.join(10)
        if self.log:
            self.log.close()


def latencies(events, page):
    """For each change, ms from its arrival to the first frame of `page`
    after it was applied (None if there is none)."""
    changes = [t for t, k, _ in events if k == 'change']
    realized = [t for t, k, _ in events if k == 'realize']
    presents = [t for t, k, r in events
                if k == 'present' and r.split()[1:2] == [str(page)]]
    result = []
    for i, t0 in enumerate(changes):
        # Changes are applied in order, possibly later (TeXpresso delays
        # changes that come while it is about to show the page)
        tr = realized[i] if i < len(realized) else None
        tp = next((t for t in presents if tr is not None and t > tr), None)
        result.append(None if tp is None else tp - t0)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin', required=True, help='texpresso binary')
    ap.add_argument('-I', dest='include', help='include directory')
    ap.add_argument('--content',
                    help='open the document with the contents of this file')
    ap.add_argument('--line', type=int, required=True,
                    help='source line to type on (1-based)')
    ap.add_argument('--col', type=int, default=0, help='column to type at')
    ap.add_argument('--edits', type=int, default=20)
    ap.add_argument('--interval', type=float, default=0,
                    help='ms between changes (0: one at a time)')
    ap.add_argument('--sync', action='store_true',
                    help='forward sync to the cursor after each change')
    ap.add_argument('--view-top', type=float,
                    help='scroll the top of the page to this height in the '
                    'window (pixels), before the changes')
    ap.add_argument('--cy', type=float, default=(700 * 792 / 612 - 900) / 2,
                    help='half the excess of the page height over the window '
                    '(pixels; by default, a letter page in the default window)')
    ap.add_argument('--settle', type=float, default=0.6,
                    help='seconds without events that count as idle')
    ap.add_argument('--log', help='write the stderr of TeXpresso here')
    ap.add_argument('--json', help='write the latencies here')
    ap.add_argument('doc')
    args = ap.parse_args()

    doc = os.path.abspath(args.doc)
    content = open(args.content or doc, encoding='utf-8').read()
    run = Run(args)
    try:
        run.send('open', doc, content)
        run.wait_for(lambda e: e[1] == 'present', timeout=120)
        run.quiet(args.settle * 2)
        run.send('synctex-forward', doc, args.line, args.col)
        # The viewer resolves forward syncs on its next event: send some
        # (rerun nil changes nothing) until the page shows
        for _ in range(40):
            try:
                run.wait_for(lambda e: e[1] == 'present', timeout=0.25)
                break
            except TimeoutError:
                run.send('rerun', False)
        run.quiet(args.settle * 2)
        page = run.current_page()
        print(f'page {page}', file=sys.stderr)
        if args.view_top is not None:
            # 20 pixels per wheel unit (WHEEL_PAN_SPEED, at scale 1)
            top = run.wheel(0, 'fingers') - args.cy
            pan = run.wheel((args.view_top - top) / 20, 'fingers')
            run.wheel(0, 'release')
            run.quiet(args.settle * 2)
            print(f'page top {top:.1f} -> {pan - args.cy:.1f}', file=sys.stderr)

        with run.cond:
            start = len(run.events)
        text = 'lorem ipsum dolor sit amet '
        for k in range(args.edits):
            col = args.col + k
            run.send('change-range', doc, args.line - 1, col, args.line - 1,
                     col, text[k % len(text)])
            if args.sync:
                run.send('synctex-forward', doc, args.line, col + 1)
            if args.interval > 0:
                time.sleep(args.interval / 1000)
            else:
                run.wait_for(lambda e: e[1] == 'present'
                             and e[2].split()[1:2] == [str(page)])
                run.quiet(args.settle)
        run.quiet(args.settle * 2)
        with run.cond:
            events = run.events[start:]
    finally:
        run.stop()

    lat = latencies(events, page)
    got = [x for x in lat if x is not None]
    if got:
        q = statistics.quantiles(got, n=10) if len(got) > 1 else got * 9
        print(f'{len(got)}/{len(lat)} updates: median {statistics.median(got):.1f} ms, '
              f'mean {statistics.mean(got):.1f}, p10 {q[0]:.1f}, '
              f'p90 {q[-1]:.1f}, max {max(got):.1f}')
    else:
        print(f'0/{len(lat)} updates')
    if args.json:
        with open(args.json, 'w') as f:
            json.dump({'page': page, 'latencies': lat}, f)


if __name__ == '__main__':
    main()
