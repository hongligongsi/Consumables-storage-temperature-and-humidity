#pragma once
#include "ui_model.h"

class TftUi {
 public:
  void begin();
  void render(const UiSnapshot &snapshot);
 private:
  bool ready_ = false;
};
