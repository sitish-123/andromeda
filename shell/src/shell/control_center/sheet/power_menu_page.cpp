#include "shell/control_center/sheet/power_menu_page.h"

#include "config/config_service.h"
#include "dbus/andromeda/software_update_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "render/core/async_texture_cache.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "shell/profile/avatar_path.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_guard.h"
#include "system/distro_info.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <cctype>
#include <cmath>
#include <format>

namespace {

  constexpr float kAvatar = 40.0F;
  // The bottom row is 40 high (mockup power-menu.html section 0); the four tiles are taller with a round icon.
  constexpr float kTile = 56.0F;
  constexpr float kRowTile = 40.0F;
  // DESIGN.md: the update tile hides on battery below 30 % while not charging.
  constexpr double kUpdateMinBattery = 30.0;

} // namespace

PowerMenuPage::PowerMenuPage(const ControlCenterServices& services) : m_services(services) {}

std::unique_ptr<Flex> PowerMenuPage::create() {
  const float s = contentScale();
  const float gap = Tokens::componentTileGap * s;
  auto tab = ui::column({.out = &m_rootLayout, .align = FlexAlign::Stretch, .gap = gap});

  const std::string name = sessionDisplayName();
  // The initial on an accent circle shows until (or unless) the account picture loads over it.
  auto avatar = ui::row(
      {.align = FlexAlign::Center, .justify = FlexJustify::Center, .fill = colorSpecFromRole(ColorRole::Accent),
       .radius = kAvatar * 0.5F * s, .width = kAvatar * s, .height = kAvatar * s},
      ui::label({.text = name.empty() ? std::string{} : std::string(1, static_cast<char>(std::toupper(
                                                                           static_cast<unsigned char>(name.front())))),
                 .fontSize = Tokens::typeHeadline.size * s,
                 .fontWeight = static_cast<FontWeight>(Style::glassWeight(Tokens::typeHeadline.weight)),
                 .color = colorSpecFromRole(ColorRole::OnAccent), .maxLines = 1})
  );
  avatar->addChild(ui::image({
      .out = &m_avatar,
      .fit = ImageFit::Cover,
      .radius = kAvatar * 0.5F * s,
      .width = kAvatar * s,
      .height = kAvatar * s,
      .participatesInLayout = false,
      .configure = [](Image& image) { image.setAsyncReadyCallback([] { PanelManager::instance().refresh(); }); },
  }));
  tab->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingV = Tokens::spaceP4 * s,
       .paddingH = Tokens::spaceP4 * s},
      std::move(avatar),
      ui::column(
          {.align = FlexAlign::Start, .flexGrow = 1.0F},
          ui::label({.text = name, .fontSize = Tokens::typeHeadline.size * s,
                     .fontWeight = static_cast<FontWeight>(Style::glassWeight(Tokens::typeHeadline.weight)),
                     .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}),
          ui::label({.out = &m_subtitle, .fontSize = Tokens::typeFootnote.size * s,
                     .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 1})
      )
  ));

  const auto pair = [&](std::unique_ptr<Flex> a, std::unique_ptr<Flex> b) {
    auto row = ui::row({.align = FlexAlign::Stretch, .gap = gap});
    a->setFlexGrow(1.0F);
    b->setFlexGrow(1.0F);
    row->addChild(std::move(a));
    row->addChild(std::move(b));
    return row;
  };
  tab->addChild(makeUpdateTile());
  tab->addChild(makeBlockerRow());
  tab->addChild(pair(
      makeTile(session_action::defaultGlyph("lock"), i18n::tr("control-center.power-menu.lock"), "lock", kTile, true),
      makeTile("moon", i18n::tr("control-center.power-menu.sleep"), "suspend", kTile, true)
  ));
  tab->addChild(pair(
      makeTile(session_action::defaultGlyph("reboot"), i18n::tr("control-center.power-menu.restart"), "reboot", kTile,
               true, &m_restartLabel),
      makeTile(session_action::defaultGlyph("shutdown"), i18n::tr("control-center.power-menu.shut-down"), "shutdown",
               kTile, true, &m_shutdownLabel)
  ));
  // Switch User joins this row once greetd can start a second session; until then Log Out spans it.
  tab->addChild(makeTile(session_action::defaultGlyph("logout"), i18n::tr("control-center.power-menu.log-out"),
                         "logout", kRowTile, false));
  if (m_services.sessionGuard != nullptr && !m_watching) {
    m_services.sessionGuard->watch(true);
    m_watching = true;
  }
  syncState();
  return tab;
}

