#include "system/ambient_brightness.h"

#include <cmath>
#include <cstdio>
#include <print>

int main() {
  bool ok = true;
  AmbientBrightnessModel model;
  if (!(model.target(0) < model.target(300) && model.target(300) < model.target(10000))) {
    std::println(stderr, "ambient_brightness_test: more light must give a brighter screen");
    ok = false;
  }
  const float before = model.target(1000);
  model.learn(50, model.target(50) + 0.2F);
  if (model.target(1000) < before + 0.15F || std::abs(model.target(50) - (AmbientBrightnessModel::curve(50) + 0.2F)) > 0.001F) {
    std::println(stderr, "ambient_brightness_test: a brighter choice at one light level must carry to other levels");
    ok = false;
  }
  model.learn(50, 0.0F);
  if (model.target(0) <= 0.0F) {
    std::println(stderr, "ambient_brightness_test: the target must never turn the screen off");
    ok = false;
  }
  return ok ? 0 : 1;
}
