#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

struct dsk_output_manager_v1;
struct wl_registry;

// Client of the desktop fork's dsk_output_manager_v1: which outputs mirror which, and requests to change that. Like
// OutputManagement it binds only while a caller needs it.
class MirrorControl {
public:
  using ChangeCallback = std::function<void()>;

  MirrorControl(wl_registry* registry, std::uint32_t globalName, std::uint32_t version, ChangeCallback onChange);
  ~MirrorControl();

  MirrorControl(const MirrorControl&) = delete;
  MirrorControl& operator=(const MirrorControl&) = delete;

  [[nodiscard]] bool ready() const noexcept { return m_ready; }
  // Target connector to source connector, for outputs that currently mirror.
  [[nodiscard]] const std::unordered_map<std::string, std::string>& mirrors() const noexcept { return m_mirrors; }
  // Reason the compositor gave for the last rejected request, empty when none.
  [[nodiscard]] const std::string& lastFailure() const noexcept { return m_lastFailure; }

  void setMirror(const std::string& target, const std::string& source);
  void clearMirror(const std::string& target);

private:
  friend struct MirrorControlListeners;

  dsk_output_manager_v1* m_manager = nullptr;
  std::unordered_map<std::string, std::string> m_mirrors;
  std::string m_lastFailure;
  bool m_ready = false;
  ChangeCallback m_onChange;
};
