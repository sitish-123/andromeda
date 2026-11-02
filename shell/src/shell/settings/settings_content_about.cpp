#include "shell/settings/settings_content_about.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/style.h"

#include <gio/gio.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <string>

namespace settings {
  namespace {

    using namespace page_rows;

    // hostname1 properties; empty when hostnamed is missing.
    std::map<std::string, std::string> hostnameProperties() {
      std::map<std::string, std::string> out;
      GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
      if (bus == nullptr) {
        return out;
      }
      GVariant* reply = g_dbus_connection_call_sync(
          bus, "org.freedesktop.hostname1", "/org/freedesktop/hostname1", "org.freedesktop.DBus.Properties", "GetAll",
          g_variant_new("(s)", "org.freedesktop.hostname1"), G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 2000,
          nullptr, nullptr
      );
      if (reply != nullptr) {
        GVariantIter* iter = nullptr;
        g_variant_get(reply, "(a{sv})", &iter);
        const char* key = nullptr;
        GVariant* value = nullptr;
        while (g_variant_iter_loop(iter, "{&sv}", &key, &value)) {
          if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
            out[key] = g_variant_get_string(value, nullptr);
          }
        }
        g_variant_iter_free(iter);
        g_variant_unref(reply);
      }
      g_object_unref(bus);
      return out;
    }

    std::string processorName() {
      std::ifstream cpuinfo("/proc/cpuinfo");
      for (std::string line; std::getline(cpuinfo, line);) {
        if (line.starts_with("model name")) {
          const auto colon = line.find(':');
          return colon == std::string::npos ? std::string{} : line.substr(line.find_first_not_of(' ', colon + 1));
        }
      }
      return {};
    }

    std::string memoryText() {
      const double bytes = static_cast<double>(sysconf(_SC_PHYS_PAGES)) * static_cast<double>(sysconf(_SC_PAGESIZE));
      return bytes > 0.0 ? std::format("{:.0f} GB", std::round(bytes / (1024.0 * 1024.0 * 1024.0))) : std::string{};
    }

    void addFact(Flex& card, const char* key, const std::string& value, float scale) {
      if (!value.empty()) {
        addValueRow(card, i18n::tr(key), value, scale);
      }
    }

  } // namespace

  std::string hostnameFromName(const std::string& name) {
    constexpr std::size_t kMaxHostname = 63;
    std::string out;
    for (const char c : name) {
      const auto u = static_cast<unsigned char>(c);
      if (std::isalnum(u) != 0 && u < 0x80U) {
        out.push_back(static_cast<char>(std::tolower(u)));
      } else if ((c == ' ' || c == '-' || c == '_' || c == '.') && !out.empty() && out.back() != '-') {
        out.push_back('-');
      }
    }
    while (!out.empty() && out.back() == '-') {
      out.pop_back();
    }
    return out.substr(0, kMaxHostname);
  }

  void addSettingsAbout(Flex& content, const SettingsAboutContext& ctx) {
    const float scale = ctx.scale;
    const auto props = hostnameProperties();
    const auto prop = [&](const char* key) {
      const auto it = props.find(key);
      return it == props.end() ? std::string{} : it->second;
    };
    std::string name = prop("PrettyHostname");
    if (name.empty()) {
      name = prop("StaticHostname").empty() ? prop("Hostname") : prop("StaticHostname");
    }

    Flex* nameCard = addCard(content, scale);
    auto r = row(scale);
    r->addChild(title(i18n::tr("settings.about.name"), scale));
    constexpr float kFieldWidth = 240.0F;
    r->addChild(ui::input({
        .value = name,
        .enabled = !ctx.renaming && !props.empty(),
        .width = kFieldWidth * scale,
        .onSubmit = [rename = ctx.rename, name](const std::string& text) {
          if (!text.empty() && text != name) {
            rename(text);
          }
        },
    }));
    nameCard->addChild(std::move(r));
    if (ctx.renaming || !ctx.renameError.empty()) {
      auto status = row(scale);
      status->addChild(ui::label({
          .text = ctx.renaming ? i18n::tr("settings.users.creating") : ctx.renameError,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ctx.renaming ? ColorRole::Label2 : ColorRole::DangerText),
          .maxLines = 2,
          .flexGrow = 1.0F,
      }));
      nameCard->addChild(std::move(status));
    }

    std::string model = prop("HardwareVendor");
    if (const std::string hw = prop("HardwareModel"); !hw.empty()) {
      model = model.empty() ? hw : model + " " + hw;
    }
    utsname uts{};
    const std::string kernel = prop("KernelRelease").empty() && uname(&uts) == 0 ? uts.release : prop("KernelRelease");

    Flex* facts = addCard(content, scale);
    addFact(*facts, "settings.about.model", model, scale);
    addFact(*facts, "settings.about.processor", processorName(), scale);
    addFact(*facts, "settings.about.memory", memoryText(), scale);
    addFact(*facts, "settings.about.system", prop("OperatingSystemPrettyName"), scale);
    addFact(*facts, "settings.about.kernel", kernel, scale);
  }

} // namespace settings
