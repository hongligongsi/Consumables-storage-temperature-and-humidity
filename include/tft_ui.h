#pragma once
#include "ui_model.h"

class TftUi {
 public:
  void begin();
  // Non-blocking: the newest snapshot replaces the pending frame.  Actual SPI
  // drawing runs in a dedicated low-priority task, so a 480x320 transfer can
  // never stall the 50 ms thermal-control cadence in loop().
  void render(const UiSnapshot &snapshot);
  bool doubleBuffered() const { return doubleBuffered_; }
 private:
  bool ready_ = false;
  bool doubleBuffered_ = false;
  void *queue_ = nullptr;
};
