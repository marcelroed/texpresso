#ifndef TXP_SCROLL_H
#define TXP_SCROLL_H

#include <stdbool.h>

// Scroll events with the phases of trackpad gestures, which SDL does not
// report. Only on macOS: elsewhere scrolling comes from SDL_MOUSEWHEEL events.

enum txp_scroll_phase
{
  TXP_SCROLL_WHEEL,    // without phases (a mouse wheel)
  TXP_SCROLL_TOUCH,    // the fingers touched the trackpad
  TXP_SCROLL_FINGERS,  // the fingers move on the trackpad
  TXP_SCROLL_RELEASE,  // the fingers left the trackpad
  TXP_SCROLL_MOMENTUM, // after the fingers left, until the scroll stops
};

typedef struct
{
  enum txp_scroll_phase phase;
  float dx, dy; // as the x and y of SDL_MOUSEWHEEL events
  double ms;    // time of the event
} txp_scroll_event;

#ifdef __APPLE__

// Take the scroll events away from SDL. wakeup is called when some are
// queued. False if they cannot be taken (without a Cocoa application).
bool txp_scroll_start(void (*wakeup)(void));
void txp_scroll_stop(void);

// The next queued event, false if there is none
bool txp_scroll_next(txp_scroll_event *ev);

#else

static inline bool txp_scroll_start(void (*wakeup)(void))
{
  (void)wakeup;
  return false;
}

static inline void txp_scroll_stop(void) {}

static inline bool txp_scroll_next(txp_scroll_event *ev)
{
  (void)ev;
  return false;
}

#endif

#endif // TXP_SCROLL_H
