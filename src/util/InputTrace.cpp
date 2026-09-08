#include "InputTrace.h"

#if CROSSPOINT_INPUT_TRACE

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <freertos/FreeRTOS.h>

#include <cstdio>

#include "../CrossPointSettings.h"
#include "../activities/RenderLock.h"

#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "dev"
#endif

namespace InputTrace {
namespace {

struct Record {
  uint32_t ms;
  int32_t aux;
  uint16_t dt;         // ms since the previous input poll (poll records only)
  int16_t adc1;        // raw ADC, group 1 (Back/Confirm/Left/Right), -1 if not sampled
  int16_t adc2;        // raw ADC, group 2 (Up/Down)
  uint8_t tag;
  uint8_t cur;         // committed pressed-state bitmask (BTN_* bits)
  uint8_t pressed;     // press edges this frame
  uint8_t released;    // release edges this frame
  uint8_t flags;       // bit0 debounce pending, bit1 render lock held, bit2 CPU low-power
  int8_t cls1;         // classified BTN_* for group 1, -1 none
  int8_t cls2;
  int8_t sub;
  int8_t pendingTurn;
};

constexpr size_t RING = 128;
constexpr unsigned long SLOWPOLL_MS = 120;
constexpr const char* PATH = "/.crosspoint/inputtrace.log";

Record ring[RING];
volatile size_t head = 0;  // next write
volatile size_t tail = 0;  // next read
volatile size_t dropped = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
unsigned long lastPollMs = 0;

const char* tagName(uint8_t t) {
  switch (t) {
    case BOOT: return "BOOT";
    case PENDING: return "PENDING";
    case PRESS: return "PRESS";
    case RELEASE: return "RELEASE";
    case SEEN: return "SEEN";
    case DEFER: return "DEFER";
    case DEFER_EXEC: return "DEFER_EXEC";
    case TURN: return "TURN";
    case RENDER: return "RENDER";
    case SLOWPOLL: return "SLOWPOLL";
    case FLUSH: return "FLUSH";
    default: return "?";
  }
}

uint8_t baseFlags() {
  uint8_t f = 0;
  if (RenderLock::peek()) f |= 0x02;
  if (getCpuFrequencyMhz() < 100) f |= 0x04;
  return f;
}

void push(const Record& r) {
  portENTER_CRITICAL(&mux);
  const size_t next = (head + 1) % RING;
  if (next == tail) {
    dropped = dropped + 1;  // ring full: drop the newest rather than block
  } else {
    ring[head] = r;
    head = next;
  }
  portEXIT_CRITICAL(&mux);
}

bool pop(Record& r) {
  portENTER_CRITICAL(&mux);
  const bool have = head != tail;
  if (have) {
    r = ring[tail];
    tail = (tail + 1) % RING;
  }
  portEXIT_CRITICAL(&mux);
  return have;
}

Record blank(Tag tag) {
  Record r{};
  r.ms = millis();
  r.tag = tag;
  r.adc1 = -1;
  r.adc2 = -1;
  r.cls1 = -1;
  r.cls2 = -1;
  r.flags = baseFlags();
  return r;
}

}  // namespace

void begin() {
  Record r = blank(BOOT);
  r.aux = (static_cast<int32_t>(SETTINGS.longPressButtonBehavior) << 16) |
          (static_cast<int32_t>(SETTINGS.sideButtonLayout) << 8) | static_cast<int32_t>(SETTINGS.refreshFrequency);
  push(r);
  lastPollMs = millis();
}

void samplePoll(HalGPIO& gpio) {
  const unsigned long now = millis();
  const unsigned long dt = now - lastPollMs;
  lastPollMs = now;

  const uint8_t pressed = gpio.pressedMask();
  const uint8_t released = gpio.releasedMask();
  const bool pending = gpio.isDebouncePending();

  if (dt > SLOWPOLL_MS) {
    Record r = blank(SLOWPOLL);
    r.dt = dt > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(dt);
    r.aux = static_cast<int32_t>(dt);
    r.cur = gpio.currentMask();
    push(r);
  }
  if (!pressed && !released && !pending) return;

  Record r = blank(pressed ? PRESS : (released ? RELEASE : PENDING));
  r.dt = dt > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(dt);
  r.cur = gpio.currentMask();
  r.pressed = pressed;
  r.released = released;
  if (pending) r.flags |= 0x01;
  int raw1, cls1, raw2, cls2;
  gpio.readButtonAdc(raw1, cls1, raw2, cls2);
  r.adc1 = static_cast<int16_t>(raw1);
  r.adc2 = static_cast<int16_t>(raw2);
  r.cls1 = static_cast<int8_t>(cls1);
  r.cls2 = static_cast<int8_t>(cls2);
  push(r);
}

void record(Tag tag, int32_t aux, int8_t sub, int8_t pendingTurn) {
  Record r = blank(tag);
  r.aux = aux;
  r.sub = sub;
  r.pendingTurn = pendingTurn;
  push(r);
}

bool nearlyFull() {
  const size_t used = (head + RING - tail) % RING;
  return used > (RING * 3) / 4;
}

const char* logPath() { return PATH; }

void flush() {
  if (head == tail) return;
  if (!Storage.ready()) return;
  HalFile f = Storage.open(PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return;
  char line[160];
  size_t written = 0;
  Record r;
  while (pop(r)) {
    int n;
    if (r.tag == BOOT) {
      n = snprintf(line, sizeof(line), "%lu BOOT ver=%s lp=%ld side=%ld rf=%ld flags=%u\n",
                   static_cast<unsigned long>(r.ms), CROSSPOINT_VERSION, static_cast<long>((r.aux >> 16) & 0xFF),
                   static_cast<long>((r.aux >> 8) & 0xFF), static_cast<long>(r.aux & 0xFF), r.flags);
    } else {
      n = snprintf(line, sizeof(line), "%lu %s dt=%u cur=%u pr=%u rl=%u fl=%u adc1=%d cls1=%d adc2=%d cls2=%d pt=%d sub=%d aux=%ld\n",
                   static_cast<unsigned long>(r.ms), tagName(r.tag), r.dt, r.cur, r.pressed, r.released, r.flags, r.adc1,
                   r.cls1, r.adc2, r.cls2, r.pendingTurn, r.sub, static_cast<long>(r.aux));
    }
    if (n > 0) {
      f.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n) < sizeof(line) ? n : sizeof(line) - 1);
      written++;
    }
  }
  const size_t lost = dropped;
  dropped = 0;
  const int n = snprintf(line, sizeof(line), "%lu FLUSH written=%u dropped=%u\n", static_cast<unsigned long>(millis()),
                         static_cast<unsigned>(written), static_cast<unsigned>(lost));
  if (n > 0) f.write(reinterpret_cast<const uint8_t*>(line), n);
  f.close();
}

}  // namespace InputTrace

#endif  // CROSSPOINT_INPUT_TRACE
