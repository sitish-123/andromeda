#include "shell/setup_wizard/setup_wizard_panel.h"

#include "shell/desktop_layout/desktop_layout.h"

#include "auth/biometric_enrollment.h"
#include "compositors/settings_control.h"
#include "compositors/shell_wayland_connection.h"
#include "config/config_service.h"
#include "core/files/directory_scanner.h"
#include "core/files/resource_paths.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/link/link_service.h"
#include "dbus/andromeda/software_update_service.h"
#include "dbus/system_bus.h"
#include "dbus/system_interactive_call.h"
#include "dbus/timedate/timedate_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/countdown_ring_node.h"
#include "shell/panel/panel_manager.h"
#include "system/default_apps.h"
#include "system/display_manager.h"
#include "system/desktop_entry.h"
#include "system/xkb_layout_catalog.h"
#include "theme/builtin_palettes.h"
#include "ui/builders.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <glib.h>
#include <iterator>
#include <memory>
#include <pwd.h>
#include <qrencode.h>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <span>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr Logger kLog("setup-wizard");

  enum PageId : int { kWelcome, kAppearance, kLayout, kKeyboard, kTimezone, kPhone, kUnlock, kLogin, kApps, kSearch, kUpdate, kDone };
  constexpr const char* kPageNames[] = {"welcome", "appearance", "layout", "keyboard", "timezone", "phone", "unlock",
                                        "login", "apps", "search", "update", "done"};

  // Thumbnail cell: 3 equal columns in the 640 sheet (592 content - 2 x 4 card - 2 x 8 thumbs - 2 x 8 gaps) / 3.
  constexpr float kThumbWidth = 184.0F;
  constexpr float kThumbHeight = 80.0F;
  constexpr float kSwatch = 20.0F;
  constexpr float kRingOutset = 4.0F;
  constexpr float kSlotWidth = 208.0F;
  constexpr float kQrSize = 160.0F;
  constexpr float kQrPadding = 12.0F;
  constexpr float kQrCode = kQrSize - 2.0F * kQrPadding;
  // Module colour of the mockup QR on its white box.
  constexpr std::uint8_t kQrInk[3] = {0x14, 0x14, 0x18};
  // Text column beside the QR box: 592 content - 8 card - 32 padding - 160 QR - 24 gap.
  constexpr float kQrTextWidth = 368.0F;
  constexpr float kEnrolCircle = 128.0F;
  constexpr float kLogoSize = 96.0F;
  constexpr float kKeycapSize = 20.0F;
  constexpr float kHeroTextWidth = 400.0F;
  constexpr float kPageDotSize = 6.0F;

  // Installed with the andromeda-link package; without it the Phone page has nothing to offer.
  constexpr auto kLinkUnit = "/usr/lib/systemd/user/andromeda-linkd.service";

  constexpr auto kFaceName = "Face";

  constexpr auto kLayoutKey = "input.keyboard.layout";
  constexpr float kChoiceWidth = 260.0F;
  // The default apps a first login asks about; Settings › Default Apps has the rest.
  constexpr default_apps::Category kWizardApps[] = {
      default_apps::Category::WebBrowser, default_apps::Category::FileManager, default_apps::Category::Mail};

  std::string_view appLabelKey(default_apps::Category category) {
    switch (category) {
    case default_apps::Category::WebBrowser:
      return "settings.default-apps.web-browser";
    case default_apps::Category::FileManager:
      return "settings.default-apps.file-manager";
    default:
      return "settings.default-apps.mail";
    }
  }


  constexpr auto kPairingPoll = std::chrono::seconds(1);

  struct SelectOption {
    std::string_view labelKey;
    std::string_view value;
  };

  constexpr SelectOption kWallpaperSchemes[] = {
      {"theme.scheme.m3-content", "m3-content"},
      {"theme.scheme.m3-tonal-spot", "m3-tonal-spot"},
      {"theme.scheme.m3-fruit-salad", "m3-fruit-salad"},
      {"theme.scheme.m3-rainbow", "m3-rainbow"},
      {"theme.scheme.m3-monochrome", "m3-monochrome"},
      {"theme.scheme.vibrant", "vibrant"},
      {"theme.scheme.faithful", "faithful"},
      {"theme.scheme.soft", "soft"},
      {"theme.scheme.dysfunctional", "dysfunctional"},
      {"theme.scheme.muted", "muted"},
  };

  constexpr const char* kModeValues[] = {"light", "dark", "auto"};

  std::unique_ptr<Label> makeLabel(
      std::string_view text, const Tokens::Type& type, float scale, ColorRole role,
      FontWeight weight = FontWeight::Normal
  ) {
    return ui::label({
        .text = std::string(text),
        .fontSize = type.size * scale,
        .fontWeight = weight,
        .color = colorSpecFromRole(role),
    });
  }

  std::unique_ptr<Flex> makeCard(float scale) {
    return ui::column({
        .align = FlexAlign::Stretch,
        .padding = Tokens::spaceP4 * scale,
        .fill = colorSpecFromRole(ColorRole::Fill1),
        .radius = Style::scaledRadius(Tokens::radiusR12, scale),
        .border = colorSpecFromRole(ColorRole::Stroke),
        .borderWidth = 0.5F * scale,
    });
  }

  std::unique_ptr<Flex> makeListRow(float scale, float height = Tokens::sizeRow) {
    return ui::row({
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP8 * scale,
        .paddingH = Tokens::spaceP12 * scale,
        .height = height * scale,
    });
  }

  void addSeparator(Flex* card, float inset, float scale) {
    auto line = ui::column({.align = FlexAlign::Stretch});
    line->setPadding(0.0F, 0.0F, 0.0F, inset * scale);
    line->addChild(ui::separator({.color = colorSpecFromRole(ColorRole::Separator), .thickness = 1.0F * scale, .spacing = 0.0F}));
    card->addChild(std::move(line));
  }

  std::unique_ptr<Label> makeGrowLabel(std::string_view text, float scale) {
    auto label = makeLabel(text, Tokens::typeBody, scale, ColorRole::Label1, FontWeight::Medium);
    label->setFlexGrow(1.0F);
    label->setMaxLines(1);
    return label;
  }

  std::unique_ptr<Button> makeRowButton(std::string_view text, ButtonVariant variant, float scale, std::function<void()> onClick, Button** out = nullptr) {
    return ui::button({
        .out = out,
        .text = std::string(text),
        .fontSize = Style::fontSizeBody * scale,
        .variant = variant,
        .minWidth = 64.0F * scale,
        .minHeight = Style::controlHeight * scale,
        .paddingH = Tokens::spaceP12 * scale,
        .radius = Style::scaledRadius(Tokens::radiusR8, scale),
        .onClick = std::move(onClick),
    });
  }

  std::unique_ptr<Flex> makeKeycap(std::string_view text, float scale) {
    auto cap = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .paddingH = 6.0F * scale,
        .radius = Style::scaledRadius(Tokens::radiusR4, scale),
        .border = colorSpecFromRole(ColorRole::Separator),
        .borderWidth = 1.0F * scale,
        .minWidth = kKeycapSize * scale,
        .height = kKeycapSize * scale,
    });
    cap->addChild(makeLabel(text, Tokens::typeCaption, scale, ColorRole::Label2, FontWeight::Medium));
    return cap;
  }

  std::unique_ptr<Flex> makeChip(std::string_view text, float scale) {
    auto chip = ui::row({
        .align = FlexAlign::Center,
        .paddingH = Tokens::spaceP8 * scale,
        .fill = colorSpecFromRole(ColorRole::Fill2),
        .radius = Style::scaledRadius(Tokens::radiusR12, scale),
        .height = Tokens::sizeControlS * scale,
    });
    chip->addChild(makeLabel(text, Tokens::typeFootnote, scale, ColorRole::Label2, FontWeight::Medium));
    return chip;
  }

  std::size_t selectedOptionIndex(std::span<const SelectOption> options, std::string_view value) {
    for (std::size_t i = 0; i < options.size(); ++i) {
      if (options[i].value == value) {
        return i;
      }
    }
    return 0;
  }

  void setRing(Flex* ring, bool on, ColorRole role, float scale) {
    if (ring == nullptr) {
      return;
    }
    if (on) {
      ring->setBorder(colorSpecFromRole(role), 2.0F * scale);
    } else {
      ring->clearBorder();
    }
  }

  void show(Node* node, bool on) {
    if (node != nullptr) {
      node->setVisible(on);
      node->setParticipatesInLayout(on);
    }
  }

  // callMethodAsync throws, not replies, when the connection itself is gone.

  std::string currentUser() {
    if (const passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_name != nullptr) {
      return pw->pw_name;
    }
    const char* user = std::getenv("USER");
    return user != nullptr ? user : "";
  }

  std::string spacedCode(const std::string& code) {
    return code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
  }


  bool indexerReachable() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] == '\0') {
      return false;
    }
    const std::string path = std::string(runtime) + "/andromeda-index.sock";
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return false;
    }
    const bool ok = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
    close(fd);
    return ok;
  }

} // namespace

SetupWizardPanel::SetupWizardPanel(
    ConfigService* config, ShellWaylandConnection* wayland, LinkService* link, SystemBus* systemBus,
    std::function<SoftwareUpdateService*()> updates
)
    : m_config(config), m_link(link), m_systemBus(systemBus), m_updates(std::move(updates)), m_user(currentUser()),
      m_wayland(wayland) {}

SetupWizardPanel::~SetupWizardPanel() { stopEnrol(); }

bool SetupWizardPanel::isFirstRun(const ConfigService& config) { return config.shouldRunSetupWizard(); }

void SetupWizardPanel::addOverlay(Node* node, Node* host, float outset) {
  node->setParticipatesInLayout(false);
  m_overlays.push_back({.node = node, .host = host, .outset = outset});
}

