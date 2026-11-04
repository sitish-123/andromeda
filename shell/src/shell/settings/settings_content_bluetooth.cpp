#include "shell/settings/settings_content_bluetooth.h"

#include "dbus/bluetooth/bluetooth_service.h"
#include "i18n/i18n.h"
#include "shell/control_center/tabs/bluetooth_tab.h"
#include "shell/settings/settings_page_rows.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>
#include <format>

namespace settings {
  namespace {

    using namespace page_rows;

    // Detail ids carry the alias for the title and the object path to find the device again.
    std::string detailId(const BluetoothDeviceInfo& device) {
      return "bt:" + (device.alias.empty() ? device.address : device.alias) + "\t" + device.path;
    }

    std::string statusText(const BluetoothDeviceInfo& device) {
      if (device.connecting) {
        return i18n::tr("settings.bluetooth.connecting");
      }
      std::string text = i18n::tr(device.connected ? "settings.bluetooth.connected" : "settings.bluetooth.not-connected");
      if (device.connected && device.hasBattery) {
        text += std::format(" · {} %", device.batteryPercent);
      }
      return text;
    }

    void addDeviceRow(Flex& card, const BluetoothDeviceInfo& device, const SettingsBluetoothContext& ctx) {
      const float scale = ctx.scale;
      BluetoothService* bt = ctx.bluetooth;
      auto r = row(scale);
      r->addChild(symbol(bluetoothDeviceGlyph(device.kind), Tokens::sizeSymbol * scale, ColorRole::Label2));
      r->addChild(title(device.alias.empty() ? device.address : device.alias, scale));
      if (device.paired) {
        r->addChild(ui::label({.text = statusText(device), .fontSize = Style::fontSizeBody * scale,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
        if (!device.connected && !device.connecting) {
          r->addChild(plainButton(i18n::tr("settings.bluetooth.connect"), ButtonVariant::Ghost, scale,
                                  [bt, path = device.path] { bt->connect(path); }));
        }
        r->addChild(iconButton("chevron-right", i18n::tr("settings.wifi.details"), scale,
                               [open = ctx.openDetail, id = detailId(device)] { open(id); }));
      } else {
        r->addChild(plainButton(i18n::tr("settings.bluetooth.connect"), ButtonVariant::Ghost, scale,
                                [bt, path = device.path] { bt->pair(path); }));
      }
      card.addChild(std::move(r));
    }

    void addDetail(Flex& content, const SettingsBluetoothContext& ctx) {
      const float scale = ctx.scale;
      const auto tab = ctx.detail.find('\t');
      const std::string path = tab == std::string::npos ? std::string{} : ctx.detail.substr(tab + 1);
      const auto& devices = ctx.bluetooth->devices();
      const auto it = std::ranges::find(devices, path, &BluetoothDeviceInfo::path);
      if (it == devices.end()) {
        addEmptyState(content, "bluetooth-off", i18n::tr("settings.bluetooth.gone"), scale);
        return;
      }
      const BluetoothDeviceInfo& device = *it;
      Flex* card = addCard(content, scale);
      addValueRow(*card, i18n::tr("settings.wifi.status"), statusText(device), scale);
      addValueRow(*card, i18n::tr("settings.bluetooth.address"), device.address, scale);
      if (device.hasBattery) {
        addValueRow(*card, i18n::tr("settings.bluetooth.battery"), std::format("{} %", device.batteryPercent), scale);
      }
      BluetoothService* bt = ctx.bluetooth;
      auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .paddingH = Style::spaceLg * scale});
      if (device.connected) {
        actions->addChild(plainButton(i18n::tr("settings.wifi.disconnect"), ButtonVariant::Default, scale,
                                      [bt, path] { bt->disconnectDevice(path); }));
      } else {
        actions->addChild(plainButton(i18n::tr("settings.bluetooth.connect"), ButtonVariant::Default, scale,
                                      [bt, path] { bt->connect(path); }));
      }
      actions->addChild(plainButton(i18n::tr("settings.bluetooth.forget"), ButtonVariant::Destructive, scale,
                                    [bt, path, back = ctx.back] {
                                      bt->forget(path);
                                      back();
                                    }));
      content.addChild(std::move(actions));
    }

  } // namespace

  void addSettingsBluetooth(Flex& content, const SettingsBluetoothContext& ctx) {
    const float scale = ctx.scale;
    if (ctx.bluetooth == nullptr || !ctx.bluetooth->state().adapterPresent) {
      addEmptyState(content, "bluetooth-off", i18n::tr("settings.bluetooth.unavailable"), scale);
      return;
    }
    if (ctx.detail.starts_with("bt:")) {
      addDetail(content, ctx);
      return;
    }
    const BluetoothState& state = ctx.bluetooth->state();
    BluetoothService* bt = ctx.bluetooth;
    addHero(
        content,
        Hero{
            .glyph = state.powered ? "bluetooth" : "bluetooth-off",
            .on = state.powered,
            .title = i18n::tr("settings.pages.bluetooth"),
            .status = state.powered
                ? i18n::tr("settings.bluetooth.discoverable-as", "name", state.adapterName)
                : i18n::tr(state.rfkillSoftBlocked ? "settings.bluetooth.blocked" : "settings.wifi.off"),
            .onToggle = [bt](bool on) { bt->setPowered(on); },
        },
        scale
    );
    if (!state.powered) {
      return;
    }

    std::vector<BluetoothDeviceInfo> mine;
    std::vector<BluetoothDeviceInfo> nearby;
    for (const auto& device : ctx.bluetooth->devices()) {
      (device.paired ? mine : nearby).push_back(device);
    }
    std::ranges::stable_sort(mine, std::greater<>{}, &BluetoothDeviceInfo::connected);

    if (!mine.empty()) {
      addHeader(content, i18n::tr("settings.bluetooth.mine"), nullptr, scale);
      Flex* card = addCard(content, scale);
      for (const auto& device : mine) {
        addDeviceRow(*card, device, ctx);
      }
    }

    std::unique_ptr<Node> trailing;
    if (state.discovering) {
      trailing = ui::spinner({.spinnerSize = Tokens::sizeSymbolInline * scale, .spinning = true});
    } else {
      trailing = iconButton("refresh", i18n::tr("settings.wifi.scan"), scale, [bt] { bt->startDiscovery(); });
    }
    addHeader(content, i18n::tr("settings.bluetooth.nearby"), std::move(trailing), scale);
    Flex* card = addCard(content, scale);
    // Unnamed devices are address-only noise; macOS hides them too.
    std::erase_if(nearby, [](const BluetoothDeviceInfo& d) { return d.alias.empty() || d.alias == d.address; });
    if (nearby.empty()) {
      auto r = row(scale);
      r->addChild(ui::label({.text = i18n::tr(state.discovering ? "settings.wifi.searching" : "settings.bluetooth.none"),
                             .fontSize = Style::fontSizeBody * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
      card->addChild(std::move(r));
      return;
    }
    for (const auto& device : nearby) {
      addDeviceRow(*card, device, ctx);
    }
  }

} // namespace settings
