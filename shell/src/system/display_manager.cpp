#include "system/display_manager.h"

#include "dbus/system_interactive_call.h"

#include <algorithm>
#include <filesystem>
#include <glib.h>
#include <utility>

namespace display_manager {

  namespace {

    constexpr auto kGreeterConfig = "/usr/share/noctalia-greeter/greetd.toml";
    constexpr auto kAlias = "/etc/systemd/system/display-manager.service";
    constexpr const char* kOthers[] = {"gdm3.service", "gdm.service", "sddm.service", "lightdm.service"};

    // The unit's real file name: Ubuntu's gdm3.service is a link to gdm.service, which display-manager.service
    // then points at. Empty when the unit is not installed.
    std::string realUnit(const std::string& unit) {
      std::error_code ec;
      for (const char* dir : {"/usr/lib/systemd/system/", "/lib/systemd/system/", "/etc/systemd/system/"}) {
        const std::filesystem::path path = std::string(dir) + unit;
        if (std::filesystem::exists(path, ec)) {
          const auto real = std::filesystem::canonical(path, ec);
          return ec ? unit : real.filename().string();
        }
      }
      return {};
    }

  } // namespace

  std::string nameOf(const std::string& unit) {
    if (unit == "greetd.service") {
      return "Andromeda";
    }
    if (unit == "gdm.service" || unit == "gdm3.service") {
      return "GDM";
    }
    if (unit == "sddm.service") {
      return "SDDM";
    }
    if (unit == "lightdm.service") {
      return "LightDM";
    }
    return unit.substr(0, unit.rfind('.'));
  }

  std::vector<Entry> installed() {
    std::vector<Entry> out;
    std::error_code ec;
    if (std::filesystem::exists(kGreeterConfig, ec)) {
      out.push_back({.unit = "greetd.service", .name = nameOf("greetd.service")});
    }
    for (const char* unit : kOthers) {
      const std::string real = realUnit(unit);
      if (!real.empty() && std::ranges::find(out, real, &Entry::unit) == out.end()) {
        out.push_back({.unit = real, .name = nameOf(real)});
      }
    }
    return out;
  }

  std::string activeUnit() {
    std::error_code ec;
    const auto target = std::filesystem::read_symlink(kAlias, ec);
    return ec ? std::string{} : target.filename().string();
  }

  void switchTo(const std::string& unit, std::function<void(bool ok, bool denied, std::string error)> reply) {
    const char* names[] = {unit.c_str(), nullptr};
    dbus::callInteractive(
        "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "EnableUnitFiles",
        g_variant_new("(^asbb)", names, FALSE, TRUE),
        [reply = std::move(reply)](GVariant* result, std::string error) {
          const bool denied = error.contains("AccessDenied") || error.contains("NotAuthorized")
                              || error.contains("Dismissed") || error.contains("InteractiveAuthorizationRequired");
          reply(result != nullptr, denied, std::move(error));
        }
    );
  }

} // namespace display_manager
