#include "shell/setup_wizard/setup_wizard_panel.h"

#include "config/config_service.h"
#include "core/files/directory_scanner.h"
#include "core/files/resource_paths.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/process/process.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/link/link_service.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/countdown_ring_node.h"
#include "shell/panel/panel_manager.h"
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

  enum PageId : int { kWelcome, kAppearance, kPhone, kUnlock, kSearch, kUpdate, kDone };
  constexpr const char* kPageNames[] = {"welcome", "appearance", "phone", "unlock", "search", "update", "done"};

  // Thumbnail cell: 3 equal columns in the 640 sheet (592 content - 2 x 4 card - 2 x 8 thumbs - 2 x 8 gaps) / 3.
  constexpr float kThumbWidth = 184.0F;
  constexpr float kThumbHeight = 80.0F;
  constexpr float kSwatch = 20.0F;
  constexpr float kRingOutset = 4.0F;
  constexpr float kSlotWidth = 208.0F;
  constexpr float kQrSize = 160.0F;
  constexpr float kQrPadding = 12.0F;
  // Module colour of the mockup QR on its white box.
  constexpr std::uint8_t kQrInk[3] = {0x14, 0x14, 0x18};
  // Text column beside the QR box: 592 content - 8 card - 32 padding - 160 QR - 24 gap.
  constexpr float kQrTextWidth = 368.0F;
  constexpr float kEnrolCircle = 128.0F;

  const sdbus::ServiceName kGazeBusName{"com.gundulabs.Gaze"};
  const sdbus::ObjectPath kGazePath{"/com/gundulabs/Gaze"};
  constexpr auto kGazeInterface = "com.gundulabs.Gaze";
  constexpr auto kFaceName = "Face";

  const sdbus::ServiceName kFprintBusName{"net.reactivated.Fprint"};
  const sdbus::ObjectPath kFprintManagerPath{"/net/reactivated/Fprint/Manager"};
  constexpr auto kFprintManagerInterface = "net.reactivated.Fprint.Manager";
  constexpr auto kFprintDeviceInterface = "net.reactivated.Fprint.Device";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr const char* kFingers[] = {
      "right-index-finger", "left-index-finger", "right-middle-finger", "left-middle-finger", "right-thumb",
      "left-thumb",         "right-ring-finger", "left-ring-finger",    "right-little-finger", "left-little-finger",
  };

  constexpr auto kPairingPoll = std::chrono::seconds(1);
  constexpr auto kUpdateTimeout = std::chrono::seconds(120);

  using FaceEntry = sdbus::Struct<std::string, std::uint32_t, bool, bool>;

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
        .minWidth = 20.0F * scale,
        .height = 20.0F * scale,
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
        .height = 24.0F * scale,
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
  template <typename Call> bool tryCall(const char* what, Call&& call) {
    try {
      call();
      return true;
    } catch (const sdbus::Error& e) {
      kLog.info("could not call {}: {}", what, e.what());
      return false;
    }
  }

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

  std::string facePrompt(const std::string& message) {
    if (message == "Tilt your face slightly up") {
      return i18n::tr("setup-wizard.face-up");
    }
    if (message == "Tilt your face slightly down") {
      return i18n::tr("setup-wizard.face-down");
    }
    if (message == "Turn your face slightly left") {
      return i18n::tr("setup-wizard.face-left");
    }
    if (message == "Turn your face slightly right") {
      return i18n::tr("setup-wizard.face-right");
    }
    if (message == "Face the camera") {
      return i18n::tr("setup-wizard.face-straight");
    }
    return {};
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
    ConfigService* config, WaylandConnection* /*wayland*/, LinkService* link, SystemBus* systemBus
)
    : m_config(config), m_link(link), m_systemBus(systemBus), m_user(currentUser()) {}

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
  m_updateChecker = process::commandExists("checkupdates");

  auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch});

  auto pages = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  pages->addChild(buildWelcome(scale));
  pages->addChild(buildAppearance(scale));
  pages->addChild(buildPhone(scale));
  pages->addChild(buildUnlock(scale));
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
      .gap = 6.0F * scale,
      .height = 16.0F * scale,
      .flexGrow = 1.0F,
  });
  for (int i = 0; i < kPageCount; ++i) {
    dots->addChild(ui::row({
        .out = &m_dots[static_cast<std::size_t>(i)],
        .radius = 3.0F * scale,
        .width = 6.0F * scale,
        .height = 6.0F * scale,
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
  page->addChild(ui::image({.out = &m_logo, .width = 96.0F * scale, .height = 96.0F * scale}));
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
      .width = (kQrSize - 2.0F * kQrPadding) * scale,
      .height = (kQrSize - 2.0F * kQrPadding) * scale,
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
  ring->setThickness(4.0F * scale);
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
  const auto& cfg = m_config->config();
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

  auto column = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP16 * scale, .width = 400.0F * scale});
  auto tips = makeCard(scale);
  {
    auto row = makeListRow(scale);
    row->addChild(ui::glyph({.glyph = "search", .glyphSize = Tokens::sizeSymbol * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
    row->addChild(makeGrowLabel(i18n::tr("setup-wizard.tip-search"), scale));
    auto keys = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * scale});
    keys->addChild(makeKeycap("Super", scale));
    keys->addChild(makeKeycap("Space", scale));
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

  auto privacy = makeCard(scale);
  auto row = makeListRow(scale);
  row->addChild(makeGrowLabel(i18n::tr("setup-wizard.share-usage"), scale));
  row->addChild(ui::toggle({.out = &m_telemetryToggle, .checked = cfg.shell.telemetryEnabled, .scale = scale}));
  privacy->addChild(std::move(row));
  column->addChild(std::move(privacy));
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
    dot->setSize((i == current ? 16.0F : 6.0F) * scale, 6.0F * scale);
    dot->setFill(colorSpecFromRole(i == current ? ColorRole::Label1 : ColorRole::Label3));
  }

  const bool unlockSet = m_face.enrolled > 0 || m_finger.enrolled > 0;
  m_leadButton->setText(i18n::tr(m_step == kWelcome ? "setup-wizard.skip" : "setup-wizard.back"));
  m_leadButton->setVariant(m_step == kWelcome ? ButtonVariant::Ghost : ButtonVariant::Default);
  show(m_laterButton,
       (m_step == kPhone && m_phoneState != PhoneState::Paired) || (m_step == kUnlock && !unlockSet) || m_step == kUpdate);
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
  const float drawn = (kQrSize - 2.0F * kQrPadding) * contentScale();
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
  probeFace();
  probeFinger();
}

void SetupWizardPanel::probeFace() {
  if (m_gaze == nullptr) {
    try {
      m_gaze = sdbus::createProxy(m_systemBus->connection(), kGazeBusName, kGazePath);
      m_gaze->uponSignal("EnrollStatus")
          .onInterface(kGazeInterface)
          .call([this](const std::string& /*name*/, std::uint32_t progress, std::uint32_t max, bool done, const std::string& message, double /*remaining*/) {
            if (m_enrolling != Method::Face || !m_systemBus->isNameOwner(kGazeBusName, m_gaze->getCurrentlyProcessedMessage().getSender())) {
              return;
            }
            m_enrolProgress = max > 0 ? static_cast<float>(progress) / static_cast<float>(max) : 0.0F;
            if (const std::string prompt = facePrompt(message); !prompt.empty()) {
              m_enrolPrompt = prompt;
            }
            if (done) {
              finishEnrol(message == "Completed");
            } else {
              syncPages();
            }
          });
    } catch (const sdbus::Error& e) {
      kLog.debug("no gazed proxy: {}", e.what());
      m_gaze.reset();
      return;
    }
  }
  tryCall("gazed IsCameraAvailable", [&] {
    m_gaze->callMethodAsync("IsCameraAvailable")
        .onInterface(kGazeInterface)
        .uponReplyInvoke([this](std::optional<sdbus::Error> e, bool available) {
          if (m_preview) {
            return;
          }
          m_face.present = !e.has_value() && available;
          if (!m_face.present) {
            syncPages();
            return;
          }
          tryCall("gazed ListFaces", [&] {
            m_gaze->callMethodAsync("ListFaces")
                .onInterface(kGazeInterface)
                .withArguments(m_user)
                .uponReplyInvoke([this](std::optional<sdbus::Error> e, const std::vector<FaceEntry>& faces) {
                  if (m_preview) {
                    return;
                  }
                  m_face.enrolled = e.has_value() ? 0 : static_cast<int>(faces.size());
                  syncPages();
                });
          });
        });
  });
}

void SetupWizardPanel::probeFinger() {
  tryCall("fprintd GetDefaultDevice", [&] {
    auto manager = std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_systemBus->connection(), kFprintBusName, kFprintManagerPath));
    manager->callMethodAsync("GetDefaultDevice")
        .onInterface(kFprintManagerInterface)
        .uponReplyInvoke([this, manager](std::optional<sdbus::Error> e, const sdbus::ObjectPath& path) {
          if (m_preview) {
            return;
          }
          if (e.has_value()) {
            m_finger.present = false;
            syncPages();
            return;
          }
          if (m_fprintDevice == nullptr || m_fingerDevice != path) {
            try {
              m_fprintDevice = sdbus::createProxy(m_systemBus->connection(), kFprintBusName, path);
            } catch (const sdbus::Error& error) {
              kLog.info("no fprintd device proxy: {}", error.what());
              m_fprintDevice.reset();
              return;
            }
            m_fingerDevice = path;
            m_fprintDevice->uponSignal("EnrollStatus").onInterface(kFprintDeviceInterface).call([this](const std::string& result, bool done) {
              if (m_enrolling != Method::Finger) {
                return;
              }
              if (result == "enroll-completed") {
                m_enrolProgress = 1.0F;
                finishEnrol(true);
                return;
              }
              if (result == "enroll-stage-passed") {
                ++m_fingerPassed;
                m_enrolProgress = m_fingerStages > 0 ? static_cast<float>(m_fingerPassed) / static_cast<float>(m_fingerStages) : 0.0F;
                m_enrolPrompt = i18n::tr("setup-wizard.finger-again");
              } else {
                m_enrolPrompt = i18n::tr("setup-wizard.finger-retry");
              }
              if (done) {
                finishEnrol(false);
              } else {
                syncPages();
              }
            });
            m_fprintDevice->callMethodAsync("Get")
                .onInterface(kPropertiesInterface)
                .withArguments(std::string{kFprintDeviceInterface}, std::string{"num-enroll-stages"})
                .uponReplyInvoke([this](std::optional<sdbus::Error> e, const sdbus::Variant& value) {
                  m_fingerStages = e.has_value() ? 0 : value.get<std::int32_t>();
                });
          }
          m_finger.present = true;
          m_fprintDevice->callMethodAsync("ListEnrolledFingers")
              .onInterface(kFprintDeviceInterface)
              .withArguments(m_user)
              .uponReplyInvoke([this](std::optional<sdbus::Error> e, const std::vector<std::string>& fingers) {
                if (m_preview) {
                  return;
                }
                // fprintd answers NoEnrolledPrints instead of an empty list.
                m_fingerNames = e.has_value() ? std::vector<std::string>{} : fingers;
                m_finger.enrolled = static_cast<int>(m_fingerNames.size());
                syncPages();
              });
        });
  });
}