void SetupWizardPanel::create() {
  const float scale = contentScale();
  m_overlays.clear();
  m_modeArts.clear();
  m_swatchRings.clear();
  m_swatchNames.clear();
  m_pickedLoaded = false;
  m_preview = false;
  // Update1 lists pacman or apt updates; the client exists whenever there is a session bus.
  m_updateChecker = static_cast<bool>(m_updates);
  probeLogin();
  probeKeyboard();
  if (m_systemBus != nullptr && m_timedate == nullptr) {
    m_timedate = std::make_unique<TimeDateService>(*m_systemBus, [this] { syncTimezone(); });
  }
  std::error_code linkEc;
  m_linkInstalled = (m_link != nullptr && m_link->available()) || std::filesystem::exists(kLinkUnit, linkEc);

  auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch});

  auto pages = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  pages->addChild(buildWelcome(scale));
  pages->addChild(buildAppearance(scale));
  pages->addChild(buildLayout(scale));
  pages->addChild(buildKeyboard(scale));
  pages->addChild(buildTimezone(scale));
  pages->addChild(buildPhone(scale));
  pages->addChild(buildUnlock(scale));
  pages->addChild(buildLogin(scale));
  pages->addChild(buildApps(scale));
  pages->addChild(buildSearch(scale));
  pages->addChild(buildUpdate(scale));
  pages->addChild(buildDone(scale));
  root->addChild(std::move(pages));

  auto footer = ui::row({
      .align = FlexAlign::Center,
      .padding = Tokens::spaceP24 * scale,
  });
  auto lead = ui::row({.align = FlexAlign::Center, .width = kSlotWidth * scale});
  lead->addChild(makeRowButton(i18n::tr("setup-wizard.skip"), ButtonVariant::Ghost, scale, [this] {
    if (m_step == kWelcome) {
      commit();
    } else {
      advance(-1);
    }
  }, &m_leadButton));
  footer->addChild(std::move(lead));

  auto dots = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = kPageDotSize * scale,
      .height = Style::spaceLg * scale,
      .flexGrow = 1.0F,
  });
  for (int i = 0; i < kPageCount; ++i) {
    dots->addChild(ui::row({
        .out = &m_dots[static_cast<std::size_t>(i)],
        .radius = kPageDotSize * 0.5F * scale,
        .width = kPageDotSize * scale,
        .height = kPageDotSize * scale,
    }));
  }
  footer->addChild(std::move(dots));

  auto trail = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::End,
      .gap = Tokens::spaceP8 * scale,
      .width = kSlotWidth * scale,
  });
  trail->addChild(makeRowButton(i18n::tr("setup-wizard.set-up-later"), ButtonVariant::Ghost, scale, [this] { advance(1); }, &m_laterButton));
  trail->addChild(ui::button({
      .out = &m_primaryButton,
      .text = i18n::tr("setup-wizard.get-started"),
      .fontSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Primary,
      .minWidth = 96.0F * scale,
      .minHeight = Style::controlHeight * scale,
      .paddingH = Tokens::spaceP12 * scale,
      .radius = Style::controlHeight * scale * 0.5F,
      .onClick = [this] { advance(1); },
  }));
  footer->addChild(std::move(trail));
  root->addChild(std::move(footer));

  setRoot(std::move(root));
  probeUnlock();
  probeSearch();
  m_step = kWelcome;
  m_qrUri.clear();
  kLog.info("page {}", kPageNames[m_step]);
  syncPages();
  syncSelection();
}

std::unique_ptr<Flex> SetupWizardPanel::makePage(int page, float scale, std::string_view titleKey, std::string_view bodyKey) {
  auto column = ui::column({
      .out = &m_pages[static_cast<std::size_t>(page)],
      .align = FlexAlign::Stretch,
      .flexGrow = 1.0F,
      .configure = [scale](Flex& f) { f.setPadding(Tokens::spaceP24 * scale, Tokens::spaceP24 * scale, 0.0F, Tokens::spaceP24 * scale); },
  });
  auto header = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * scale});
  header->addChild(makeLabel(i18n::tr(titleKey), Tokens::typeTitle1, scale, ColorRole::Label1, FontWeight::SemiBold));
  auto body = makeLabel(i18n::tr(bodyKey), Tokens::typeBody, scale, ColorRole::Label2, FontWeight::Medium);
  body->setTextAlign(TextAlign::Center);
  body->setMaxWidth(560.0F * scale);
  body->setMaxLines(2);
  header->addChild(std::move(body));
  column->addChild(std::move(header));
  column->addChild(ui::node({.height = Tokens::spaceP24 * scale}));
  return column;
}

std::unique_ptr<Flex> SetupWizardPanel::makeAlertRow(float scale, Label** textOut, std::function<void()> onRetry) {
  auto row = makeListRow(scale);
  row->addChild(ui::glyph({.glyph = "alert-circle", .glyphSize = Tokens::sizeSymbol * scale, .color = colorSpecFromRole(ColorRole::DangerText)}));
  auto text = makeGrowLabel("", scale);
  *textOut = text.get();
  row->addChild(std::move(text));
  if (onRetry) {
    row->addChild(makeRowButton(i18n::tr("setup-wizard.try-again"), ButtonVariant::Ghost, scale, std::move(onRetry)));
  }
  return row;
}

std::unique_ptr<Flex> SetupWizardPanel::buildWelcome(float scale) {
  auto page = ui::column({
      .out = &m_pages[kWelcome],
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .paddingH = Tokens::spaceP24 * scale,
      .flexGrow = 1.0F,
  });
  page->addChild(ui::image({.out = &m_logo, .width = kLogoSize * scale, .height = kLogoSize * scale}));
  auto title = makeLabel(i18n::tr("setup-wizard.title"), Tokens::typeLarge, scale, ColorRole::Label1, FontWeight::SemiBold);
  title->setTextAlign(TextAlign::Center);
  title->setMaxWidth(480.0F * scale);
  title->setMaxLines(2);
  page->addChild(ui::node({.height = Tokens::spaceP24 * scale}));
  page->addChild(std::move(title));
  page->addChild(ui::node({.height = Tokens::spaceP8 * scale}));
  auto body = makeLabel(i18n::tr("setup-wizard.welcome-body"), Tokens::typeBody, scale, ColorRole::Label2, FontWeight::Medium);
  body->setTextAlign(TextAlign::Center);
  body->setMaxWidth(400.0F * scale);
  body->setMaxLines(2);
  page->addChild(std::move(body));
  return page;
}

std::unique_ptr<Flex> SetupWizardPanel::makeThumb(
    float scale, std::string_view label, std::function<void()> onClick, Flex** frameOut, Flex** ringOut,
    Label** labelOut
) {
  auto cell = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Tokens::spaceP8 * scale,
      .width = kThumbWidth * scale,
  });
  auto frame = ui::column({
      .out = frameOut,
      .align = FlexAlign::Stretch,
      .fill = colorSpecFromRole(ColorRole::Fill1),
      .radius = Style::scaledRadius(Tokens::radiusR8, scale),
      .clipChildren = true,
      .height = kThumbHeight * scale,
  });
  Flex* framePtr = frame.get();
  cell->addChild(std::move(frame));

  auto ring = ui::row({.out = ringOut, .radius = Style::scaledRadius(Tokens::radiusR8, scale) + kRingOutset * scale});
  addOverlay(cell->addChild(std::move(ring)), framePtr, kRingOutset * scale);

  auto text = makeLabel(label, Tokens::typeFootnote, scale, ColorRole::Label2, FontWeight::Medium);
  text->setTextAlign(TextAlign::Center);
  text->setMaxWidth(kThumbWidth * scale);
  text->setMaxLines(1);
  if (labelOut != nullptr) {
    *labelOut = text.get();
  }
  cell->addChild(std::move(text));

  auto area = ui::inputArea({});
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  area->setOnClick([onClick = std::move(onClick)](const InputArea::PointerData&) { onClick(); });
  Flex* cellPtr = cell.get();
  addOverlay(cell->addChild(std::move(area)), cellPtr, 0.0F);
  return cell;
}

