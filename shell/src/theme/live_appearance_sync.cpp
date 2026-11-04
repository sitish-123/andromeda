#include "theme/live_appearance_sync.h"

#include "compositors/compositor_detect.h"
#include "compositors/settings_control.h"
#include "compositors/shell_wayland_connection.h"
#include "design_tokens.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

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

  // The compositor reports colors as #rrggbbaa; the shell uses #rrggbb for opaque ones. Compare in one form.
  std::string normalizeColor(std::string_view text) {
    std::string out = lowercase(text);
    if (out.size() == 9 && out[0] == '#' && out.ends_with("ff")) {
      out.resize(7);
    }
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
  m_accent = normalizeColor(accent);
  flush();
}

void LiveAppearanceSync::flush() {
  if (m_control == nullptr || !m_control->ready()) {
    return;
  }
  if (!m_scheme.empty()) {
    push("appearance.scheme", m_scheme, m_control->value("appearance.scheme") != m_scheme);
  }
  if (!m_accent.empty()) {
    push("appearance.accent", m_accent, normalizeColor(m_control->value("appearance.accent")) != m_accent);
  }
  if (!m_scheme.empty()) {
    const bool light = m_scheme == "light";
    const std::pair<const char*, float> recipe[] = {
        {"appearance.blur.saturation", light ? Tokens::blurLightSaturation : Tokens::blurSaturation},
        {"appearance.blur.brightness", light ? Tokens::blurLightBrightness : Tokens::blurBrightness},
        {"appearance.blur.contrast", light ? Tokens::blurLightContrast : Tokens::blurContrast},
    };
    for (const auto& [key, value] : recipe) {
      const std::string text = std::format("{:g}", value);
      push(key, text, m_control->value(key) != text);
    }
  }
}

// Sends each wanted value once: a refused set (failed event) notifies again, and resending would loop until the
// compositor's buffer to this client overflows.
void LiveAppearanceSync::push(const std::string& key, const std::string& value, bool differs) {
  if (!differs) {
    m_sent.erase(key);
    return;
  }
  if (const auto it = m_sent.find(key); it != m_sent.end() && it->second == value) {
    return;
  }
  m_sent.insert_or_assign(key, value);
  m_control->set(key, value);
}

void LiveAppearanceSync::onSettingsChanged() {
  if (!m_control->ready()) {
    return;
  }
  const SettingsControl& c = *m_control;
  const LiveAppearance next{
      .scheme = c.value("appearance.scheme"),
      .accent = normalizeColor(c.value("appearance.accent")),
      .glass = parseUnit(c.value("appearance.glass"), 0.5F),
      .reduceTransparency = c.value("appearance.reduce_transparency") == "true",
      .increaseContrast = c.value("appearance.increase_contrast") == "true",
      .reduceMotion = c.value("appearance.reduce_motion") == "true",
      .blurEnabled = c.value("appearance.blur.enabled") == "true",
  };
  // A new value that the shell did not publish is another client's choice: adopt it instead of writing ours back.
  // The first snapshot can be stale from an earlier session, so only a change after it counts.
  const bool external = !m_state.scheme.empty() && next.scheme != m_state.scheme && next.scheme != m_scheme
      && (next.scheme == "light" || next.scheme == "dark");
  if (external) {
    m_scheme = next.scheme;
  }
  flush();
  if (external && m_onExternalScheme) {
    m_onExternalScheme(next.scheme);
  }
  if (next == m_state) {
    return;
  }
  m_state = next;
  if (m_onChange) {
    m_onChange(m_state);
  }
}