bool PowerMenuPage::updateOffered() const {
  SoftwareUpdateService* updates = m_services.softwareUpdates ? m_services.softwareUpdates() : nullptr;
  if (updates == nullptr) {
    return false;
  }
  if (m_services.upower != nullptr) {
    const UPowerState& power = m_services.upower->state();
    if (power.isPresent && power.state != BatteryState::Charging && power.percentage < kUpdateMinBattery) {
      return false;
    }
  }
  const bool install = updates->available() && updates->canInstall() && !updates->newsBlocksSystem();
  return install || updates->restartNeeded();
}

std::unique_ptr<Flex> PowerMenuPage::makeUpdateTile() {
  const float s = contentScale();
  auto button = std::make_unique<Button>();
  button->setVariant(ButtonVariant::Primary);
  button->setRadius(Tokens::componentTileRadius * s);
  button->setControlHeight(kRowTile * s);
  button->setGap(Tokens::spaceP8 * s);
  button->setContentAlign(ButtonContentAlign::Center);
  button->setDirection(FlexDirection::Horizontal);
  button->setAlign(FlexAlign::Center);
  auto glyph = ui::glyph({.glyph = "refresh", .glyphSize = Tokens::componentRoundIconGlyph * s,
                          .color = colorSpecFromRole(ColorRole::OnAccent)});
  glyph->setHitTestVisible(false);
  button->addChild(std::move(glyph));
  auto text = ui::label({.out = &m_updateLabel, .fontSize = Tokens::typeHeadline.size * s,
                         .fontWeight = static_cast<FontWeight>(Style::glassWeight(Tokens::typeHeadline.weight)),
                         .color = colorSpecFromRole(ColorRole::OnAccent), .maxLines = 1});
  text->setHitTestVisible(false);
  button->addChild(std::move(text));
  button->setOnClick([this] {
    SoftwareUpdateService* updates = m_services.softwareUpdates ? m_services.softwareUpdates() : nullptr;
    if (updates == nullptr || m_services.sessionGuard == nullptr || m_services.sessionGuard->updating()) {
      return;
    }
    PanelManager::instance().closePanel();
    if (updates->available() && updates->canInstall() && !updates->newsBlocksSystem()) {
      m_services.sessionGuard->updateAndRestart(*updates);
    } else {
      m_services.sessionGuard->queue("reboot");
    }
  });
  auto row = ui::row({.out = &m_updateRow, .align = FlexAlign::Stretch});
  button->setFlexGrow(1.0F);
  row->addChild(std::move(button));
  return row;
}

std::unique_ptr<Flex> PowerMenuPage::makeBlockerRow() {
  const float s = contentScale();
  const float iconSize = Tokens::componentRoundIconSize * s;
  // A block inhibitor (pacman, firmware, a format) is a live row: it names who holds the machine and why.
  return ui::row(
      {.out = &m_blockerRow, .align = FlexAlign::Center, .gap = Tokens::spaceP12 * s, .paddingV = Tokens::spaceP8 * s,
       .paddingH = Tokens::componentTilePadding * s,
       .fill = colorSpecFromRole(ColorRole::GlassCard, panelCardOpacity()), .radius = Tokens::componentTileRadius * s},
      ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center,
               .fill = colorSpecFromRole(ColorRole::Fill2), .radius = iconSize * 0.5F, .width = iconSize,
               .height = iconSize},
              ui::glyph({.glyph = "package", .glyphSize = Tokens::componentRoundIconGlyph * s,
                         .color = colorSpecFromRole(ColorRole::Label1)})),
      ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F},
                 ui::label({.out = &m_blockerTitle, .fontSize = Tokens::typeHeadline.size * s,
                            .fontWeight = static_cast<FontWeight>(Style::glassWeight(Tokens::typeHeadline.weight)),
                            .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1}),
                 ui::label({.out = &m_blockerBody, .fontSize = Tokens::typeFootnote.size * s,
                            .color = colorSpecFromRole(ColorRole::Label2), .maxLines = 2}))
  );
}