std::unique_ptr<Flex> SetupWizardPanel::buildAppearance(float scale) {
  const auto& cfg = m_config->config();
  auto page = makePage(kAppearance, scale, "setup-wizard.appearance-title", "setup-wizard.appearance-body");

  m_mode = cfg.theme.mode == ThemeMode::Dark ? 1 : (cfg.theme.mode == ThemeMode::Auto ? 2 : 0);
  {
    auto card = makeCard(scale);
    auto thumbs = ui::row({.align = FlexAlign::Start, .gap = Tokens::spaceP8 * scale, .padding = Tokens::spaceP8 * scale});
    const std::string labels[] = {
        i18n::tr("settings.options.theme.mode.light"), i18n::tr("settings.options.theme.mode.dark"),
        i18n::tr("common.states.auto")
    };
    for (int i = 0; i < 3; ++i) {
      Flex* frame = nullptr;
      auto cell = makeThumb(
          scale, labels[i],
          [this, i] {
            m_mode = i;
            m_config->setOverride({"theme", "mode"}, std::string(kModeValues[i]));
            syncSelection();
          },
          &frame, &m_modeRings[static_cast<std::size_t>(i)], &m_modeLabels[static_cast<std::size_t>(i)]
      );
      ModeArt art{.box = frame, .mode = i};
      art.wallpaper = static_cast<Image*>(frame->addChild(ui::image({.fit = ImageFit::Cover})));
      art.wallpaper->setParticipatesInLayout(false);
      const bool light = i != 1;
      const auto pill = light ? Color{1.0F, 1.0F, 1.0F, 0.72F} : Color{0.11F, 0.11F, 0.125F, 0.72F};
      const auto win = light ? Color{0.949F, 0.949F, 0.965F, 1.0F} : Color{0.118F, 0.118F, 0.133F, 1.0F};
      const auto bar = light ? Color{0.0F, 0.0F, 0.0F, 0.12F} : Color{1.0F, 1.0F, 1.0F, 0.16F};
      const auto darkPill = Color{0.11F, 0.11F, 0.125F, 0.72F};
      const auto darkWin = Color{0.118F, 0.118F, 0.133F, 1.0F};
      const auto darkBar = Color{1.0F, 1.0F, 1.0F, 0.16F};
      const Color colors[] = {pill, i == 2 ? darkPill : pill, win, bar};
      for (const auto& color : colors) {
        auto part = ui::row({});
        part->setFill(color);
        art.parts.push_back(static_cast<Flex*>(frame->addChild(std::move(part))));
      }
      if (i == 2) {
        for (const auto& color : {darkWin, darkBar}) {
          auto part = ui::row({});
          part->setFill(color);
          art.parts.push_back(static_cast<Flex*>(frame->addChild(std::move(part))));
        }
      }
      for (Flex* part : art.parts) {
        part->setParticipatesInLayout(false);
      }
      m_modeArts.push_back(std::move(art));
      thumbs->addChild(std::move(cell));
    }
    card->addChild(std::move(thumbs));
    page->addChild(std::move(card));
  }
  page->addChild(ui::node({.height = Tokens::spaceP16 * scale}));

  m_paletteSource = cfg.theme.source == PaletteSource::Wallpaper ? PaletteSource::Wallpaper : PaletteSource::Builtin;
  m_builtinPalette = cfg.theme.builtinPalette;
  {
    auto card = makeCard(scale);
    auto colour = makeListRow(scale);
    m_colourRow = colour.get();
    colour->addChild(makeGrowLabel(i18n::tr("setup-wizard.colour"), scale));
    auto swatches = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12 * scale});
    const bool light = isLightPalette();
    for (const auto& entry : noctalia::theme::builtinPalettes()) {
      const std::string name(entry.name);
      auto swatch = ui::row({
          .fill = fixedColorSpec(light ? entry.light.palette.primary : entry.dark.palette.primary),
          .radius = kSwatch * 0.5F * scale,
          .width = kSwatch * scale,
          .height = kSwatch * scale,
      });
      Flex* swatchPtr = swatch.get();
      auto ring = ui::row({.radius = (kSwatch * 0.5F + kRingOutset) * scale});
      m_swatchRings.push_back(ring.get());
      m_swatchNames.push_back(name);
      addOverlay(swatch->addChild(std::move(ring)), swatchPtr, kRingOutset * scale);
      auto area = ui::inputArea({});
      area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      area->setTooltip(name);
      area->setOnClick([this, name](const InputArea::PointerData&) {
        m_builtinPalette = name;
        m_config->setOverride({"theme", "source"}, std::string("builtin"));
        m_config->setOverride({"theme", "builtin"}, name);
        syncSelection();
      });
      addOverlay(swatch->addChild(std::move(area)), swatchPtr, 0.0F);
      swatches->addChild(std::move(swatch));
    }
    colour->addChild(std::move(swatches));
    card->addChild(std::move(colour));

    auto match = makeListRow(scale);
    match->addChild(makeGrowLabel(i18n::tr("setup-wizard.match-wallpaper"), scale));
    std::vector<std::string> schemes;
    for (const auto& option : kWallpaperSchemes) {
      schemes.emplace_back(i18n::tr(option.labelKey));
    }
    match->addChild(ui::select({
        .out = &m_schemeSelect,
        .options = std::move(schemes),
        .selectedIndex = selectedOptionIndex(kWallpaperSchemes, cfg.theme.wallpaperScheme),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeightSm * scale,
        .horizontalPadding = Tokens::spaceP8 * scale,
        .onSelectionChanged =
            [this](std::size_t index, std::string_view /*label*/) {
              if (index < std::size(kWallpaperSchemes)) {
                m_config->setOverride({"theme", "wallpaper_scheme"}, std::string(kWallpaperSchemes[index].value));
              }
            },
    }));
    match->addChild(ui::toggle({
        .out = &m_matchToggle,
        .checked = m_paletteSource == PaletteSource::Wallpaper,
        .scale = scale,
        .onChange =
            [this](bool on) {
              m_paletteSource = on ? PaletteSource::Wallpaper : PaletteSource::Builtin;
              m_config->setOverride({"theme", "source"}, std::string(on ? "wallpaper" : "builtin"));
              if (!on && !m_builtinPalette.empty()) {
                m_config->setOverride({"theme", "builtin"}, m_builtinPalette);
              }
              syncSelection();
            },
    }));
    m_matchSeparator = match->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Separator)}));
    m_matchSeparator->setParticipatesInLayout(false);
    card->addChild(std::move(match));
    page->addChild(std::move(card));
  }
  page->addChild(ui::node({.height = Tokens::spaceP16 * scale}));

  {
    const std::string bundled = paths::assetPath("andromeda-wallpaper.png").string();
    const std::string current = m_config->getDefaultWallpaperPath();
    if (!current.empty() && current != bundled) {
      m_pickedPath = current;
      m_wallpaper = 1;
    }
    auto card = makeCard(scale);
    auto thumbs = ui::row({.align = FlexAlign::Start, .gap = Tokens::spaceP8 * scale, .padding = Tokens::spaceP8 * scale});
    Flex* frame = nullptr;
    auto andromeda = makeThumb(
        scale, i18n::tr("setup-wizard.andromeda-wallpaper"),
        [this, bundled] {
          m_wallpaper = 0;
          m_config->setWallpaperPath(std::nullopt, bundled);
          syncSelection();
        },
        &frame, &m_wallpaperRings[0], &m_wallpaperLabels[0]
    );
    m_bundledImage = static_cast<Image*>(frame->addChild(ui::image({.fit = ImageFit::Cover, .flexGrow = 1.0F})));
    thumbs->addChild(std::move(andromeda));

    auto choose = makeThumb(
        scale, m_pickedPath.empty() ? i18n::tr("setup-wizard.choose") : std::filesystem::path(m_pickedPath).filename().string(),
        [this] { chooseWallpaper(); }, &frame, &m_wallpaperRings[1], &m_wallpaperLabels[1]
    );
    frame->setAlign(FlexAlign::Center);
    frame->setJustify(FlexJustify::Center);
    m_addGlyph = frame->addChild(ui::glyph({
        .glyph = "plus",
        .glyphSize = 20.0F * scale,
        .color = colorSpecFromRole(ColorRole::Label2),
    }));
    m_pickedImage = static_cast<Image*>(frame->addChild(ui::image({.fit = ImageFit::Cover})));
    m_pickedImage->setParticipatesInLayout(false);
    thumbs->addChild(std::move(choose));
    card->addChild(std::move(thumbs));
    page->addChild(std::move(card));
  }
  return page;
}

std::unique_ptr<Flex> SetupWizardPanel::buildPhone(float scale) {
  auto page = makePage(kPhone, scale, "setup-wizard.phone-title", "setup-wizard.phone-body");

  auto card = makeCard(scale);
  m_phoneCard = card.get();
  auto wrap = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP24 * scale, .padding = Tokens::spaceP16 * scale});
  auto qr = ui::column({
      .out = &m_qrBox,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .fill = colorSpecFromRole(ColorRole::Fill1),
      .radius = Style::scaledRadius(Tokens::radiusR12, scale),
      .width = kQrSize * scale,
      .height = kQrSize * scale,
  });
  m_qrGlyph = qr->addChild(ui::glyph({.glyph = "qrcode", .glyphSize = 48.0F * scale, .color = colorSpecFromRole(ColorRole::Label3)}));
  m_qrImage = static_cast<Image*>(qr->addChild(ui::image({
      .fit = ImageFit::Contain,
      .width = kQrCode * scale,
      .height = kQrCode * scale,
  })));
  m_pairedGlyph = qr->addChild(ui::glyph({.glyph = "circle-check", .glyphSize = 48.0F * scale, .color = colorSpecFromRole(ColorRole::Success)}));
  wrap->addChild(std::move(qr));

  auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
  auto headline = makeLabel("", Tokens::typeHeadline, scale, ColorRole::Label1, FontWeight::SemiBold);
  headline->setMaxWidth(kQrTextWidth * scale);
  headline->setMaxLines(1);
  m_phoneHeadline = headline.get();
  text->addChild(std::move(headline));
  text->addChild(ui::node({.height = Tokens::spaceP4 * scale}));
  auto body = makeLabel("", Tokens::typeParagraph, scale, ColorRole::Label2, FontWeight::Medium);
  body->setMaxWidth(kQrTextWidth * scale);
  body->setMaxLines(3);
  m_phoneBody = body.get();
  text->addChild(std::move(body));
  auto code = ui::label({
      .text = "",
      .fontSize = Tokens::typeTitle1.size * scale,
      .fontWeight = FontWeight::SemiBold,
      .fontFeatures = "tnum",
      .color = colorSpecFromRole(ColorRole::Label1),
  });
  m_phoneCodeLabel = code.get();
  auto codeBox = ui::column({.align = FlexAlign::Start});
  codeBox->setPadding(Tokens::spaceP12 * scale, 0.0F, 0.0F, 0.0F);
  codeBox->addChild(std::move(code));
  text->addChild(std::move(codeBox));
  auto status = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale});
  status->setPadding(Tokens::spaceP12 * scale, 0.0F, 0.0F, 0.0F);
  m_phoneStatus = status.get();
  status->addChild(ui::spinner({.color = colorSpecFromRole(ColorRole::Label2), .spinnerSize = Tokens::sizeSymbol * scale, .spinning = true}));
  status->addChild(makeLabel(i18n::tr("setup-wizard.phone-waiting"), Tokens::typeFootnote, scale, ColorRole::Label2, FontWeight::Medium));
  text->addChild(std::move(status));
  wrap->addChild(std::move(text));
  card->addChild(std::move(wrap));
  page->addChild(std::move(card));

  auto alert = makeCard(scale);
  m_phoneAlert = alert.get();
  alert->addChild(makeAlertRow(scale, &m_phoneAlertText, [this] {
    if (m_preview) {
      return;
    }
    if (m_phoneState == PhoneState::Failed && m_link != nullptr) {
      startPairing();
    }
    syncPhone();
  }));
  page->addChild(std::move(alert));
  return page;
}

std::unique_ptr<Flex> SetupWizardPanel::makeMethodRow(float scale, Method method) {
  UnlockMethod& m = method == Method::Face ? m_face : m_finger;
  auto row = makeListRow(scale, Tokens::sizeRowTwo);
  row->setGap(Tokens::spaceP12 * scale);
  m.row = row.get();
  row->addChild(ui::glyph({
      .glyph = method == Method::Face ? "face-id" : "fingerprint",
      .glyphSize = Tokens::sizeSymbolL * scale,
      .color = colorSpecFromRole(ColorRole::Label2),
  }));
  auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
  text->addChild(makeLabel(i18n::tr(method == Method::Face ? "setup-wizard.face" : "setup-wizard.fingerprint"), Tokens::typeBody, scale, ColorRole::Label1, FontWeight::Medium));
  auto sub = makeLabel("", Tokens::typeFootnote, scale, ColorRole::Label2, FontWeight::Medium);
  sub->setMaxLines(1);
  m.sub = sub.get();
  text->addChild(std::move(sub));
  row->addChild(std::move(text));
  row->addChild(makeRowButton(i18n::tr("setup-wizard.set-up"), ButtonVariant::Default, scale, [this, method] { startEnrol(method); }, &m.setUp));
  auto on = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale});
  m.on = on.get();
  on->addChild(ui::glyph({.glyph = "circle-check", .glyphSize = Tokens::sizeSymbol * scale, .color = colorSpecFromRole(ColorRole::Success)}));
  on->addChild(makeLabel(i18n::tr("setup-wizard.on"), Tokens::typeBody, scale, ColorRole::Success, FontWeight::Medium));
  row->addChild(std::move(on));
  return row;
}