void SetupWizardPanel::startEnrol(Method method) {
  if (m_preview || m_enrolling != Method::None) {
    return;
  }
  UnlockMethod& m = method == Method::Face ? m_face : m_finger;
  m.failed = false;
  m_enrolling = method;
  m_enrolProgress = 0.0F;
  m_fingerPassed = 0;
  m_enrolPrompt = i18n::tr(method == Method::Face ? "setup-wizard.face-turn" : "setup-wizard.finger-place");
  syncPages();

  if (method == Method::Face) {
    if (m_gaze == nullptr || !tryCall("gazed Claim", [&] {
          m_gaze->callMethodAsync("Claim")
              .onInterface(kGazeInterface)
              .withArguments(m_user)
              .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
                if (e.has_value()) {
                  kLog.info("gazed Claim failed: {}", e->what());
                  if (m_enrolling == Method::Face) {
                    finishEnrol(false);
                  }
                  return;
                }
                m_faceClaimed = true;
                // Cancelled while the claim was pending: a held claim blocks face unlock on the lock screen.
                if (m_enrolling != Method::Face) {
                  releaseFace();
                  return;
                }
                tryCall("gazed EnrollStart", [&] {
                  m_gaze->callMethodAsync("EnrollStart")
                      .onInterface(kGazeInterface)
                      .withArguments(std::string{kFaceName})
                      .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
                        if (e.has_value() && m_enrolling == Method::Face) {
                          kLog.info("gazed EnrollStart failed: {}", e->what());
                          finishEnrol(false);
                        }
                      });
                });
              });
        })) {
      finishEnrol(false);
    }
    return;
  }

  std::string finger;
  for (const char* name : kFingers) {
    if (std::ranges::find(m_fingerNames, name) == m_fingerNames.end()) {
      finger = name;
      break;
    }
  }
  if (m_fprintDevice == nullptr || finger.empty() || !tryCall("fprintd Claim", [&] {
        m_fprintDevice->callMethodAsync("Claim")
            .onInterface(kFprintDeviceInterface)
            .withArguments(m_user)
            .uponReplyInvoke([this, finger](std::optional<sdbus::Error> e) {
              if (e.has_value()) {
                kLog.info("fprintd Claim failed: {}", e->what());
                if (m_enrolling == Method::Finger) {
                  finishEnrol(false);
                }
                return;
              }
              m_fingerClaimed = true;
              if (m_enrolling != Method::Finger) {
                releaseFinger();
                return;
              }
              tryCall("fprintd EnrollStart", [&] {
                m_fprintDevice->callMethodAsync("EnrollStart")
                    .onInterface(kFprintDeviceInterface)
                    .withArguments(finger)
                    .uponReplyInvoke([this](std::optional<sdbus::Error> e) {
                      if (e.has_value() && m_enrolling == Method::Finger) {
                        kLog.info("fprintd EnrollStart failed: {}", e->what());
                        finishEnrol(false);
                      }
                    });
              });
            });
      })) {
    finishEnrol(false);
  }
}

