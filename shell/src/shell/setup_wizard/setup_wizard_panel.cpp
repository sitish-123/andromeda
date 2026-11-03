#include "shell/setup_wizard/setup_wizard_panel.h"

#include "config/config_service.h"
#include "core/files/directory_scanner.h"
#include "core/files/resource_paths.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "theme/builtin_palettes.h"
#include "ui/builders.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("setup-wizard");
  constexpr int kStepCount = 3;

  // Thumbnail cell: 3 equal columns in the 640 sheet (592 content - 2 x 4 card - 2 x 8 thumbs - 2 x 8 gaps) / 3.
  constexpr float kThumbWidth = 184.0F;
  constexpr float kThumbHeight = 80.0F;
  constexpr float kSwatch = 20.0F;
  constexpr float kRingOutset = 4.0F;

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

  std::unique_ptr<Flex> makeListRow(float scale) {
    return ui::row({
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP8 * scale,
        .paddingH = Tokens::spaceP12 * scale,
        .height = Tokens::sizeRow * scale,
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

} // namespace

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

  auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch});

  auto pages = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
  pages->addChild(buildWelcome(scale));
  pages->addChild(buildAppearance(scale));
  pages->addChild(buildDone(scale));
  root->addChild(std::move(pages));

  auto footer = ui::row({
      .align = FlexAlign::Center,
      .padding = Tokens::spaceP24 * scale,
  });
  const float slotWidth = 160.0F * scale;
  auto lead = ui::row({.align = FlexAlign::Center, .width = slotWidth});
  lead->addChild(ui::button({
      .out = &m_leadButton,
      .text = i18n::tr("setup-wizard.skip"),
      .fontSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Ghost,
      .minWidth = 64.0F * scale,
      .minHeight = Style::controlHeight * scale,
      .paddingH = Tokens::spaceP12 * scale,
      .radius = Style::scaledRadius(Tokens::radiusR8, scale),
      .onClick =
          [this] {
            if (m_step == 0) {
              commit();
            } else {
              setStep(m_step - 1);
            }
          },
  }));
  footer->addChild(std::move(lead));

  auto dots = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = 6.0F * scale,
      .height = 16.0F * scale,
      .flexGrow = 1.0F,
  });
  for (int i = 0; i < kStepCount; ++i) {
    dots->addChild(ui::row({
        .out = &m_dots[static_cast<std::size_t>(i)],
        .radius = 3.0F * scale,
        .width = 6.0F * scale,
        .height = 6.0F * scale,
    }));
  }
  footer->addChild(std::move(dots));

  auto trail = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .width = slotWidth});
  trail->addChild(ui::button({
      .out = &m_primaryButton,
      .text = i18n::tr("setup-wizard.get-started"),
      .fontSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Primary,
      .minWidth = 96.0F * scale,
      .minHeight = Style::controlHeight * scale,
      .paddingH = Tokens::spaceP12 * scale,
      .radius = Style::controlHeight * scale * 0.5F,
      .onClick =
          [this] {
            if (m_step + 1 < kStepCount) {
              setStep(m_step + 1);
            } else {
              commit();
            }
          },
  }));
  footer->addChild(std::move(trail));
  root->addChild(std::move(footer));

  setRoot(std::move(root));
  setStep(0);
  syncSelection();
}

