#pragma once

#include <cstdint>
#include <unordered_set>

class AnimationManager;

class MotionService {
public:
  static MotionService& instance();

  MotionService(const MotionService&) = delete;
  MotionService& operator=(const MotionService&) = delete;

  void registerManager(AnimationManager* manager);
  void unregisterManager(AnimationManager* manager);

  void setEnabled(bool enabled);
  void setSpeed(float speed);
  void setReduced(bool reduced) { m_reduced = reduced; }

  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  // Reduced motion (rulebook 7.8): spatial springs jump; the caller crossfades. enabled() == false is instant.
  [[nodiscard]] bool reduced() const noexcept { return m_reduced; }
  [[nodiscard]] float speed() const noexcept { return m_speed; }

private:
  MotionService() = default;

  bool m_enabled = true;
  bool m_reduced = false;
  float m_speed = 1.0F;
  std::unordered_set<AnimationManager*> m_managers;
};
