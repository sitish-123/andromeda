#include "system/activity_actions.h"

#include "dbus/system_interactive_call.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <sstream>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

  const sdbus::ServiceName kBusName{"org.freedesktop.systemd1"};
  const sdbus::ObjectPath kManagerPath{"/org/freedesktop/systemd1"};
  constexpr auto kManager = "org.freedesktop.systemd1.Manager";
  constexpr auto kOverrideMarker = "X-Andromeda-Override=true";

  bool deniedText(std::string_view error) {
    return error.contains("AccessDenied") || error.contains("InteractiveAuthorizationRequired") || error.contains("NotAuthorized")
           || error.contains("Dismissed");
  }

  ActivityActionResult fromError(std::string error) {
    ActivityActionResult result;
    result.accessDenied = deniedText(error);
    result.error = std::move(error);
    return result;
  }

  bool isAutostartUnit(const SystemdUnit& unit) { return unit.name.ends_with("@autostart.service"); }

  const AutostartEntry* autostartFor(const SystemdUnit& unit, const std::vector<AutostartEntry>& autostart) {
    const auto it = std::ranges::find(autostart, unit.name, &AutostartEntry::unitName);
    return it == autostart.end() ? nullptr : &*it;
  }

  std::string readAll(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }

  // Removes every "Hidden=" line of the [Desktop Entry] group, then inserts one when hidden.
  std::string withHidden(const std::string& text, bool hidden) {
    std::istringstream in(text);
    std::string out;
    std::string line;
    bool inGroup = false;
    while (std::getline(in, line)) {
      if (!line.empty() && line.front() == '[') {
        inGroup = line == "[Desktop Entry]";
        out += line + "\n";
        if (inGroup && hidden) {
          out += "Hidden=true\n";
        }
        continue;
      }
      if (inGroup && line.starts_with("Hidden=")) {
        continue;
      }
      out += line + "\n";
    }
    return out;
  }

  void callSystemInteractive(const char* method, GVariant* args, ActivityActions::Reply reply) {
    dbus::callInteractive(
        "org.freedesktop.systemd1", "/org/freedesktop/systemd1", kManager, method, args,
        [reply = std::move(reply)](GVariant* result, std::string error) {
          if (result == nullptr) {
            reply(fromError(std::move(error)));
            return;
          }
          reply(ActivityActionResult{.ok = true});
        }
    );
  }

  GVariant* unitFilesArgs(const std::string& name, bool withForce) {
    const char* names[] = {name.c_str(), nullptr};
    return withForce ? g_variant_new("(^asbb)", names, FALSE, FALSE) : g_variant_new("(^asb)", names, FALSE);
  }

} // namespace

bool setAutostartHidden(const std::string& userDir, const AutostartEntry& entry, bool hidden, std::string& error) {
  const fs::path target = fs::path(userDir) / (entry.id + ".desktop");
  std::error_code ec;
  const std::string current = readAll(target);
  const bool ours = current.contains(kOverrideMarker);
  if (!hidden) {
    if (ours) {
      fs::remove(target, ec);
    } else if (!current.empty()) {
      std::ofstream(target) << withHidden(current, false);
    }
    return true;
  }
  fs::create_directories(userDir, ec);
  std::string text;
  if (!current.empty()) {
    text = withHidden(current, true); // the user's own entry keeps everything but gains Hidden=true
  } else {
    text = "[Desktop Entry]\nType=Application\nName=" + entry.name + "\nHidden=true\n" + kOverrideMarker + "\n";
  }
  std::ofstream out(target);
  out << text;
  if (!out) {
    error = "Can't write " + target.string();
    return false;
  }
  return true;
}

ActivityActions::ActivityActions(sdbus::IConnection& userBus, sdbus::IConnection* systemBus)
    : m_userBus(userBus), m_systemBus(systemBus) {}

ActivityActions::~ActivityActions() = default;

sdbus::IProxy& ActivityActions::proxyFor(const SystemdUnit& unit, const std::string& path) {
  const std::string key = (unit.systemManager ? "s:" : "u:") + path;
  auto& proxy = m_proxies[key];
  if (proxy == nullptr) {
    proxy = sdbus::createProxy(unit.systemManager ? *m_systemBus : m_userBus, kBusName, sdbus::ObjectPath{path});
  }
  return *proxy;
}

