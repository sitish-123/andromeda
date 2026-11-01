#include "theme/live_appearance_sync.h"

#include "compositors/compositor_detect.h"
#include "compositors/settings_control.h"
#include "compositors/shell_wayland_connection.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace {

  float parseUnit(const std::string& text, float fallback) {
    float value = fallback;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return std::clamp(value, 0.0F, 1.0F);
  }

  std::string lowercase(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
  }

} // namespace

LiveAppearanceSync::LiveAppearanceSync() = default;

LiveAppearanceSync::~LiveAppearanceSync() = default;

void LiveAppearanceSync::initialize(ShellWaylandConnection& wayland, ChangeCallback onChange) {
  m_onChange = std::move(onChange);
  const auto [name, version] = wayland.desktopSettingsGlobal();
  if (!compositors::isAndromeda() || name == 0) {
    return;
  }
  m_control = std::make_unique<SettingsControl>(wayland.registry(), name, version, [this]() { onSettingsChanged(); });
}

void LiveAppearanceSync::publish(std::string_view scheme, std::string_view accent) {
  m_scheme = std::string(scheme);
  m_accent = lowercase(accent);
  flush();
}

void LiveAppearanceSync::flush() {
  if (m_control == nullptr || !m_control->ready()) {
    return;
  }
  if (!m_scheme.empty() && m_control->value("appearance.scheme") != m_scheme) {
    m_control->set("appearance.scheme", m_scheme);
  }
  if (!m_accent.empty() && lowercase(m_control->value("appearance.accent")) != m_accent) {
    m_control->set("appearance.accent", m_accent);
  }
}

void LiveAppearanceSync::onSettingsChanged() {
  if (!m_control->ready()) {
    return;
  }
  const SettingsControl& c = *m_control;
  const LiveAppearance next{
      .scheme = c.value("appearance.scheme"),
      .accent = lowercase(c.value("appearance.accent")),
      .glassOpacity = parseUnit(c.value("appearance.glass_opacity"), 1.0F),
      .reduceTransparency = c.value("appearance.reduce_transparency") == "true",
      .increaseContrast = c.value("appearance.increase_contrast") == "true",
      .reduceMotion = c.value("appearance.reduce_motion") == "true",
  };
  flush();
  if (next == m_state) {
    return;
  }
  m_state = next;
  if (m_onChange) {
    m_onChange(m_state);
  }
}
