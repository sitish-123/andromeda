#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

struct dsk_output_manager_v1;
struct wl_registry;

// Client of the desktop fork's dsk_output_manager_v1: which outputs mirror which, the display properties
// zwlr_output_manager_v1 does not carry (HDR, VRR mode, workspaces), and requests to change them. Like
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

  // Effective value of display property `key` ("hdr", "workspaces") on connector `target`; empty when unknown.
  [[nodiscard]] std::string property(const std::string& target, std::string_view key) const;
  // Empty value removes the saved property.
  void setProperty(const std::string& target, const std::string& key, const std::string& value);

  void setMirror(const std::string& target, const std::string& source);
  void clearMirror(const std::string& target);

private:
  friend struct MirrorControlListeners;

  dsk_output_manager_v1* m_manager = nullptr;
  std::unordered_map<std::string, std::string> m_mirrors;
  // Keyed "<connector>\n<key>".
  std::unordered_map<std::string, std::string> m_properties;
  std::string m_lastFailure;
  bool m_ready = false;
  ChangeCallback m_onChange;
};