void PowerMenuPage::syncState() {
  const SessionGuard* guard = m_services.sessionGuard;
  const bool blocked = guard != nullptr && !guard->blockers().empty();
  if (m_blockerRow != nullptr) {
    m_blockerRow->setVisible(blocked);
    if (blocked) {
      const SessionGuard::Blocker& first = guard->blockers().front();
      m_blockerTitle->setText(first.who);
      m_blockerBody->setText(guard->blockers().size() > 1
                                 ? i18n::tr("control-center.power-menu.blocked-more", "why", first.why, "count",
                                            std::to_string(guard->blockers().size() - 1))
                                 : first.why);
    }
  }
  if (m_updateRow != nullptr) {
    const bool offered = updateOffered();
    m_updateRow->setVisible(offered);
    if (offered && m_updateLabel != nullptr) {
      SoftwareUpdateService* updates = m_services.softwareUpdates();
      const bool install = updates->available() && updates->canInstall() && !updates->newsBlocksSystem();
      m_updateLabel->setText(i18n::tr(guard != nullptr && guard->updating() ? "control-center.power-menu.updating"
                                          : install ? "control-center.power-menu.update-restart"
                                                    : "control-center.power-menu.restart-update"));
    }
  }
  const bool firmware = m_alt && guard != nullptr && guard->firmwareSetup();
  if (m_restartLabel != nullptr) {
    m_restartLabel->setText(i18n::tr(firmware  ? "control-center.power-menu.restart-firmware"
                                     : blocked ? "control-center.power-menu.restart-when-done"
                                               : "control-center.power-menu.restart"));
  }
  if (m_shutdownLabel != nullptr) {
    m_shutdownLabel->setText(i18n::tr(blocked ? "control-center.power-menu.shut-down-when-done"
                                              : "control-center.power-menu.shut-down"));
  }
}

bool PowerMenuPage::handleKey(std::uint32_t sym, std::uint32_t /*modifiers*/, bool pressed, bool /*preedit*/) {
  if (sym != XKB_KEY_Alt_L && sym != XKB_KEY_Alt_R) {
    return false;
  }
  m_alt = pressed;
  syncState();
  PanelManager::instance().refresh();
  return false;
}

std::unique_ptr<Flex> PowerMenuPage::makeTile(std::string_view glyph, const std::string& label, std::string action,
                                           float height, bool roundIcon, Label** labelOut) {
  const float s = contentScale();
  const float opacity = panelCardOpacity();
  auto button = std::make_unique<Button>();
  Button::ButtonPalette colors;
  colors.borderWidth = 0.5F * s;
  colors.normal = {.bg = colorSpecFromRole(ColorRole::GlassCard, opacity),
                    .border = colorSpecFromRole(ColorRole::GlassCardEdge),
                    .label = colorSpecFromRole(ColorRole::Label1)};
  colors.hover = colors.normal;
  colors.hover.bg = colorSpecFromRole(ColorRole::Fill2, opacity);
  colors.pressed = colors.normal;
  colors.pressed.bg = colorSpecFromRole(ColorRole::Fill3, opacity);
  colors.disabled = colors.normal;
  button->setCustomPalette(colors);
  button->setRadius(Tokens::componentTileRadius * s);
  button->setControlHeight(height * s);
  button->setPadding(0.0F, Tokens::componentTilePadding * s);
  button->setGap(Tokens::spaceP12 * s);
  button->setContentAlign(roundIcon ? ButtonContentAlign::Start : ButtonContentAlign::Center);
  button->setDirection(FlexDirection::Horizontal);
  button->setAlign(FlexAlign::Center);
  const float iconSize = roundIcon ? Tokens::componentRoundIconSize * s : Tokens::componentRoundIconGlyph * s;
  auto iconBox = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center,
                          .fill = roundIcon ? std::optional(colorSpecFromRole(ColorRole::Fill2)) : std::nullopt,
                          .radius = iconSize * 0.5F, .width = iconSize, .height = iconSize},
                         ui::glyph({.glyph = std::string(glyph), .glyphSize = Tokens::componentRoundIconGlyph * s,
                                    .color = colorSpecFromRole(ColorRole::Label1)}));
  iconBox->setHitTestVisible(false);
  button->addChild(std::move(iconBox));
  auto text = ui::label({.out = labelOut, .text = label, .fontSize = Tokens::typeHeadline.size * s,
                         .fontWeight = static_cast<FontWeight>(Style::glassWeight(Tokens::typeHeadline.weight)),
                         .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1});
  text->setHitTestVisible(false);
  button->addChild(std::move(text));
  button->setOnClick([this, action = std::move(action)] { run(action); });
  auto card = ui::row({.align = FlexAlign::Stretch});
  button->setFlexGrow(1.0F);
  card->addChild(std::move(button));
  return card;
}