void ActivityActions::stop(const SystemdUnit& unit, Reply reply) {
  if (unit.systemManager) {
    callSystemInteractive("StopUnit", g_variant_new("(ss)", unit.name.c_str(), "replace"), std::move(reply));
    return;
  }
  try {
    proxyFor(unit, kManagerPath).callMethodAsync("StopUnit").onInterface(kManager).withArguments(unit.name, std::string("replace")).uponReplyInvoke(
        [reply = std::move(reply)](const std::optional<sdbus::Error>& error, const sdbus::ObjectPath& /*job*/) {
          reply(error.has_value() ? fromError(error->getName() + ": " + error->getMessage()) : ActivityActionResult{.ok = true});
        }
    );
  } catch (const sdbus::Error& e) {
    reply(fromError(e.what()));
  }
}

void ActivityActions::forceQuit(const SystemdUnit& unit, Reply reply) {
  constexpr std::int32_t kSigKill = 9;
  if (unit.systemManager) {
    callSystemInteractive("KillUnit", g_variant_new("(ssi)", unit.name.c_str(), "all", kSigKill), std::move(reply));
    return;
  }
  try {
    proxyFor(unit, kManagerPath)
        .callMethodAsync("KillUnit")
        .onInterface(kManager)
        .withArguments(unit.name, std::string("all"), kSigKill)
        .uponReplyInvoke([reply = std::move(reply)](const std::optional<sdbus::Error>& error) {
          reply(error.has_value() ? fromError(error->getName() + ": " + error->getMessage()) : ActivityActionResult{.ok = true});
        });
  } catch (const sdbus::Error& e) {
    reply(fromError(e.what()));
  }
}

void ActivityActions::setFrozen(const SystemdUnit& unit, bool frozen, Reply reply) {
  const char* method = frozen ? "FreezeUnit" : "ThawUnit";
  if (unit.systemManager) {
    callSystemInteractive(method, g_variant_new("(s)", unit.name.c_str()), std::move(reply));
    return;
  }
  try {
    proxyFor(unit, kManagerPath)
        .callMethodAsync(method)
        .onInterface(kManager)
        .withArguments(unit.name)
        .uponReplyInvoke([reply = std::move(reply)](const std::optional<sdbus::Error>& error) {
          reply(error.has_value() ? fromError(error->getName() + ": " + error->getMessage()) : ActivityActionResult{.ok = true});
        });
  } catch (const sdbus::Error& e) {
    reply(fromError(e.what()));
  }
}

void ActivityActions::setLimits(const SystemdUnit& unit, std::uint64_t cpuQuotaPerSecUsec, std::uint64_t memoryHighBytes, Reply reply) {
  if (unit.systemManager) {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a(sv)"));
    g_variant_builder_add(&props, "(sv)", "CPUQuotaPerSecUSec", g_variant_new_uint64(cpuQuotaPerSecUsec));
    g_variant_builder_add(&props, "(sv)", "MemoryHigh", g_variant_new_uint64(memoryHighBytes));
    callSystemInteractive("SetUnitProperties", g_variant_new("(sba(sv))", unit.name.c_str(), TRUE, &props), std::move(reply));
    return;
  }
  const std::vector<sdbus::Struct<std::string, sdbus::Variant>> props{
      {std::string("CPUQuotaPerSecUSec"), sdbus::Variant(cpuQuotaPerSecUsec)},
      {std::string("MemoryHigh"), sdbus::Variant(memoryHighBytes)},
  };
  try {
    proxyFor(unit, kManagerPath)
        .callMethodAsync("SetUnitProperties")
        .onInterface(kManager)
        .withArguments(unit.name, true, props)
        .uponReplyInvoke([reply = std::move(reply)](const std::optional<sdbus::Error>& error) {
          reply(error.has_value() ? fromError(error->getName() + ": " + error->getMessage()) : ActivityActionResult{.ok = true});
        });
  } catch (const sdbus::Error& e) {
    reply(fromError(e.what()));
  }
}