void SetupWizardPanel::releaseFace() {
  if (!m_faceClaimed || m_gaze == nullptr) {
    m_faceClaimed = false;
    return;
  }
  m_faceClaimed = false;
  tryCall("gazed EnrollStop", [&] {
    m_gaze->callMethodAsync("EnrollStop").onInterface(kGazeInterface).uponReplyInvoke([](std::optional<sdbus::Error>) {});
    m_gaze->callMethodAsync("Release").onInterface(kGazeInterface).uponReplyInvoke([](std::optional<sdbus::Error>) {});
  });
}

void SetupWizardPanel::releaseFinger() {
  if (!m_fingerClaimed || m_fprintDevice == nullptr) {
    m_fingerClaimed = false;
    return;
  }
  m_fingerClaimed = false;
  tryCall("fprintd EnrollStop", [&] {
    m_fprintDevice->callMethodAsync("EnrollStop").onInterface(kFprintDeviceInterface).uponReplyInvoke([](std::optional<sdbus::Error>) {});
    m_fprintDevice->callMethodAsync("Release").onInterface(kFprintDeviceInterface).uponReplyInvoke([](std::optional<sdbus::Error>) {});
  });
}

void SetupWizardPanel::stopEnrol() {
  m_enrolling = Method::None;
  releaseFace();
  releaseFinger();
}