void PowerMenuPage::run(const std::string& action) {
  SessionGuard* guard = m_services.sessionGuard;
  const bool firmware = action == "reboot" && m_alt && guard != nullptr && guard->firmwareSetup();
  PanelManager::instance().closePanel();
  if (m_services.ipc == nullptr) {
    return;
  }
  if (firmware) {
    (void)m_services.ipc->execute("session reboot_uefi");
    return;
  }
  // A block inhibitor holds the machine: Restart and Shut Down wait for it ("When done", island live activity).
  if ((action == "reboot" || action == "shutdown") && guard != nullptr && !guard->blockers().empty()) {
    guard->queue(action);
    return;
  }
  // Restart, Shut Down and Log Out first ask every window to close (D3 session end).
  const bool ends = action == "reboot" || action == "shutdown" || action == "logout";
  (void)m_services.ipc->execute((ends ? "power-overlay end " : "session ") + action);
}

void PowerMenuPage::syncWho(Renderer& renderer) {
  if (m_subtitle != nullptr) {
    std::string line;
    if (const auto uptime = systemUptime(); uptime.has_value()) {
      line = i18n::tr("control-center.power-menu.up", "uptime", formatDuration(*uptime));
    }
    const UPowerState* power = m_services.upower != nullptr ? &m_services.upower->state() : nullptr;
    if (power != nullptr && power->isPresent) {
      const std::string level = std::format("{:.0F} %", power->percentage);
      const std::string battery = power->state == BatteryState::Charging
          ? i18n::tr("control-center.power-menu.charging", "level", level)
          : level;
      line = line.empty() ? battery : line + " · " + battery;
    }
    m_subtitle->setText(line);
  }
  if (m_avatar != nullptr && m_services.config != nullptr && m_services.asyncTextures != nullptr) {
    const std::string path = shell::avatarDisplayPath(m_services.accounts, m_services.config->config());
    if (path != m_avatarPath && !path.empty()) {
      m_avatarPath = path;
      (void)m_avatar->setSourceFileAsync(renderer, *m_services.asyncTextures, path,
                                         static_cast<int>(std::round(m_avatar->width())), false);
    }
  }
}

void PowerMenuPage::onClose() {
  if (m_watching && m_services.sessionGuard != nullptr) {
    m_services.sessionGuard->watch(false);
  }
  m_watching = false;
  m_alt = false;
  m_updateRow = nullptr;
  m_updateLabel = nullptr;
  m_blockerRow = nullptr;
  m_blockerTitle = nullptr;
  m_blockerBody = nullptr;
  m_restartLabel = nullptr;
  m_shutdownLabel = nullptr;
  m_rootLayout = nullptr;
  m_avatar = nullptr;
  m_subtitle = nullptr;
  m_avatarPath.clear();
}

void PowerMenuPage::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  syncWho(renderer);
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void PowerMenuPage::doUpdate(Renderer& renderer) {
  syncWho(renderer);
  syncState();
}

float PowerMenuPage::measureHeight(Renderer& renderer, float width) {
  if (m_rootLayout == nullptr) {
    return 0.0F;
  }
  syncWho(renderer);
  syncState();
  m_rootLayout->setSize(width, 0.0F);
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  return m_rootLayout->measure(renderer, constraints).height;
}