void ActivityActions::disableAtLogin(
    const SystemdUnit& unit, const std::vector<AutostartEntry>& autostart, const std::string& autostartUserDir, Reply reply
) {
  if (isAutostartUnit(unit)) {
    const AutostartEntry* entry = autostartFor(unit, autostart);
    ActivityActionResult result;
    if (entry == nullptr) {
      result.error = "No autostart file for " + unit.name;
    } else {
      result.ok = setAutostartHidden(autostartUserDir, *entry, true, result.error);
    }
    reply(std::move(result));
    return;
  }
  if (unit.systemManager) {
    callSystemInteractive("DisableUnitFiles", unitFilesArgs(unit.name, false), [this, unit, reply](ActivityActionResult result) {
      if (result.ok) {
        checkOnDemand(unit, std::move(result), reply);
      } else {
        reply(std::move(result));
      }
    });
    return;
  }
  using Change = sdbus::Struct<std::string, std::string, std::string>;
  try {
    auto& manager = proxyFor(unit, kManagerPath);
    manager.callMethodAsync("DisableUnitFiles").onInterface(kManager).withArguments(std::vector<std::string>{unit.name}, false).uponReplyInvoke(
        [this, unit, reply, &manager](const std::optional<sdbus::Error>& error, const std::vector<Change>& /*changes*/) {
          if (error.has_value()) {
            reply(fromError(error->getName() + ": " + error->getMessage()));
            return;
          }
          // Reload so UnitFileState shows "disabled" at once; a user manager reload needs no authorization.
          manager.callMethodAsync("Reload").onInterface(kManager).uponReplyInvoke([this, unit, reply](const std::optional<sdbus::Error>&) {
            checkOnDemand(unit, ActivityActionResult{.ok = true}, reply);
          });
        }
    );
  } catch (const sdbus::Error& e) {
    reply(fromError(e.what()));
  }
}

void ActivityActions::enableAtLogin(
    const SystemdUnit& unit, const std::vector<AutostartEntry>& autostart, const std::string& autostartUserDir, Reply reply
) {
  if (isAutostartUnit(unit)) {
    const AutostartEntry* entry = autostartFor(unit, autostart);
    ActivityActionResult result;
    if (entry == nullptr) {
      result.error = "No autostart file for " + unit.name;
    } else {
      result.ok = setAutostartHidden(autostartUserDir, *entry, false, result.error);
    }
    reply(std::move(result));
    return;
  }
  if (unit.systemManager) {
    callSystemInteractive("EnableUnitFiles", unitFilesArgs(unit.name, true), std::move(reply));
    return;
  }
  using Change = sdbus::Struct<std::string, std::string, std::string>;
  try {
    auto& manager = proxyFor(unit, kManagerPath);
    manager.callMethodAsync("EnableUnitFiles").onInterface(kManager).withArguments(std::vector<std::string>{unit.name}, false, false).uponReplyInvoke(
        [reply, &manager](const std::optional<sdbus::Error>& error, bool /*carriesInstallInfo*/, const std::vector<Change>& /*changes*/) {
          if (error.has_value()) {
            reply(fromError(error->getName() + ": " + error->getMessage()));
            return;
          }
          manager.callMethodAsync("Reload").onInterface(kManager).uponReplyInvoke([reply](const std::optional<sdbus::Error>&) {
            reply(ActivityActionResult{.ok = true});
          });
        }
    );
  } catch (const sdbus::Error& e) {
    reply(fromError(e.what()));
  }
}

// A unit with TriggeredBy= (a socket, path or timer) or a D-Bus name can start again without its [Install] links.
void ActivityActions::checkOnDemand(const SystemdUnit& unit, ActivityActionResult result, Reply reply) {
  if ((unit.systemManager && m_systemBus == nullptr) || unit.objectPath.empty()) {
    reply(std::move(result));
    return;
  }
  using Properties = std::map<sdbus::PropertyName, sdbus::Variant>;
  auto& proxy = proxyFor(unit, unit.objectPath);
  proxy.getAllPropertiesAsync().onInterface("org.freedesktop.systemd1.Unit").uponReplyInvoke(
      [unit, result, reply, &proxy](const std::optional<sdbus::Error>& error, const Properties& props) {
        ActivityActionResult checked = result;
        if (!error.has_value()) {
          if (const auto it = props.find(sdbus::PropertyName{"TriggeredBy"}); it != props.end()) {
            checked.stillStartsOnDemand = !it->second.get<std::vector<std::string>>().empty();
          }
        }
        if (checked.stillStartsOnDemand || !unit.name.ends_with(".service")) {
          reply(std::move(checked));
          return;
        }
        proxy.getAllPropertiesAsync().onInterface("org.freedesktop.systemd1.Service").uponReplyInvoke(
            [checked, reply](const std::optional<sdbus::Error>& serviceError, const Properties& serviceProps) {
              ActivityActionResult final = checked;
              if (!serviceError.has_value()) {
                if (const auto it = serviceProps.find(sdbus::PropertyName{"BusName"}); it != serviceProps.end()) {
                  final.stillStartsOnDemand = !it->second.get<std::string>().empty();
                }
              }
              reply(std::move(final));
            }
        );
      }
  );
}