std::unique_ptr<Flex> SetupWizardPanel::buildWelcome(float scale) {
  auto page = ui::column({
      .out = &m_pages[0],
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
  auto page = ui::column({
      .out = &m_pages[1],
      .align = FlexAlign::Stretch,
      .padding = Tokens::spaceP24 * scale,
      .flexGrow = 1.0F,
      .configure = [scale](Flex& f) { f.setPadding(Tokens::spaceP24 * scale, Tokens::spaceP24 * scale, 0.0F, Tokens::spaceP24 * scale); },
  });

  auto header = ui::column({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * scale});
  header->addChild(makeLabel(i18n::tr("setup-wizard.appearance-title"), Tokens::typeTitle1, scale, ColorRole::Label1, FontWeight::SemiBold));
  header->addChild(makeLabel(i18n::tr("setup-wizard.appearance-body"), Tokens::typeBody, scale, ColorRole::Label2, FontWeight::Medium));
  page->addChild(std::move(header));
  page->addChild(ui::node({.height = Tokens::spaceP24 * scale}));

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
    auto colourLabel = makeLabel(i18n::tr("setup-wizard.colour"), Tokens::typeBody, scale, ColorRole::Label1, FontWeight::Medium);
    colourLabel->setFlexGrow(1.0F);
    colour->addChild(std::move(colourLabel));
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
    auto matchLabel = makeLabel(i18n::tr("setup-wizard.match-wallpaper"), Tokens::typeBody, scale, ColorRole::Label1, FontWeight::Medium);
    matchLabel->setFlexGrow(1.0F);
    match->addChild(std::move(matchLabel));
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

std::unique_ptr<Flex> SetupWizardPanel::buildDone(float scale) {
  const auto& cfg = m_config->config();
  auto page = ui::column({
      .out = &m_pages[2],
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
  struct Tip {
    const char* glyph;
    const char* key;
    std::vector<std::string> keys;
  };
  const Tip list[] = {
      {"search", "setup-wizard.tip-search", {"Super", "Space"}},
      {"mood-smile", "setup-wizard.tip-emoji", {"Super", "."}},
      {"clipboard", "setup-wizard.tip-clipboard", {"Super", "V"}},
  };
  bool first = true;
  for (const auto& tip : list) {
    if (!first) {
      auto inset = ui::column({.align = FlexAlign::Stretch});
      inset->setPadding(0.0F, 0.0F, 0.0F, 36.0F * scale);
      inset->addChild(ui::separator({.color = colorSpecFromRole(ColorRole::Separator), .thickness = 1.0F * scale, .spacing = 0.0F}));
      tips->addChild(std::move(inset));
    }
    first = false;
    auto row = makeListRow(scale);
    row->addChild(ui::glyph({.glyph = tip.glyph, .glyphSize = 16.0F * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
    auto label = makeLabel(i18n::tr(tip.key), Tokens::typeBody, scale, ColorRole::Label1, FontWeight::Medium);
    label->setFlexGrow(1.0F);
    row->addChild(std::move(label));
    auto keys = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * scale});
    for (const auto& key : tip.keys) {
      keys->addChild(makeKeycap(key, scale));
    }
    row->addChild(std::move(keys));
    tips->addChild(std::move(row));
  }
  column->addChild(std::move(tips));

  auto privacy = makeCard(scale);
  auto row = makeListRow(scale);
  auto label = makeLabel(i18n::tr("setup-wizard.share-usage"), Tokens::typeBody, scale, ColorRole::Label1, FontWeight::Medium);
  label->setFlexGrow(1.0F);
  row->addChild(std::move(label));
  row->addChild(ui::toggle({.out = &m_telemetryToggle, .checked = cfg.shell.telemetryEnabled, .scale = scale}));
  privacy->addChild(std::move(row));
  column->addChild(std::move(privacy));
  page->addChild(std::move(column));
  return page;
}

void SetupWizardPanel::setStep(int step) {
  m_step = std::clamp(step, 0, kStepCount - 1);
  const float scale = contentScale();
  for (int i = 0; i < kStepCount; ++i) {
    const auto index = static_cast<std::size_t>(i);
    if (m_pages[index] != nullptr) {
      m_pages[index]->setVisible(i == m_step);
      m_pages[index]->setParticipatesInLayout(i == m_step);
    }
    if (m_dots[index] != nullptr) {
      m_dots[index]->setSize((i == m_step ? 16.0F : 6.0F) * scale, 6.0F * scale);
      m_dots[index]->setFill(colorSpecFromRole(i == m_step ? ColorRole::Label1 : ColorRole::Label3));
    }
  }
  if (m_leadButton != nullptr) {
    m_leadButton->setText(i18n::tr(m_step == 0 ? "setup-wizard.skip" : "setup-wizard.back"));
    m_leadButton->setVariant(m_step == 0 ? ButtonVariant::Ghost : ButtonVariant::Default);
  }
  if (m_primaryButton != nullptr) {
    static constexpr const char* kPrimary[] = {"setup-wizard.get-started", "setup-wizard.continue", "setup-wizard.done"};
    m_primaryButton->setText(i18n::tr(kPrimary[m_step]));
  }
  if (m_root != nullptr) {
    m_root->markLayoutDirty();
    PanelManager::instance().refresh();
  }
}

void SetupWizardPanel::syncSelection() {
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
  if (m_colourRow != nullptr) {
    m_colourRow->setVisible(!match);
    m_colourRow->setParticipatesInLayout(!match);
  }
  if (m_matchSeparator != nullptr) {
    m_matchSeparator->setVisible(!match);
  }
  if (m_schemeSelect != nullptr) {
    m_schemeSelect->setVisible(match);
    m_schemeSelect->setParticipatesInLayout(match);
  }
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
  if (m_root != nullptr) {
    m_root->markLayoutDirty();
    PanelManager::instance().refresh();
  }
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
    if (m_wallpaperLabels[1] != nullptr) {
      m_wallpaperLabels[1]->setText(result->filename().string());
    }
    m_config->setOverride({"wallpaper", "directory"}, m_wallpaperDir);
    m_config->setWallpaperPath(std::nullopt, m_pickedPath);
    syncSelection();
  });
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
  for (const auto& overlay : m_overlays) {
    const float hx = overlay.host->parent() == overlay.node->parent() ? overlay.host->x() : 0.0F;
    const float hy = overlay.host->parent() == overlay.node->parent() ? overlay.host->y() : 0.0F;
    overlay.node->setPosition(hx - overlay.outset, hy - overlay.outset);
    overlay.node->setSize(overlay.host->width() + 2.0F * overlay.outset, overlay.host->height() + 2.0F * overlay.outset);
  }
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
  if (context == "appearance") {
    setStep(1);
  } else if (context == "done") {
    setStep(2);
  }
}

void SetupWizardPanel::onClose() {
  if (m_config != nullptr) {
    (void)m_config->markSetupWizardCompleted();
  }
}
