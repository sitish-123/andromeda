#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
} // namespace sdbus

// Client of org.freedesktop.locale1, bound only while the Language & Region settings page is open.
class LocaleService {
public:
  using ChangeCallback = std::function<void()>;

  explicit LocaleService(SystemBus& bus, ChangeCallback onChange = {});
  ~LocaleService();

  LocaleService(const LocaleService&) = delete;
  LocaleService& operator=(const LocaleService&) = delete;

  [[nodiscard]] bool ready() const noexcept { return m_ready; }
  // "LANG=…", "LC_TIME=…", etc.
  [[nodiscard]] const std::vector<std::string>& locale() const noexcept { return m_locale; }
  [[nodiscard]] const std::string& x11Layout() const noexcept { return m_x11Layout; }
  [[nodiscard]] const std::string& x11Model() const noexcept { return m_x11Model; }
  [[nodiscard]] const std::string& x11Variant() const noexcept { return m_x11Variant; }
  [[nodiscard]] const std::string& x11Options() const noexcept { return m_x11Options; }
  [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

  void setLocale(const std::vector<std::string>& assignments);
  // Locale names from /usr/share/i18n/SUPPORTED (glibc's own catalog), for the language picker.
  [[nodiscard]] static std::vector<std::string> supportedLocales();
  void setX11Keyboard(
      const std::string& layout, const std::string& model, const std::string& variant, const std::string& options,
      bool convert
  );

private:
  void refreshProperties();

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_proxy;
  ChangeCallback m_onChange;
  std::vector<std::string> m_locale;
  std::string m_x11Layout;
  std::string m_x11Model;
  std::string m_x11Variant;
  std::string m_x11Options;
  std::string m_lastError;
  bool m_ready = false;
};
