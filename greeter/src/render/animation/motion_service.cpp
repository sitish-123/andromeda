#include "render/animation/motion_service.h"

MotionService& MotionService::instance() {
  static MotionService s_instance;
  return s_instance;
}
