#include "shell/settings/settings_content_about.h"

#include "core/build_info.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>

namespace settings {
  namespace {

    using namespace page_rows;

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

    // Free and total space of the file system that holds the home folder.
    std::string storageText() {
      struct statvfs fs{};
      const char* home = std::getenv("HOME");
      if (statvfs(home != nullptr ? home : "/", &fs) != 0 || fs.f_blocks == 0) {
        return {};
      }
      constexpr double kGb = 1000.0 * 1000.0 * 1000.0;
      const double total = static_cast<double>(fs.f_blocks) * static_cast<double>(fs.f_frsize) / kGb;
      const double free = static_cast<double>(fs.f_bavail) * static_cast<double>(fs.f_frsize) / kGb;
      return i18n::tr("settings.about.storage-free", "free", std::format("{:.0f}", free), "total",
                      std::format("{:.0f}", total));
    }

    std::string readLine(const std::filesystem::path& path) {
      std::ifstream in(path);
      std::string line;
      std::getline(in, line);
      return line;
    }

    // The boot GPU's PCI device (boot_vga 1), else the first DRM card with PCI ids.
    std::filesystem::path graphicsDevice() {
      std::error_code ec;
      std::filesystem::path first;
      for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
        const std::string name = entry.path().filename().string();
        const std::filesystem::path dev = entry.path() / "device";
        if (!name.starts_with("card") || name.find('-') != std::string::npos || readLine(dev / "vendor").empty()) {
          continue;
        }
        if (readLine(dev / "boot_vga") == "1") {
          return dev;
        }
        if (first.empty() || name < first.parent_path().filename().string()) {
          first = dev;
        }
      }
      return first;
    }

    // "Intel Arc Pro 130T/140T (xe)" from the GPU's PCI ids, hwdata's pci.ids and the kernel driver.
    std::string graphicsText() {
      const std::filesystem::path dev = graphicsDevice();
      if (dev.empty()) {
        return {};
      }
      std::string vendorId = readLine(dev / "vendor");
      std::string deviceId = readLine(dev / "device");
      if (vendorId.size() < 6 || deviceId.size() < 6) {
        return {};
      }
      vendorId = vendorId.substr(2);
      deviceId = deviceId.substr(2);
      std::string vendor;
      std::string device;
      std::ifstream ids("/usr/share/hwdata/pci.ids");
      bool inVendor = false;
      for (std::string line; std::getline(ids, line);) {
        if (line.empty() || line.front() == '#') {
          continue;
        }
        if (line.front() != '\t') {
          if (inVendor) {
            break;
          }
          inVendor = line.starts_with(vendorId + "  ");
          if (inVendor) {
            vendor = line.substr(vendorId.size() + 2);
          }
        } else if (inVendor && line.size() > 1 && line[1] != '\t' && line.substr(1).starts_with(deviceId + "  ")) {
          device = line.substr(deviceId.size() + 3);
          break;
        }
      }
      if (const auto open = device.find('['); open != std::string::npos && device.back() == ']') {
        device = device.substr(open + 1, device.size() - open - 2);
      }
      vendor = vendor.substr(0, vendor.find(' '));
      std::string out = device.empty() ? vendor : (device.starts_with(vendor) ? device : vendor + " " + device);
      std::ifstream uevent(dev / "uevent");
      for (std::string line; std::getline(uevent, line);) {
        if (line.starts_with("DRIVER=")) {
          out += (out.empty() ? "" : " ") + std::string("(") + line.substr(7) + ")";
        }
      }
      return out;
    }

