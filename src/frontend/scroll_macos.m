#import <AppKit/AppKit.h>
#include <math.h>
#include "scroll.h"

// The monitor runs on the main thread, while SDL pumps the events, and the
// queue is emptied there too: no locking.

#define QUEUE_SIZE 64

static id monitor;
static void (*wakeup)(void);
static txp_scroll_event queue[QUEUE_SIZE];
static unsigned queue_head, queue_len;

static enum txp_scroll_phase event_phase(NSEvent *e)
{
  if (e.momentumPhase != NSEventPhaseNone)
    return TXP_SCROLL_MOMENTUM;
  NSEventPhase phase = e.phase;
  if (phase & (NSEventPhaseMayBegin | NSEventPhaseBegan))
    return TXP_SCROLL_TOUCH;
  if (phase & (NSEventPhaseEnded | NSEventPhaseCancelled))
    return TXP_SCROLL_RELEASE;
  if (phase != NSEventPhaseNone)
    return TXP_SCROLL_FINGERS;
  return TXP_SCROLL_WHEEL;
}

static void enqueue(const txp_scroll_event *ev)
{
  if (queue_len == QUEUE_SIZE)
  {
    // Not emptied for a while: add to the last event
    txp_scroll_event *last = &queue[(queue_head + queue_len - 1) % QUEUE_SIZE];
    if (last->phase == ev->phase)
    {
      last->dx += ev->dx;
      last->dy += ev->dy;
      last->ms = ev->ms;
    }
    return;
  }
  queue[(queue_head + queue_len) % QUEUE_SIZE] = *ev;
  queue_len += 1;
}

static NSEvent *scroll_event(NSEvent *e)
{
  txp_scroll_event ev = {.phase = event_phase(e), .ms = e.timestamp * 1000.0};

  // The deltas SDL computes (Cocoa_HandleMouseWheel)
  if (e.hasPreciseScrollingDeltas)
  {
    ev.dx = -e.scrollingDeltaX * 0.1f;
    ev.dy = e.scrollingDeltaY * 0.1f;
  }
  else
  {
    ev.dx = -e.deltaX;
    ev.dy = e.deltaY;
    ev.dx = ev.dx > 0 ? ceilf(ev.dx) : floorf(ev.dx);
    ev.dy = ev.dy > 0 ? ceilf(ev.dy) : floorf(ev.dy);
  }

  bool moves = ev.dx != 0 || ev.dy != 0;
  if (moves || ev.phase == TXP_SCROLL_TOUCH || ev.phase == TXP_SCROLL_RELEASE)
  {
    enqueue(&ev);
    if (wakeup)
      wakeup();
  }

  // SDL does not see it
  return nil;
}

bool txp_scroll_start(void (*callback)(void))
{
  wakeup = callback;
  if (!monitor && NSApp)
    monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                                    handler:^NSEvent *(NSEvent *e) {
                                                      return scroll_event(e);
                                                    }];
  return monitor != nil;
}

void txp_scroll_stop(void)
{
  if (monitor)
    [NSEvent removeMonitor:monitor];
  monitor = nil;
  wakeup = NULL;
  queue_len = 0;
}

bool txp_scroll_next(txp_scroll_event *ev)
{
  if (queue_len == 0)
    return false;
  *ev = queue[queue_head];
  queue_head = (queue_head + 1) % QUEUE_SIZE;
  queue_len -= 1;
  return true;
}