void SetupWizardPanel::finishEnrol(bool ok) {
  UnlockMethod& m = m_enrolling == Method::Face ? m_face : m_finger;
  stopEnrol();
  m.failed = !ok;
  probeUnlock();
  syncPages();
}

void SetupWizardPanel::probeSearch() {
  if (!m_preview) {
    m_searchState = indexerReachable() ? SearchState::NoProgress : SearchState::NotRunning;
  }
}

void SetupWizardPanel::checkUpdates() {
  m_updateState = UpdateState::Checking;
  syncPages();
  const bool started = process::runAsync(
      std::vector<std::string>{"checkupdates"},
      {.onExit =
           [this](process::RunResult result) {
             if (m_preview) {
               return;
             }
             // checkupdates: 0 = updates listed, 2 = none, anything else = the check failed.
             if (result.exitCode == 0 && !result.timedOut) {
               std::istringstream lines(result.out);
               std::vector<std::string> packages;
               std::string line;
               while (std::getline(lines, line)) {
                 if (!line.empty()) {
                   packages.push_back(line);
                 }
               }
               const auto count = static_cast<long>(packages.size());
               const auto lead = std::ranges::find_if(packages, [](const std::string& p) { return p.starts_with("andromeda"); });
               std::istringstream fields(lead != packages.end() ? *lead : packages.front());
               std::string name;
               std::string oldVersion;
               std::string arrow;
               std::string version;
               fields >> name >> oldVersion >> arrow >> version;
               m_updateState = UpdateState::Available;
               m_updateTitle = i18n::trp("setup-wizard.update-available", count);
               m_updateSub = count == 1 ? i18n::tr("setup-wizard.update-one", "name", name, "version", version)
                                        : i18n::trp("setup-wizard.update-summary", count - 1, "name", name, "version", version);
             } else if (result.exitCode == 2 && !result.timedOut) {
               m_updateState = UpdateState::UpToDate;
               m_updateTitle = i18n::tr("setup-wizard.update-current");
               m_updateSub = i18n::tr("setup-wizard.update-checked");
             } else {
               std::string reason = result.err;
               while (!reason.empty() && (reason.back() == '\n' || reason.back() == ' ')) {
                 reason.pop_back();
               }
               reason = reason.substr(reason.find_last_of('\n') + 1);
               if (const auto colon = reason.find("ERROR: "); colon != std::string::npos) {
                 reason = reason.substr(colon + 7);
               }
               m_updateState = UpdateState::Failed;
               m_updateTitle = i18n::tr("setup-wizard.update-failed");
               m_updateSub = reason;
             }
             syncPages();
           }},
      {.timeout = kUpdateTimeout}
  );
  if (!started) {
    m_updateState = UpdateState::Failed;
    m_updateTitle = i18n::tr("setup-wizard.update-failed");
    m_updateSub.clear();
    syncPages();
  }
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
    overlay.node->setSize(overlay.host->width() + 2.0F * overlay.outset, overlay.host->height() + 2.0F * overlay.outset);
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
  if (m_telemetryToggle != nullptr) {
    m_config->setOverride({"shell", "telemetry_enabled"}, m_telemetryToggle->checked());
  }
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
  if (m_config != nullptr) {
    (void)m_config->markSetupWizardCompleted();
  }
}