    // Built-in panel first, else the first connected output: "14″ · 2880 × 1800" (size from the EDID, in cm).
    std::string displayText() {
      std::error_code ec;
      std::filesystem::path pick;
      for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.find('-') == std::string::npos || readLine(entry.path() / "status") != "connected") {
          continue;
        }
        if (pick.empty() || name.find("-eDP-") != std::string::npos || name.find("-LVDS-") != std::string::npos) {
          pick = entry.path();
        }
      }
      if (pick.empty()) {
        return {};
      }
      std::string mode = readLine(pick / "modes");
      if (const auto x = mode.find('x'); x != std::string::npos) {
        mode = mode.substr(0, x) + " \u00d7 " + mode.substr(x + 1);
      }
      std::ifstream edid(pick / "edid", std::ios::binary);
      char head[23]{};
      if (edid.read(head, sizeof(head)) && head[21] > 0 && head[22] > 0) {
        const double w = static_cast<unsigned char>(head[21]);
        const double h = static_cast<unsigned char>(head[22]);
        const std::string inches = std::format("{:.0f}\u2033", std::sqrt(w * w + h * h) / 2.54);
        return mode.empty() ? inches : inches + " \u00b7 " + mode;
      }
      return mode;
    }

    void addFact(Flex& card, std::string& clip, std::string_view glyph, const char* key, std::string value,
                 float scale) {
      if (value.empty()) {
        return;
      }
      const std::string label = i18n::tr(key);
      clip += label + ": " + value + "\n";
      addInfoRow(card, glyph, label, std::move(value), scale);
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

  AboutHardware readAboutHardware() {
    return {.processor = processorName(), .graphics = graphicsText(), .memory = memoryText(), .storage = storageText(),
            .display = displayText()};
  }

  void addSettingsAbout(Flex& content, const SettingsAboutContext& ctx) {
    const float scale = ctx.scale;
    static const AboutFacts kNoFacts;
    const AboutFacts& facts = ctx.facts != nullptr ? *ctx.facts : kNoFacts;
    const auto& props = facts.hostname;
    const auto prop = [&](const char* key) {
      const auto it = props.find(key);
      return it == props.end() ? std::string{} : it->second;
    };
    std::string name = prop("PrettyHostname");
    if (name.empty()) {
      name = prop("StaticHostname").empty() ? prop("Hostname") : prop("StaticHostname");
    }
    if (char host[256]{}; name.empty() && gethostname(host, sizeof(host) - 1) == 0) {
      name = host;
    }
    std::string model = prop("HardwareVendor");
    if (const std::string hw = prop("HardwareModel"); !hw.empty()) {
      model = model.empty() ? hw : model + " " + hw;
    }
    std::string clip = name + "\n" + (model.empty() ? std::string{} : model + "\n");

    constexpr float kTile = 48.0F;
    constexpr float kTileGlyph = 32.0F;
    Flex* hero = addCard(content, scale);
    auto r = row(scale);
    r->setGap(Style::spaceLg * scale);
    r->setMinHeight(Tokens::spaceP64 * scale);
    r->addChild(ui::row(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::Center,
         .fill = colorSpecFromRole(ColorRole::Fill1),
         .radius = Tokens::radiusR8 * scale,
         .width = kTile * scale,
         .height = kTile * scale},
        symbol(prop("Chassis") == "desktop" ? "device-desktop" : "device-laptop", kTileGlyph * scale, ColorRole::Label2)
    ));
    auto copy = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
    copy->addChild(ui::label({.text = name, .fontSize = Tokens::typeTitle3.size * scale, .fontWeight = FontWeight::SemiBold,
                              .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}));
    std::string sub = ctx.renaming ? i18n::tr("settings.about.renaming") : !ctx.renameError.empty() ? ctx.renameError : model;
    if (!sub.empty()) {
      copy->addChild(ui::label({.text = std::move(sub), .fontSize = Tokens::typeFootnote.size * scale,
                                .color = colorSpecFromRole(ctx.renameError.empty() || ctx.renaming ? ColorRole::Label2
                                                                                                   : ColorRole::DangerText),
                                .maxLines = 2}));
    }
    r->addChild(std::move(copy));
    if (ctx.openRename && !props.empty() && !ctx.renaming) {
      r->addChild(plainButton(i18n::tr("settings.about.rename"), ButtonVariant::Default, scale,
                              [open = ctx.openRename, name] { open(name); }));
    }
    hero->addChild(std::move(r));

    Flex* hardware = addSettingsGroupCard(content, i18n::tr("settings.about.hardware"), scale);
    if (facts.hardware.has_value()) {
      const AboutHardware& hw = *facts.hardware;
      addFact(*hardware, clip, "cpu", "settings.about.processor", hw.processor, scale);
      addFact(*hardware, clip, "device-desktop", "settings.about.graphics", hw.graphics, scale);
      addFact(*hardware, clip, "cpu", "settings.about.memory", hw.memory, scale);
      addFact(*hardware, clip, "database", "settings.about.storage", hw.storage, scale);
      addFact(*hardware, clip, "device-desktop", "settings.about.display", hw.display, scale);
    } else {
      addInfoRow(*hardware, "", i18n::tr("settings.services-page.loading"), {}, scale);
    }
    addSettingsRowSeparators(*hardware, scale);

    utsname uts{};
    const std::string kernel = prop("KernelRelease").empty() && uname(&uts) == 0 ? uts.release : prop("KernelRelease");
    std::string system = prop("OperatingSystemPrettyName");
    if (system.empty()) {
      system = i18n::tr("settings.about.system");
    }
    Flex* software = addSettingsGroupCard(content, i18n::tr("settings.about.software"), scale);
    const std::string version = noctalia::build_info::displayVersion();
    clip += "Andromeda: " + version + "\n" + system + ": " + kernel + "\n";
    addInfoRow(*software, "info-circle", "Andromeda", version, scale);
    addInfoRow(*software, "", system, kernel.empty() ? std::string{} : i18n::tr("settings.about.kernel-value", "version", kernel),
               scale);
    if (ctx.openSoftwareUpdate) {
      addInfoRow(*software, "download", i18n::tr("settings.pages.software-update"), ctx.updateStatus, scale,
                 ctx.openSoftwareUpdate);
    }
    addSettingsRowSeparators(*software, scale);

    auto actions = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale,
                            .fillWidth = true});
    if (ctx.openLegal) {
      actions->addChild(plainButton(i18n::tr("settings.about.legal"), ButtonVariant::Default, scale, ctx.openLegal));
    }
    if (ctx.copyInfo) {
      actions->addChild(plainButton(i18n::tr("settings.about.copy-info"), ButtonVariant::Default, scale,
                                    [copyInfo = ctx.copyInfo, clip] { copyInfo(clip); }));
    }
    content.addChild(std::move(actions));
  }

} // namespace settings
