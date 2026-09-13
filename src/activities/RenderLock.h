#pragma once

#include <freertos/FreeRTOS.h>

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  // Wait at most ticksToWait for the mutex (0 = take it only if free now) and
  // report the outcome via locked(). For main-task code that must not stall
  // behind an in-flight render: the main loop is the only button sampler.
  explicit RenderLock(TickType_t ticksToWait);
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  bool locked() const { return isLocked; }
  static bool peek();
};