std::unique_ptr<Flex> SetupWizardPanel::buildUnlock(float scale) {
  auto page = makePage(kUnlock, scale, "setup-wizard.unlock-title", "setup-wizard.unlock-body");

  auto card = makeCard(scale);
  m_methodCard = card.get();
  card->addChild(makeMethodRow(scale, Method::Face));
  addSeparator(card.get(), 44.0F, scale);
  m_unlockSeparator = card->children().back().get();
  card->addChild(makeMethodRow(scale, Method::Finger));
  page->addChild(std::move(card));

  auto enrol = makeCard(scale);
  m_enrolCard = enrol.get();
  auto block = ui::column({.align = FlexAlign::Center, .padding = Tokens::spaceP24 * scale});
  auto circle = ui::column({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .fill = colorSpecFromRole(ColorRole::Fill1),
      .radius = kEnrolCircle * 0.5F * scale,
      .width = kEnrolCircle * scale,
      .height = kEnrolCircle * scale,
  });
  auto ring = std::make_unique<CountdownRingNode>();
  ring->setThickness(Style::spaceXs * scale);
  ring->setParticipatesInLayout(false);
  m_enrolRing = static_cast<CountdownRingNode*>(circle->addChild(std::move(ring)));
  m_enrolFaceGlyph = circle->addChild(ui::glyph({.glyph = "face-id", .glyphSize = 48.0F * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
  m_enrolFingerGlyph = circle->addChild(ui::glyph({.glyph = "fingerprint", .glyphSize = 48.0F * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
  block->addChild(std::move(circle));
  block->addChild(ui::node({.height = Tokens::spaceP16 * scale}));
  auto headline = makeLabel("", Tokens::typeHeadline, scale, ColorRole::Label1, FontWeight::SemiBold);
  m_enrolHeadline = headline.get();
  block->addChild(std::move(headline));
  block->addChild(ui::node({.height = Tokens::spaceP4 * scale}));
  auto prompt = makeLabel("", Tokens::typeParagraph, scale, ColorRole::Label2, FontWeight::Medium);
  prompt->setTextAlign(TextAlign::Center);
  prompt->setMaxWidth(480.0F * scale);
  prompt->setMaxLines(2);
  m_enrolPromptLabel = prompt.get();
  block->addChild(std::move(prompt));
  block->addChild(ui::node({.height = Tokens::spaceP16 * scale}));
  block->addChild(makeRowButton(i18n::tr("setup-wizard.cancel"), ButtonVariant::Default, scale, [this] { stopEnrol(); syncPages(); }));
  enrol->addChild(std::move(block));
  page->addChild(std::move(enrol));
  return page;
}

std::unique_ptr<Flex> SetupWizardPanel::buildLayout(float scale) {
  auto page = makePage(kLayout, scale, "setup-wizard.layout-title", "setup-wizard.layout-body");
  m_layoutPick = desktop_layout::current(m_config->config(), m_compositorSettings.get());
  auto card = makeCard(scale);
  auto grid = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale, .padding = Tokens::spaceP8 * scale});
  std::unique_ptr<Flex> row;
  for (std::size_t i = 0; i < desktop_layout::kPresets.size(); ++i) {
    const std::string preset(desktop_layout::kPresets[i]);
    if (row == nullptr) {
      row = ui::row({.align = FlexAlign::Start, .gap = Tokens::spaceP8 * scale});
    }
    Flex* frame = nullptr;
    auto cell = makeThumb(
        scale, i18n::tr("settings.desktop-layout.options." + preset), [this, preset] { applyLayout(preset); }, &frame,
        &m_layoutRings[i], &m_layoutLabels[i]
    );
    // The 64 x 40 card at 2x, centred in the wider thumb (the same card as Settings › Appearance).
    auto card = desktop_layout::preview(preset, 128.0F * scale, kThumbHeight * scale, scale);
    card->setParticipatesInLayout(false);
    card->setPosition((kThumbWidth - 128.0F) * 0.5F * scale, 0.0F);
    frame->addChild(std::move(card));
    row->addChild(std::move(cell));
    if (i % 2 == 1) {
      grid->addChild(std::move(row));
    }
  }
  card->addChild(std::move(grid));
  page->addChild(std::move(card));
  return page;
}

void SetupWizardPanel::applyLayout(const std::string& preset) {
  m_layoutPick = preset;
  const desktop_layout::Snapshot now = desktop_layout::snapshot(m_config->config(), m_compositorSettings.get());
  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> shell;
  const auto compositorKey = [](const std::vector<std::string>& path) {
    std::string key;
    for (std::size_t i = 1; i < path.size(); ++i) {
      key += (i > 1 ? "." : "") + path[i];
    }
    return key;
  };
  for (auto& [path, value] : desktop_layout::writes(preset, now)) {
    if (!path.empty() && path.front() == "andromeda") {
      if (m_compositorSettings != nullptr) {
        m_compositorSettings->set(compositorKey(path), std::get<std::string>(value));
      }
    } else {
      shell.emplace_back(std::move(path), std::move(value));
    }
  }
  m_config->setOverrides(shell);
  for (const auto& path : desktop_layout::clears(preset, now)) {
    if (!path.empty() && path.front() == "andromeda") {
      if (m_compositorSettings != nullptr) {
        m_compositorSettings->set(compositorKey(path), {});
      }
    } else {
      m_config->clearOverride(path);
    }
  }
  syncSelection();
}

std::unique_ptr<Flex> SetupWizardPanel::buildKeyboard(float scale) {
  auto page = makePage(kKeyboard, scale, "setup-wizard.keyboard-title", "setup-wizard.keyboard-body");
  auto card = makeCard(scale);
  auto row = makeListRow(scale);
  row->addChild(makeGrowLabel(i18n::tr("setup-wizard.keyboard-layout"), scale));
  std::vector<std::string> labels;
  const xkb::Catalog catalog = xkb::loadCatalog();
  m_layoutNames.clear();
  m_layout.clear();
  for (const xkb::Layout& layout : catalog.layouts) {
    if (layout.name == "custom") {
      continue; // "A user-defined custom Layout": an empty file unless the user wrote one
    }
    m_layoutNames.push_back(layout.name);
    labels.push_back(layout.description);
  }
  row->addChild(ui::select({
      .out = &m_layoutSelect,
      .options = std::move(labels),
      .clearSelection = true,
      .placeholder = i18n::tr("setup-wizard.keyboard-system"),
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeightSm * scale,
      .horizontalPadding = Tokens::spaceP8 * scale,
      .width = kChoiceWidth * scale,
      .onSelectionChanged =
          [this](std::size_t index, std::string_view /*label*/) {
            if (index >= m_layoutNames.size() || m_compositorSettings == nullptr) {
              return;
            }
            // Replace only the default (first) input source; the others and their variants stay.
            m_layout = m_layoutNames[index];
            std::string layouts = m_compositorSettings->value(kLayoutKey);
            std::string variants = m_compositorSettings->value("input.keyboard.variant");
            const auto rest = [](const std::string& list) {
              const auto comma = list.find(',');
              return comma == std::string::npos ? std::string{} : list.substr(comma);
            };
            m_compositorSettings->set(kLayoutKey, m_layout + rest(layouts));
            m_compositorSettings->set("input.keyboard.variant", rest(variants));
          },
  }));
  card->addChild(std::move(row));
  addSeparator(card.get(), 12.0F, scale);
  auto test = makeListRow(scale);
  test->addChild(ui::input({
      .placeholder = i18n::tr("setup-wizard.keyboard-test"),
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeightSm * scale,
      .flexGrow = 1.0F,
  }));
  card->addChild(std::move(test));
  page->addChild(std::move(card));
  return page;
}

std::unique_ptr<Flex> SetupWizardPanel::buildTimezone(float scale) {
  auto page = makePage(kTimezone, scale, "setup-wizard.timezone-title", "setup-wizard.timezone-body");
  auto card = makeCard(scale);
  auto zone = makeListRow(scale);
  zone->addChild(makeGrowLabel(i18n::tr("setup-wizard.timezone-zone"), scale));
  m_zoneCount = 0;
  zone->addChild(ui::select({
      .out = &m_zoneSelect,
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeightSm * scale,
      .horizontalPadding = Tokens::spaceP8 * scale,
      .width = kChoiceWidth * scale,
      .onSelectionChanged =
          [this](std::size_t index, std::string_view /*label*/) {
            if (m_timedate != nullptr && index < m_timedate->timezones().size()) {
              m_timedate->setTimezone(m_timedate->timezones()[index]);
            }
          },
  }));
  card->addChild(std::move(zone));
  addSeparator(card.get(), 12.0F, scale);
  auto ntp = makeListRow(scale);
  ntp->addChild(makeGrowLabel(i18n::tr("setup-wizard.timezone-auto"), scale));
  ntp->addChild(ui::toggle({
      .out = &m_ntpToggle,
      .checked = m_timedate != nullptr && m_timedate->ntp(),
      .scale = scale,
      .onChange =
          [this](bool on) {
            if (m_timedate != nullptr) {
              m_timedate->setNtp(on);
            }
          },
  }));
  card->addChild(std::move(ntp));
  page->addChild(std::move(card));
  auto error = makeLabel("", Tokens::typeFootnote, scale, ColorRole::DangerText, FontWeight::Medium);
  error->setMaxLines(2);
  m_zoneError = error.get();
  page->addChild(ui::node({.height = Tokens::spaceP8 * scale}));
  page->addChild(std::move(error));
  return page;
}

std::unique_ptr<Flex> SetupWizardPanel::buildApps(float scale) {
  auto page = makePage(kApps, scale, "setup-wizard.apps-title", "setup-wizard.apps-body");
  auto card = makeCard(scale);
  m_appRows = 0;
  const auto entries = desktopEntriesSnapshot();
  for (const default_apps::Category category : kWizardApps) {
    const std::vector<const DesktopEntry*> candidates =
        entries != nullptr ? default_apps::candidatesForCategory(*entries, category) : std::vector<const DesktopEntry*>{};
    if (candidates.size() < 2) {
      continue;
    }
    const std::string current = default_apps::effectiveDefault(category);
    std::vector<std::string> labels;
    std::vector<std::string> ids;
    std::optional<std::size_t> selected;
    for (const DesktopEntry* entry : candidates) {
      if (entry->id + ".desktop" == current) {
        selected = ids.size();
      }
      ids.push_back(entry->id + ".desktop");
      labels.push_back(entry->name);
    }
    if (m_appRows > 0) {
      addSeparator(card.get(), 12.0F, scale);
    }
    ++m_appRows;
    auto row = makeListRow(scale);
    row->addChild(makeGrowLabel(i18n::tr(appLabelKey(category)), scale));
    row->addChild(ui::select({
        .options = std::move(labels),
        .selectedIndex = selected,
        .clearSelection = !selected.has_value(),
        .placeholder = i18n::tr("setup-wizard.apps-none"),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeightSm * scale,
        .horizontalPadding = Tokens::spaceP8 * scale,
        .width = kChoiceWidth * scale,
        .onSelectionChanged =
            [category, ids = std::move(ids)](std::size_t index, std::string_view /*label*/) {
              if (index < ids.size()) {
                (void)default_apps::setUserDefault(category, ids[index]);
              }
            },
    }));
    card->addChild(std::move(row));
  }
  page->addChild(std::move(card));
  return page;
}

std::unique_ptr<Flex> SetupWizardPanel::buildLogin(float scale) {
  auto page = makePage(kLogin, scale, "setup-wizard.login-title", "setup-wizard.login-body");
  auto card = makeCard(scale);
  auto row = makeListRow(scale, Tokens::sizeRowTwo);
  row->setGap(Tokens::spaceP12 * scale);
  row->addChild(ui::glyph({.glyph = "lock", .glyphSize = Tokens::sizeSymbolL * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
  auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
  auto title = makeLabel(i18n::tr("setup-wizard.login-row"), Tokens::typeBody, scale, ColorRole::Label1, FontWeight::Medium);
  title->setMaxLines(1);
  text->addChild(std::move(title));
  auto sub = makeLabel("", Tokens::typeFootnote, scale, ColorRole::Label2, FontWeight::Medium);
  sub->setMaxLines(2);
  sub->setMaxWidth(400.0F * scale);
  m_loginSubLabel = sub.get();
  text->addChild(std::move(sub));
  row->addChild(std::move(text));
  row->addChild(makeRowButton(i18n::tr("setup-wizard.login-switch"), ButtonVariant::Default, scale, [this] {
    if (!m_preview) {
      switchLogin();
    }
  }, &m_loginSwitch));
  row->addChild(ui::glyph({.out = &m_loginOn, .glyph = "circle-check", .glyphSize = Tokens::sizeSymbolL * scale, .color = colorSpecFromRole(ColorRole::Success)}));
  card->addChild(std::move(row));
  page->addChild(std::move(card));
  return page;
}

std::unique_ptr<Flex> SetupWizardPanel::buildSearch(float scale) {
  auto page = makePage(kSearch, scale, "setup-wizard.search-title", "setup-wizard.search-body");

  auto card = makeCard(scale);
  const char* rows[][2] = {{"setup-wizard.search-contents", "setup-wizard.on"}, {"setup-wizard.search-folders", "setup-wizard.home"}};
  bool first = true;
  for (const auto& entry : rows) {
    if (!first) {
      addSeparator(card.get(), 12.0F, scale);
    }
    first = false;
    auto row = makeListRow(scale);
    row->addChild(makeGrowLabel(i18n::tr(entry[0]), scale));
    row->addChild(makeLabel(i18n::tr(entry[1]), Tokens::typeBody, scale, ColorRole::Label2, FontWeight::Medium));
    card->addChild(std::move(row));
  }
  page->addChild(std::move(card));
  page->addChild(ui::node({.height = Tokens::spaceP16 * scale}));

  auto alert = makeCard(scale);
  m_searchAlert = alert.get();
  Label* alertText = nullptr;
  alert->addChild(makeAlertRow(scale, &alertText, [this] {
    if (!m_preview) {
      probeSearch();
      syncPages();
    }
  }));
  alertText->setText(i18n::tr("setup-wizard.search-not-running"));
  page->addChild(std::move(alert));

  auto info = makeCard(scale);
  m_searchInfo = info.get();
  auto row = makeListRow(scale);
  row->addChild(ui::glyph({.glyph = "info-circle", .glyphSize = Tokens::sizeSymbol * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
  row->addChild(makeGrowLabel(i18n::tr("setup-wizard.search-no-progress"), scale));
  info->addChild(std::move(row));
  page->addChild(std::move(info));
  return page;
}

std::unique_ptr<Flex> SetupWizardPanel::buildUpdate(float scale) {
  auto page = makePage(kUpdate, scale, "setup-wizard.update-title", "setup-wizard.update-body");

  auto checking = makeCard(scale);
  m_updateChecking = checking.get();
  auto row = makeListRow(scale);
  row->addChild(ui::spinner({.color = colorSpecFromRole(ColorRole::Label2), .spinnerSize = Tokens::sizeSymbol * scale, .spinning = true}));
  row->addChild(makeGrowLabel(i18n::tr("setup-wizard.update-checking"), scale));
  checking->addChild(std::move(row));
  page->addChild(std::move(checking));

  auto result = makeCard(scale);
  m_updateResult = result.get();
  auto two = makeListRow(scale, Tokens::sizeRowTwo);
  two->setGap(Tokens::spaceP12 * scale);
  two->addChild(ui::glyph({.out = &m_updateGlyph, .glyph = "download", .glyphSize = Tokens::sizeSymbolL * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
  auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
  auto title = makeLabel("", Tokens::typeBody, scale, ColorRole::Label1, FontWeight::Medium);
  title->setMaxLines(1);
  m_updateTitleLabel = title.get();
  text->addChild(std::move(title));
  auto sub = makeLabel("", Tokens::typeFootnote, scale, ColorRole::Label2, FontWeight::Medium);
  sub->setMaxLines(1);
  sub->setMaxWidth(440.0F * scale);
  m_updateSubLabel = sub.get();
  text->addChild(std::move(sub));
  two->addChild(std::move(text));
  two->addChild(makeRowButton(i18n::tr("setup-wizard.try-again"), ButtonVariant::Ghost, scale, [this] {
    if (!m_preview) {
      checkUpdates();
    }
  }, &m_updateRetry));
  result->addChild(std::move(two));
  page->addChild(std::move(result));
  return page;
}

std::unique_ptr<Flex> SetupWizardPanel::buildDone(float scale) {
  auto page = ui::column({
      .out = &m_pages[kDone],
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .paddingH = Tokens::spaceP24 * scale,
      .flexGrow = 1.0F,
  });
  page->addChild(ui::glyph({
      .glyph = "circle-check",
      .glyphSize = 48.0F * scale,
      .color = colorSpecFromRole(ColorRole::Success),
  }));
  page->addChild(ui::node({.height = Tokens::spaceP16 * scale}));
  page->addChild(makeLabel(i18n::tr("setup-wizard.done-title"), Tokens::typeLarge, scale, ColorRole::Label1, FontWeight::SemiBold));
  page->addChild(ui::node({.height = Tokens::spaceP8 * scale}));
  page->addChild(makeLabel(i18n::tr("setup-wizard.done-body"), Tokens::typeBody, scale, ColorRole::Label2, FontWeight::Medium));
  page->addChild(ui::node({.height = Tokens::spaceP24 * scale}));

  auto column = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP16 * scale, .width = kHeroTextWidth * scale});
  auto tips = makeCard(scale);
  {
    auto row = makeListRow(scale);
    row->addChild(ui::glyph({.glyph = "search", .glyphSize = Tokens::sizeSymbol * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
    row->addChild(makeGrowLabel(i18n::tr("setup-wizard.tip-search"), scale));
    auto keys = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * scale});
    // config.toml binds a Super tap to the launcher; Super+Space is the scratchpad.
    keys->addChild(makeKeycap("Super", scale));
    row->addChild(std::move(keys));
    tips->addChild(std::move(row));
  }
  const char* chips[][3] = {
      {"layout-grid", "setup-wizard.tip-control-center", "setup-wizard.tip-control-center-how"},
      {"arrow-bar-to-right", "setup-wizard.tip-workspaces", "setup-wizard.tip-workspaces-how"},
  };
  for (const auto& tip : chips) {
    addSeparator(tips.get(), 36.0F, scale);
    auto row = makeListRow(scale);
    row->addChild(ui::glyph({.glyph = tip[0], .glyphSize = Tokens::sizeSymbol * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
    row->addChild(makeGrowLabel(i18n::tr(tip[1]), scale));
    row->addChild(makeChip(i18n::tr(tip[2]), scale));
    tips->addChild(std::move(row));
  }
  column->addChild(std::move(tips));
  page->addChild(std::move(column));
  return page;
}

bool SetupWizardPanel::pageShown(int page) const {
  if (page == kUnlock) {
    return m_face.present || m_finger.present;
  }
  if (page == kUpdate) {
    return m_updateChecker;
  }
  if (page == kLogin) {
    return m_loginState != LoginState::Hidden;
  }
  if (page == kPhone) {
    return m_linkInstalled;
  }
  if (page == kKeyboard) {
    return m_compositorSettings != nullptr && !m_layoutNames.empty();
  }
  if (page == kTimezone) {
    // Shown while ListTimezones is on its way; hidden once it failed (no timedated).
    return m_timedate != nullptr && !m_timedate->timezonesFailed();
  }
  if (page == kApps) {
    return m_appRows > 0;
  }
  return true;
}

void SetupWizardPanel::advance(int direction) {
  if (m_enrolling != Method::None) {
    stopEnrol();
  }
  int next = m_step + direction;
  while (next >= 0 && next < kPageCount && !pageShown(next)) {
    next += direction;
  }
  if (next >= kPageCount) {
    commit();
    return;
  }
  if (next >= 0) {
    setStep(next);
  }
}

void SetupWizardPanel::setStep(int step) {
  step = std::clamp(step, 0, kPageCount - 1);
  if (step != m_step) {
    leavePage(m_step);
  }
  m_step = step;
  kLog.info("page {}", kPageNames[m_step]);
  if (!m_preview) {
    enterPage(m_step);
  }
  syncPages();
}

void SetupWizardPanel::enterPage(int page) {
  if (page == kPhone) {
    syncPhone();
    m_phoneTimer.startRepeating(kPairingPoll, [this] {
      syncPhone();
      syncPages();
    });
  } else if (page == kKeyboard) {
    syncKeyboard();
  } else if (page == kTimezone) {
    syncTimezone();
  } else if (page == kUnlock) {
    probeUnlock();
  } else if (page == kSearch) {
    probeSearch();
  } else if (page == kUpdate && m_updateState == UpdateState::Idle) {
    checkUpdates();
  }
}

void SetupWizardPanel::leavePage(int page) {
  if (page == kPhone) {
    m_phoneTimer.stop();
    m_pairingRequested = false;
    if (m_link != nullptr && m_link->pairing().has_value()) {
      m_link->cancelPairing();
    }
  } else if (page == kUnlock) {
    stopEnrol();
  }
}

void SetupWizardPanel::refreshUi() {
  if (m_root != nullptr) {
    m_root->markLayoutDirty();
    PanelManager::instance().refresh();
  }
}

void SetupWizardPanel::syncPages() {
  if (m_root == nullptr) {
    return;
  }
  // A re-probe can hide the open page (Unlock with no hardware left); every later page path ends at Done.
  if (!m_preview && !pageShown(m_step)) {
    advance(1);
    return;
  }
  const float scale = contentScale();
  int shown = 0;
  int current = 0;
  for (int i = 0; i < kPageCount; ++i) {
    const auto index = static_cast<std::size_t>(i);
    show(m_pages[index], i == m_step);
    if (i == m_step) {
      current = shown;
    }
    if (pageShown(i)) {
      ++shown;
    }
  }
  for (int i = 0; i < kPageCount; ++i) {
    Flex* dot = m_dots[static_cast<std::size_t>(i)];
    show(dot, i < shown);
    dot->setSize((i == current ? Style::spaceLg : kPageDotSize) * scale, kPageDotSize * scale);
    dot->setFill(colorSpecFromRole(i == current ? ColorRole::Label1 : ColorRole::Label3));
  }

  const bool unlockSet = m_face.enrolled > 0 || m_finger.enrolled > 0;
  m_leadButton->setText(i18n::tr(m_step == kWelcome ? "setup-wizard.skip" : "setup-wizard.back"));
  m_leadButton->setVariant(m_step == kWelcome ? ButtonVariant::Ghost : ButtonVariant::Default);
  show(m_laterButton,
       (m_step == kPhone && m_phoneState != PhoneState::Paired) || (m_step == kUnlock && !unlockSet) || m_step == kUpdate
           || (m_step == kLogin && m_loginState != LoginState::On));
  m_primaryButton->setText(i18n::tr(
      m_step == kWelcome ? "setup-wizard.get-started" : (m_step == kDone ? "setup-wizard.done" : "setup-wizard.continue")
  ));
  const bool primaryOn = m_step != kPhone || m_phoneState == PhoneState::Paired;
  m_primaryButton->setEnabled(primaryOn);
  m_primaryButton->setOpacity(primaryOn ? 1.0F : 0.38F);

  const bool linkUp = m_phoneState == PhoneState::Waiting || m_phoneState == PhoneState::Paired;
  show(m_phoneCard, linkUp);
  show(m_phoneAlert, !linkUp);
  const bool paired = m_phoneState == PhoneState::Paired;
  const bool qrShown = !paired && !m_phoneUri.empty();
  show(m_qrGlyph, !paired && !qrShown);
  show(m_qrImage, qrShown);
  if (qrShown) {
    m_qrBox->setFill(rgba(1.0F, 1.0F, 1.0F));
  } else {
    m_qrBox->setFill(colorSpecFromRole(ColorRole::Fill1));
  }
  show(m_pairedGlyph, paired);
  show(m_phoneCodeLabel, !paired && !m_phoneCode.empty());
  show(m_phoneStatus, !paired);
  m_phoneHeadline->setText(paired ? i18n::tr("setup-wizard.phone-paired-title", "name", m_phoneDetail) : i18n::tr("setup-wizard.phone-headline"));
  m_phoneBody->setText(i18n::tr(paired ? "setup-wizard.phone-paired-body" : "setup-wizard.phone-instructions"));
  m_phoneCodeLabel->setText(spacedCode(m_phoneCode));
  m_phoneAlertText->setText(
      m_phoneState == PhoneState::Failed ? i18n::tr("setup-wizard.phone-failed", "reason", m_phoneDetail) : i18n::tr("setup-wizard.phone-not-running")
  );

  const bool enrolling = m_enrolling != Method::None;
  show(m_methodCard, !enrolling);
  show(m_enrolCard, enrolling);
  for (UnlockMethod* m : {&m_face, &m_finger}) {
    const bool face = m == &m_face;
    show(m->row, m->present);
    show(m->setUp, m->enrolled == 0);
    show(m->on, m->enrolled > 0);
    std::string sub;
    if (m->failed) {
      sub = i18n::tr("setup-wizard.unlock-failed");
    } else if (m->enrolled > 0) {
      sub = i18n::trp(face ? "setup-wizard.faces-enrolled" : "setup-wizard.fingers-enrolled", m->enrolled);
    } else {
      sub = i18n::tr(face ? "setup-wizard.face-found" : "setup-wizard.fingerprint-found");
    }
    m->sub->setText(sub);
    m->sub->setColor(colorSpecFromRole(m->failed ? ColorRole::DangerText : ColorRole::Label2));
  }
  show(m_unlockSeparator, m_face.present && m_finger.present);
  show(m_enrolFaceGlyph, m_enrolling == Method::Face);
  show(m_enrolFingerGlyph, m_enrolling == Method::Finger);
  m_enrolRing->setProgress(m_enrolProgress);
  m_enrolHeadline->setText(i18n::tr(m_enrolling == Method::Finger ? "setup-wizard.finger-touch" : "setup-wizard.face-look"));
  m_enrolPromptLabel->setText(m_enrolPrompt);

  const bool loginOn = m_loginState == LoginState::On;
  show(m_loginSwitch, m_loginState == LoginState::Offer || m_loginState == LoginState::Failed);
  show(m_loginOn, loginOn);
  std::string loginSub;
  if (m_loginState == LoginState::Failed) {
    loginSub = m_loginError;
  } else if (m_loginState == LoginState::Switching) {
    loginSub = i18n::tr("setup-wizard.login-switching");
  } else if (loginOn) {
    loginSub = m_loginCurrent.empty() ? i18n::tr("setup-wizard.login-on") : i18n::tr("setup-wizard.login-on-keeps", "name", m_loginCurrent);
  } else {
    loginSub = m_loginCurrent.empty() ? i18n::tr("setup-wizard.login-now-none") : i18n::tr("setup-wizard.login-now", "name", m_loginCurrent);
  }
  m_loginSubLabel->setText(loginSub);
  m_loginSubLabel->setColor(colorSpecFromRole(m_loginState == LoginState::Failed ? ColorRole::DangerText : ColorRole::Label2));

  show(m_searchAlert, m_searchState == SearchState::NotRunning);
  show(m_searchInfo, m_searchState == SearchState::NoProgress);

  show(m_updateChecking, m_updateState == UpdateState::Checking || m_updateState == UpdateState::Idle);
  show(m_updateResult, m_updateState != UpdateState::Checking && m_updateState != UpdateState::Idle);
  show(m_updateRetry, m_updateState == UpdateState::Failed);
  m_updateGlyph->setGlyph(
      m_updateState == UpdateState::Available ? "download" : (m_updateState == UpdateState::Failed ? "alert-circle" : "circle-check")
  );
  m_updateTitleLabel->setText(m_updateTitle);
  m_updateSubLabel->setText(m_updateSub);
  refreshUi();
}

void SetupWizardPanel::syncSelection() {
  if (m_root == nullptr) {
    return;
  }
  const float scale = contentScale();
  for (std::size_t i = 0; i < m_modeRings.size(); ++i) {
    const bool on = static_cast<int>(i) == m_mode;
    setRing(m_modeRings[i], on, ColorRole::Label1, scale);
    if (m_modeLabels[i] != nullptr) {
      m_modeLabels[i]->setColor(colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2));
      m_modeLabels[i]->setFontWeight(on ? FontWeight::SemiBold : FontWeight::Medium);
    }
  }
  for (std::size_t i = 0; i < m_layoutRings.size(); ++i) {
    const bool on = desktop_layout::kPresets[i] == m_layoutPick;
    setRing(m_layoutRings[i], on, ColorRole::Accent, scale);
    if (m_layoutLabels[i] != nullptr) {
      m_layoutLabels[i]->setColor(colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2));
      m_layoutLabels[i]->setFontWeight(on ? FontWeight::SemiBold : FontWeight::Medium);
    }
  }
  const bool match = m_paletteSource == PaletteSource::Wallpaper;
  for (std::size_t i = 0; i < m_swatchRings.size(); ++i) {
    setRing(m_swatchRings[i], m_swatchNames[i] == m_builtinPalette, ColorRole::Label3, scale);
  }
  show(m_colourRow, !match);
  if (m_matchSeparator != nullptr) {
    m_matchSeparator->setVisible(!match);
  }
  show(m_schemeSelect, match);
  for (std::size_t i = 0; i < m_wallpaperRings.size(); ++i) {
    const bool on = static_cast<int>(i) == m_wallpaper && (i == 0 || !m_pickedPath.empty());
    setRing(m_wallpaperRings[i], on, ColorRole::Label1, scale);
    if (m_wallpaperLabels[i] != nullptr) {
      m_wallpaperLabels[i]->setColor(colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2));
      m_wallpaperLabels[i]->setFontWeight(on ? FontWeight::SemiBold : FontWeight::Medium);
    }
  }
  if (m_addGlyph != nullptr) {
    m_addGlyph->setVisible(m_pickedPath.empty());
  }
  refreshUi();
}

void SetupWizardPanel::chooseWallpaper() {
  FileDialogOptions options;
  options.mode = FileDialogMode::Open;
  options.defaultViewMode = FileDialogViewMode::Grid;
  options.title = i18n::tr("setup-wizard.select-wallpaper");
  options.extensions = DirectoryScanner::imageExtensionFilter(false);
  std::filesystem::path startDir;
  if (!m_wallpaperDir.empty()) {
    startDir = m_wallpaperDir;
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    startDir = std::filesystem::path(home) / "Pictures";
  }
  options.startDirectory = std::move(startDir);
  (void)FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> result) {
    if (!result.has_value()) {
      return;
    }
    m_pickedPath = result->string();
    m_pickedLoaded = false;
    m_wallpaperDir = result->parent_path().string();
    m_wallpaper = 1;
    if (m_root != nullptr && m_wallpaperLabels[1] != nullptr) {
      m_wallpaperLabels[1]->setText(result->filename().string());
    }
    m_config->setOverride({"wallpaper", "directory"}, m_wallpaperDir);
    m_config->setWallpaperPath(std::nullopt, m_pickedPath);
    syncSelection();
  });
}

void SetupWizardPanel::syncPhone() {
  if (m_preview) {
    return;
  }
  if (m_link == nullptr || !m_link->available()) {
    m_phoneState = PhoneState::NotRunning;
    m_pairingRequested = false;
    return;
  }
  const auto& outcome = m_link->outcome();
  if (outcome.has_value() && outcome->paired) {
    m_phoneState = PhoneState::Paired;
    m_phoneDetail = outcome->detail;
  } else if (const auto& devices = m_link->devices(); !devices.empty()) {
    m_phoneState = PhoneState::Paired;
    m_phoneDetail = devices.front().name;
  } else if (const auto& pairing = m_link->pairing(); pairing.has_value()) {
    m_phoneState = PhoneState::Waiting;
    m_phoneCode = pairing->code;
    m_phoneUri = pairing->uri;
    m_pairingRequested = false;
  } else if (outcome.has_value()) {
    m_phoneState = PhoneState::Failed;
    m_phoneDetail = outcome->detail;
    m_pairingRequested = false;
  } else if (!m_pairingRequested) {
    // No window open: the first visit, or the daemon closed an expired one; open a fresh code.
    startPairing();
  }
}

void SetupWizardPanel::renderQr(Renderer& renderer) {
  if (m_qrImage == nullptr || m_phoneUri == m_qrUri) {
    return;
  }
  m_qrUri = m_phoneUri;
  m_qrImage->clear(renderer);
  if (m_qrUri.empty()) {
    return;
  }
  const std::unique_ptr<QRcode, decltype(&QRcode_free)> qr(
      QRcode_encodeString(m_qrUri.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1), &QRcode_free
  );
  if (qr == nullptr) {
    kLog.warn("pairing QR: cannot encode the URI");
    return;
  }
  // Whole texels per module at twice the drawn size, so scaling keeps the module edges sharp.
  const float drawn = kQrCode * contentScale();
  const int texels = std::max(1, static_cast<int>(std::ceil(2.0F * drawn / static_cast<float>(qr->width))));
  const int side = qr->width * texels;
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(side) * side * 4, 0xFF);
  for (int y = 0; y < side; ++y) {
    for (int x = 0; x < side; ++x) {
      if ((qr->data[((y / texels) * qr->width) + (x / texels)] & 1) != 0) {
        std::memcpy(&rgba[(static_cast<std::size_t>(y) * side + x) * 4], kQrInk, 3);
      }
    }
  }
  (void)m_qrImage->setSourceRaw(renderer, rgba.data(), rgba.size(), side, side, side * 4, PixmapFormat::RGBA);
}

void SetupWizardPanel::startPairing() {
  m_pairingRequested = true;
  m_phoneState = PhoneState::Waiting;
  m_phoneCode.clear();
  m_phoneUri.clear();
  m_link->startPairing();
}

void SetupWizardPanel::probeUnlock() {
  if (m_preview || m_enrolling != Method::None || m_systemBus == nullptr) {
    return;
  }
  if (m_enroll == nullptr) {
    m_enroll = std::make_unique<BiometricEnrollment>(m_systemBus, m_user);
    m_enroll->setChangedCallback([this] {
      if (m_preview) {
        return;
      }
      m_face.present = m_enroll->facePresent();
      m_face.enrolled = static_cast<int>(m_enroll->faces().size());
      m_finger.present = m_enroll->fingerPresent();
      m_finger.enrolled = static_cast<int>(m_enroll->fingers().size());
      m_enrolProgress = m_enroll->progress();
      m_enrolPrompt = m_enroll->prompt();
      syncPages();
    });
    m_enroll->setFinishedCallback([this](BiometricEnrollment::Method /*method*/, bool ok) { finishEnrol(ok); });
  }
  m_enroll->probe();
}

void SetupWizardPanel::startEnrol(Method method) {
  if (m_preview || m_enrolling != Method::None || m_enroll == nullptr) {
    return;
  }
  UnlockMethod& m = method == Method::Face ? m_face : m_finger;
  m.failed = false;
  m_enrolling = method;
  m_enroll->start(
      method == Method::Face ? BiometricEnrollment::Method::Face : BiometricEnrollment::Method::Finger,
      method == Method::Face ? std::string{kFaceName} : std::string{}
  );
}

void SetupWizardPanel::stopEnrol() {
  m_enrolling = Method::None;
  if (m_enroll != nullptr) {
    m_enroll->stop();
  }
}

void SetupWizardPanel::finishEnrol(bool ok) {
  if (m_enrolling == Method::None) {
    return;
  }
  UnlockMethod& m = m_enrolling == Method::Face ? m_face : m_finger;
  m_enrolling = Method::None;
  m.failed = !ok;
  syncPages();
}

void SetupWizardPanel::probeKeyboard() {
  if (m_compositorSettings != nullptr || m_wayland == nullptr) {
    return;
  }
  const auto [name, version] = m_wayland->desktopSettingsGlobal();
  if (name == 0) {
    return;
  }
  m_compositorSettings = std::make_unique<SettingsControl>(m_wayland->registry(), name, version, [this] { syncKeyboard(); });
  if (m_systemBus == nullptr) {
    return;
  }
  try {
    auto locale = std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(
        m_systemBus->connection(), sdbus::ServiceName{"org.freedesktop.locale1"}, sdbus::ObjectPath{"/org/freedesktop/locale1"}
    ));
    locale->getPropertyAsync("X11Layout").onInterface("org.freedesktop.locale1").uponReplyInvoke(
        [this, locale, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error, sdbus::Variant value) {
          if (alive.expired() || error.has_value()) {
            return;
          }
          const auto layouts = value.get<std::string>();
          m_systemLayout = layouts.substr(0, layouts.find(','));
          syncKeyboard();
        }
    );
  } catch (const sdbus::Error& e) {
    kLog.debug("no localed keymap: {}", e.what());
  }
}

// The compositor's layout key holds the input sources comma separated; unset, the compositor uses the system
// (localed) keymap, which the Select then leaves unselected until the user picks one.
void SetupWizardPanel::syncKeyboard() {
  if (m_root == nullptr || m_compositorSettings == nullptr || m_layoutSelect == nullptr) {
    return;
  }
  const std::string value = m_compositorSettings->value(kLayoutKey);
  const std::string first = value.empty() ? m_systemLayout : value.substr(0, value.find(','));
  if (!first.empty() && first != m_layout) {
    m_layout = first;
    if (const auto it = std::ranges::find(m_layoutNames, first); it != m_layoutNames.end()) {
      m_layoutSelect->setSelectedIndexSilently(static_cast<std::size_t>(it - m_layoutNames.begin()));
    }
  }
  syncPages();
}

void SetupWizardPanel::syncTimezone() {
  if (m_root == nullptr || m_timedate == nullptr || m_zoneSelect == nullptr) {
    return;
  }
  const auto& zones = m_timedate->timezones();
  if (zones.size() != m_zoneCount) {
    m_zoneCount = zones.size();
    m_zoneSelect->setOptions(zones);
  }
  if (const auto it = std::ranges::find(zones, m_timedate->timezone()); it != zones.end()) {
    m_zoneSelect->setSelectedIndexSilently(static_cast<std::size_t>(it - zones.begin()));
  } else {
    m_zoneSelect->clearSelection();
  }
  m_ntpToggle->setChecked(m_timedate->ntp());
  m_zoneError->setText(m_timedate->lastError().empty() ? std::string{} : i18n::tr("setup-wizard.timezone-failed"));
  syncPages();
}

void SetupWizardPanel::probeLogin() {
  // A refused switch keeps its message when the wizard opens again after the polkit panel.
  if (m_loginState == LoginState::Failed && display_manager::activeUnit() != "greetd.service") {
    return;
  }
  m_loginState = LoginState::Hidden;
  const auto dms = display_manager::installed();
  const std::string unit = display_manager::activeUnit();
  if (dms.empty() || dms.front().unit != "greetd.service" || unit == "greetd.service") {
    return;
  }
  m_loginCurrent = unit.empty() ? std::string{} : display_manager::nameOf(unit);
  m_loginState = LoginState::Offer;
}

// The old display manager stays installed; Settings › Users switches back.
void SetupWizardPanel::switchLogin() {
  m_loginState = LoginState::Switching;
  // The polkit password panel replaces this one; the reply opens the wizard again where it was.
  m_awaitingAuth = true;
  syncPages();
  display_manager::switchTo("greetd.service", [this, alive = std::weak_ptr<bool>(m_alive)](bool ok, bool denied, std::string error) {
    if (alive.expired()) {
      return;
    }
    m_awaitingAuth = false;
    m_loginState = ok ? LoginState::On : LoginState::Failed;
    if (ok) {
      kLog.info("login screen: greetd enabled");
    } else {
      m_loginError = denied ? i18n::tr("setup-wizard.login-denied") : i18n::tr("setup-wizard.login-failed", "reason", error);
      kLog.warn("login screen: enabling greetd failed: {}", error);
    }
    if (m_root == nullptr) {
      PanelManager::instance().openPanel("setup-wizard", PanelOpenRequest{.context = kPageNames[kLogin]});
      return;
    }
    syncPages();
  });
}

void SetupWizardPanel::probeSearch() {
  if (!m_preview) {
    m_searchState = indexerReachable() ? SearchState::NoProgress : SearchState::NotRunning;
  }
}

void SetupWizardPanel::checkUpdates() {
  SoftwareUpdateService* updates = m_updates ? m_updates() : nullptr;
  if (updates == nullptr) {
    m_updateState = UpdateState::Failed;
    m_updateTitle = i18n::tr("setup-wizard.update-failed");
    m_updateSub.clear();
    syncPages();
    return;
  }
  m_updateState = UpdateState::Checking;
  m_updateCheckStart =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  syncPages();
  updates->check();
}

void SetupWizardPanel::onSoftwareUpdateChanged() {
  SoftwareUpdateService* updates = m_updates ? m_updates() : nullptr;
  if (m_preview || m_updateState != UpdateState::Checking || updates == nullptr) {
    return;
  }
  if (!updates->available()) {
    m_updateState = UpdateState::Failed;
    m_updateTitle = i18n::tr("setup-wizard.update-failed");
    m_updateSub.clear();
    syncPages();
    return;
  }
  if (updates->checking() || updates->lastCheck() < m_updateCheckStart) {
    return;
  }
  // The wizard reports the System group (pacman or apt), as the Software Update hero does.
  std::vector<const SoftwareUpdate*> system;
  for (const auto& update : updates->updates()) {
    if (update.group == "system") {
      system.push_back(&update);
    }
  }
  const auto& errors = updates->errors();
  const auto failed = std::ranges::find_if(errors, [](const std::string& e) { return e.starts_with("system: "); });
  if (failed != errors.end()) {
    m_updateState = UpdateState::Failed;
    m_updateTitle = i18n::tr("setup-wizard.update-failed");
    m_updateSub = failed->substr(std::string_view("system: ").size());
  } else if (system.empty()) {
    m_updateState = UpdateState::UpToDate;
    m_updateTitle = i18n::tr("setup-wizard.update-current");
    m_updateSub = i18n::tr("setup-wizard.update-checked");
  } else {
    const auto count = static_cast<long>(system.size());
    const auto lead = std::ranges::find_if(system, [](const SoftwareUpdate* u) { return u->name.starts_with("andromeda"); });
    const SoftwareUpdate& shown = lead != system.end() ? **lead : *system.front();
    m_updateState = UpdateState::Available;
    m_updateTitle = i18n::trp("setup-wizard.update-available", count);
    m_updateSub = count == 1
        ? i18n::tr("setup-wizard.update-one", "name", shown.name, "version", shown.newVersion)
        : i18n::trp("setup-wizard.update-summary", count - 1, "name", shown.name, "version", shown.newVersion);
  }
  syncPages();
}

void SetupWizardPanel::layoutModeArt(const ModeArt& art) {
  const float s = contentScale();
  const float w = art.box->width();
  const float h = art.box->height();
  art.wallpaper->setPosition(0.0F, 0.0F);
  art.wallpaper->setSize(w, h);
  const float r4 = Style::scaledRadius(Tokens::radiusR4, s);
  const float r8 = Style::scaledRadius(Tokens::radiusR8, s);
  const float wx = 32.0F * s;
  const float wy = 24.0F * s;
  const float ww = w - 64.0F * s;
  const float wh = h - 36.0F * s;
  auto place = [](Flex* part, float x, float y, float width, float height, const Radii& radii) {
    part->setPosition(x, y);
    part->setSize(width, height);
    part->setRadii(radii);
  };
  place(art.parts[0], 8.0F * s, 8.0F * s, 40.0F * s, 8.0F * s, r4);
  place(art.parts[1], w - 36.0F * s, 8.0F * s, 28.0F * s, 8.0F * s, r4);
  place(art.parts[2], wx, wy, ww, wh, r8);
  place(art.parts[3], wx + 8.0F * s, wy + 8.0F * s, ww - 16.0F * s, 4.0F * s, 2.0F * s);
  if (art.parts.size() > 4) {
    const float half = w * 0.5F;
    place(art.parts[4], half, wy, wx + ww - half, wh, Radii(0.0F, r8, r8, 0.0F));
    place(art.parts[5], half, wy + 8.0F * s, wx + ww - 8.0F * s - half, 4.0F * s, Radii(0.0F, 2.0F * s, 2.0F * s, 0.0F));
  }
}

void SetupWizardPanel::doLayout(Renderer& renderer, float width, float height) {
  const float scale = contentScale();
  if (m_logo != nullptr && !m_logo->hasImage()) {
    m_logo->setSourceFile(renderer, paths::assetPath("andromeda.svg").string(), static_cast<int>(96.0F * scale));
  }
  const std::string bundled = paths::assetPath("andromeda-wallpaper.png").string();
  const int thumbSize = static_cast<int>(kThumbWidth * scale);
  for (auto& art : m_modeArts) {
    if (!art.wallpaper->hasImage()) {
      art.wallpaper->setSourceFile(renderer, bundled, thumbSize);
    }
  }
  if (m_bundledImage != nullptr && !m_bundledImage->hasImage()) {
    m_bundledImage->setSourceFile(renderer, bundled, thumbSize);
  }
  if (m_pickedImage != nullptr && !m_pickedLoaded && !m_pickedPath.empty()) {
    m_pickedLoaded = m_pickedImage->reloadSourceFile(renderer, m_pickedPath, thumbSize);
  }
  if (m_pickedImage != nullptr) {
    m_pickedImage->setVisible(!m_pickedPath.empty());
  }
  renderQr(renderer);
  if (m_root != nullptr) {
    m_root->setPosition(0.0F, 0.0F);
    m_root->setSize(width, height);
    m_root->layout(renderer);
  }
  for (const auto& art : m_modeArts) {
    layoutModeArt(art);
  }
  if (m_matchSeparator != nullptr && m_matchSeparator->parent() != nullptr) {
    m_matchSeparator->setPosition(Tokens::spaceP12 * scale, 0.0F);
    m_matchSeparator->setSize(m_matchSeparator->parent()->width() - Tokens::spaceP8 * scale, scale);
  }
  if (m_pickedImage != nullptr && m_pickedImage->parent() != nullptr) {
    m_pickedImage->setPosition(0.0F, 0.0F);
    m_pickedImage->setSize(m_pickedImage->parent()->width(), m_pickedImage->parent()->height());
  }
  if (m_enrolRing != nullptr) {
    m_enrolRing->setColor(colorForRole(ColorRole::Accent));
    m_enrolRing->setPosition(0.0F, 0.0F);
    m_enrolRing->setSize(kEnrolCircle * scale, kEnrolCircle * scale);
  }
  for (const auto& overlay : m_overlays) {
    const float hx = overlay.host->parent() == overlay.node->parent() ? overlay.host->x() : 0.0F;
    const float hy = overlay.host->parent() == overlay.node->parent() ? overlay.host->y() : 0.0F;
    overlay.node->setPosition(hx - overlay.outset, hy - overlay.outset);
    const float grow = 2.0F * overlay.outset;
    overlay.node->setSize(overlay.host->width() + grow, overlay.host->height() + grow);
  }
}

bool SetupWizardPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool /*preedit*/) {
  if (!pressed) {
    return false;
  }
  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    if (m_enrolling != Method::None) {
      stopEnrol();
      syncPages();
    } else if (m_step == kWelcome) {
      commit();
    } else {
      advance(-1);
    }
    return true;
  }
  if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && modifiers == 0) {
    if (m_primaryButton != nullptr && m_primaryButton->enabled()) {
      advance(1);
    }
    return true;
  }
  return false;
}

void SetupWizardPanel::commit() {
  if (m_config != nullptr) {
    (void)m_config->markSetupWizardCompleted();
  }
  kLog.info("setup complete");
  PanelManager::instance().close();
}

void SetupWizardPanel::onOpen(std::string_view context) {
  const auto colon = context.find(':');
  const std::string_view name = context.substr(0, colon);
  const std::string_view state = colon == std::string_view::npos ? std::string_view{} : context.substr(colon + 1);
  int page = -1;
  for (int i = 0; i < kPageCount; ++i) {
    if (name == kPageNames[i]) {
      page = i;
    }
  }
  if (page < 0) {
    return;
  }
  // A state after the page name is a still for screenshots: it shows that state's look and calls no backend.
  if (!state.empty()) {
    m_preview = true;
    m_linkInstalled = true;
    m_phoneTimer.stop();
    if (state == "norun") {
      m_phoneState = PhoneState::NotRunning;
      m_searchState = SearchState::NotRunning;
    } else if (state == "waiting") {
      m_phoneState = PhoneState::Waiting;
      m_phoneCode = "482913";
      m_phoneUri = "andromeda-link://pair?fp=preview&secret=preview&addr=192.168.1.20:47100";
    } else if (state == "paired") {
      m_phoneState = PhoneState::Paired;
      m_phoneDetail = "Pixel 8";
    } else if (state == "failed" && page == kPhone) {
      m_phoneState = PhoneState::Failed;
      m_phoneDetail = "the code expired";
    } else if (state == "methods" || state == "face" || state == "finger" || state == "fingerdone") {
      m_face = {.present = state != "fingerdone", .row = m_face.row, .sub = m_face.sub, .setUp = m_face.setUp, .on = m_face.on};
      m_finger = {.present = true, .enrolled = state == "fingerdone" ? 2 : 0, .row = m_finger.row, .sub = m_finger.sub, .setUp = m_finger.setUp, .on = m_finger.on};
      m_enrolling = state == "face" ? Method::Face : (state == "finger" ? Method::Finger : Method::None);
      m_enrolProgress = 0.62F;
      m_enrolPrompt = i18n::tr(state == "finger" ? "setup-wizard.finger-again" : "setup-wizard.face-turn");
    } else if (page == kLogin) {
      m_loginState = state == "on" ? LoginState::On : (state == "failed" ? LoginState::Failed : LoginState::Offer);
      m_loginCurrent = state == "none" ? std::string{} : "GDM";
      m_loginError = i18n::tr("setup-wizard.login-denied");
    } else if (state == "noprogress") {
      m_searchState = SearchState::NoProgress;
    } else if (state == "checking") {
      m_updateChecker = true;
      m_updateState = UpdateState::Checking;
    } else if (state == "current") {
      m_updateChecker = true;
      m_updateState = UpdateState::UpToDate;
      m_updateTitle = i18n::tr("setup-wizard.update-current");
      m_updateSub = i18n::tr("setup-wizard.update-checked");
    } else if (state == "available") {
      m_updateChecker = true;
      m_updateState = UpdateState::Available;
      m_updateTitle = i18n::trp("setup-wizard.update-available", 23);
      m_updateSub = i18n::trp("setup-wizard.update-summary", 22, "name", "andromeda", "version", "0.9.2-1");
    } else if (state == "failed") {
      m_updateChecker = true;
      m_updateState = UpdateState::Failed;
      m_updateTitle = i18n::tr("setup-wizard.update-failed");
      m_updateSub = "Cannot fetch updates";
    }
  }
  setStep(page);
}

void SetupWizardPanel::onClose() {
  m_phoneTimer.stop();
  if (m_link != nullptr && m_link->pairing().has_value()) {
    m_link->cancelPairing();
  }
  stopEnrol();
  m_root = nullptr;
  if (m_awaitingAuth) {
    return; // closed by the polkit panel, not by the user: not finished, and its services stay bound
  }
  // Both bind only while the wizard is open; their callbacks touch the page widgets freed with the scene.
  m_compositorSettings.reset();
  m_timedate.reset();
  m_layoutSelect = nullptr;
  m_zoneSelect = nullptr;
  if (m_config != nullptr) {
    (void)m_config->markSetupWizardCompleted();
  }
}
