#pragma once

#include <cstdint>

// Field trace for "pressed the page-turn button and nothing happened".
//
// Compile-time gated by -DCROSSPOINT_INPUT_TRACE=1. Records only interesting
// input frames (a press/release edge, a half-seen sample that the two-sample
// debounce has not committed yet, or an unusually long poll gap) plus the
// reader's turn decisions into a small RAM ring, and appends them as text
// lines to /.crosspoint/inputtrace.log from the render task. Nothing is
// printed over serial, so the trace does not change what it measures.

#ifndef CROSSPOINT_INPUT_TRACE
#define CROSSPOINT_INPUT_TRACE 0
#endif

class HalGPIO;

namespace InputTrace {

enum Tag : uint8_t {
  BOOT = 0,    // session header; aux = packed settings
  PENDING,     // raw ADC sample disagrees with committed state (debounce in flight)
  PRESS,       // committed press edge; pressed mask says which
  RELEASE,     // committed release edge
  SEEN,        // reader turn block saw a page-turn trigger; sub = +1 next / -1 prev
  DEFER,       // press queued in pendingManualTurn (render lock or 200 ms guard)
  DEFER_EXEC,  // queued press executed
  TURN,        // pageTurn() called; sub = +1 / -1
  RENDER,      // renderContents finished; aux = ms, sub = 1 if refresh-cycle turn
  SLOWPOLL,    // gap between two input polls exceeded the threshold; aux = ms
  FLUSH,       // ring flushed to SD; aux = records written
  CONTACT,     // raw ADC off the idle rail with no committed/pending button: partial or light touch
};

#if CROSSPOINT_INPUT_TRACE
// Write the session header record. Call once after settings are loaded.
void begin();
// Call right after the main loop's input update. Records PENDING / PRESS /
// RELEASE / SLOWPOLL frames with raw ADC readback.
void samplePoll(HalGPIO& gpio);
// Record a reader-side event (no ADC readback).
void record(Tag tag, int32_t aux = 0, int8_t sub = 0, int8_t pendingTurn = 0);
// Append pending records to the log file. Safe from any task; cheap when empty.
void flush();
// True when the ring is over three quarters full (main-loop fallback flush).
bool nearlyFull();
// Path of the log file on SD.
const char* logPath();
#else
inline void begin() {}
inline void samplePoll(HalGPIO&) {}
inline void record(Tag, int32_t = 0, int8_t = 0, int8_t = 0) {}
inline void flush() {}
inline bool nearlyFull() { return false; }
inline const char* logPath() { return ""; }
#endif

}  // namespace InputTrace
