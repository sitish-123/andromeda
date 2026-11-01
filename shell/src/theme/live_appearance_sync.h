#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

class SettingsControl;
class ShellWaylandConnection;

// The compositor's live appearance keys (`appearance.*` in its settings table).
struct LiveAppearance {
  std::string scheme;
  std::string accent; // "#rrggbb"; empty when unset
  float glassOpacity = 1.0F;
  bool reduceTransparency = false;
  bool increaseContrast = false;
  bool reduceMotion = false;

  bool operator==(const LiveAppearance&) const = default;
};

// Keeps a dsk_settings_manager_v1 binding for the whole session on andromeda: reads the live appearance keys and
// writes the shell's scheme and accent.
class LiveAppearanceSync {
public:
  using ChangeCallback = std::function<void(const LiveAppearance&)>;

  LiveAppearanceSync();
  ~LiveAppearanceSync();

  void initialize(ShellWaylandConnection& wayland, ChangeCallback onChange);
  // Sets each key the compositor holds with a different value; before the first batch arrives, it waits for it.
  void publish(std::string_view scheme, std::string_view accent);
  // Runs when another client (Settings, `andromeda msg settings-set`) changes appearance.scheme; the shell adopts it.
  void setOnExternalScheme(std::function<void(std::string_view)> callback) { m_onExternalScheme = std::move(callback); }

  [[nodiscard]] const LiveAppearance& state() const noexcept { return m_state; }

private:
  void onSettingsChanged();
  void flush();

  std::unique_ptr<SettingsControl> m_control;
  LiveAppearance m_state;
  std::string m_scheme;
  std::string m_accent;
  ChangeCallback m_onChange;
  std::function<void(std::string_view)> m_onExternalScheme;
};
