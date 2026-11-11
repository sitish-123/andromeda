#include "shell/launcher/launcher_panel.h"
#include "wayland/drag_icon_surface.h"

#include "shell/launcher/launcher_adapters.h"
#include "shell/launcher/launcher_panel_detail.h"

#include "core/shared_worker.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "shell/panel/panel_surface_style.h"

LauncherPanel::LauncherPanel(ConfigService* config, AccountsService* accounts, AsyncTextureCache* asyncTextures)
    : m_iconResolver(true), m_config(config), m_accounts(accounts), m_asyncTextures(asyncTextures) {}

LauncherPanel::~LauncherPanel() = default;

bool LauncherPanel::usesBrowserLayout() const {
  return m_scopedProviderId.empty()
      && (m_config == nullptr || m_config->config().shell.launcher.layout == ShellConfig::LauncherLayout::Full);
}

float LauncherPanel::preferredWidth() const { return scaled(Tokens::componentLauncherWidth); }

float LauncherPanel::preferredHeight() const { return scaled(usesBrowserLayout() ? launcherSheetHeight() : 500.0F); }

float LauncherPanel::measureContentHeight(Renderer& /*renderer*/, float /*contentWidth*/) {
  const float scale = contentScale();
  const float field = Tokens::componentLauncherSearch * scale;
  // Hybrid material: liquid glass on the empty pill, frosted once it grows into the sheet.
  const float liquid = usesBrowserLayout() && !pillExpanded() ? 1.0F : 0.0F;
  if (m_sentLiquid != liquid) {
    const bool animate = m_sentLiquid >= 0.0F;
    m_sentLiquid = liquid;
    PanelManager::instance().setPanelLiquid(liquid, animate);
  }
  if (!pillExpanded()) {
    return field;
  }
  if (!shouldUseLauncherSections() || m_launcherSectionsAdapter == nullptr || m_results.empty()) {
    return preferredHeight();
  }
  // Field, the taller of the list (p 8, 8 rows visible) and the preview, footer (rulebook 9.14, direction C).
  const bool strip = (m_emojiStrip != nullptr && m_emojiStrip->visible()) || (m_packStrip != nullptr && m_packStrip->visible());
  const float list = 2.0F * Tokens::spaceP8 * scale + m_launcherSectionsAdapter->pillContentHeight(8)
      + (strip ? Tokens::componentLauncherTabs * scale : 0.0F);
  // A clipboard image replaces the 64 icon with a 160 high picture.
  const bool picture = m_selectedIndex < m_results.size()
      && (m_results[m_selectedIndex].presentation == "clipboard-image" || isMediaPresentation(m_results[m_selectedIndex]));
  const float previewHeight = (Tokens::componentLauncherPreviewMin
                               + (picture ? Tokens::componentLauncherPreviewMedia - Tokens::componentLauncherIcon : 0.0F))
      * scale;
  // Mode tabs and the chip row (file kinds, sticker packs) each add a row and a hairline.
  const float rows = (m_modeProviderId.empty() ? 0.0F : 1.0F) + (m_kindTabs != nullptr && m_kindTabs->visible() ? 1.0F : 0.0F);
  const float hairlines = 2.0F + rows; // under the field (and each row) and above the footer
  return field + hairlines + rows * Tokens::componentLauncherTabs * scale + std::max(list, previewHeight)
      + Tokens::componentLauncherFooter * scale;
}

PanelPlacement LauncherPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.launcherPlacement : PanelPlacement::Floating;
}

void LauncherPanel::applyProviderConfig(LauncherProvider& provider) const {
  std::string triggerWord = std::string(provider.defaultPrefix());
  std::string prefix = "/";
  std::optional<bool> global;
  if (m_config != nullptr) {
    const auto& launcherCfg = m_config->config().shell.launcher;
    prefix = launcherCfg.providerPrefix;
    if (provider.allowCustomPrefix()) {
      const std::string key = StringUtils::toLower(provider.id());
      auto it = std::ranges::find(launcherCfg.providers, key, &LauncherProviderConfig::name);
      if (it != launcherCfg.providers.end()) {
        if (!it->prefix.empty()) {
          triggerWord = it->prefix;
        }
        global = it->global;
      }
    }
  }

  if (provider.allowCustomPrefix()) {
    provider.setCustomPrefix(triggerWord.empty() ? std::string() : prefix + triggerWord);
    provider.setCustomIncludeInGlobalSearch(global);
  }
}

void LauncherPanel::finishActivation(
    LauncherProvider& provider, const std::string& resultId, bool copied, bool appLaunch
) {
  const bool trackUsage = m_config == nullptr || m_config->config().shell.launcher.trackUsage;
  if (trackUsage && provider.trackUsage()) {
    m_usageTracker.record(provider.id(), resultId);
  }
  const auto activated = std::ranges::find(m_results, resultId, &LauncherResult::id);
  const bool isImage = activated != m_results.end()
      && (activated->presentation == "clipboard-image" || isMediaPresentation(*activated));
  PanelManager::instance().closePanel(false);
  if (copied && provider.supportsAutoPaste() && m_onCopiedActivation) {
    m_onCopiedActivation(isImage);
  }
  if (appLaunch && m_onAppLaunch) {
    m_onAppLaunch();
  }
}

void LauncherPanel::addProvider(std::unique_ptr<LauncherProvider> provider) {
  applyProviderConfig(*provider);
  provider->initialize();
  LauncherProvider* providerPtr = provider.get();
  provider->setResultsChangedCallback([this, providerPtr] { onProviderResultsChanged(*providerPtr); });
  provider->setQueryRequestedCallback([this](const std::string& query) { setQuery(query); });
  provider->setActivationDoneCallback([this, providerPtr](const std::string& resultId, bool copied) {
    finishActivation(*providerPtr, resultId, copied);
  });
  m_providers.push_back(std::move(provider));
  if (providerPtr->id() == kApplicationsProviderId) {
    m_iconPrefetchTimer.start(std::chrono::seconds(3), [this] { prefetchAppIcons(); });
  }
}

void LauncherPanel::prefetchAppIcons() {
  const auto apps =
      std::ranges::find(m_providers, kApplicationsProviderId, [](const auto& provider) { return provider->id(); });
  if (m_iconPrefetching || apps == m_providers.end()) {
    return;
  }
  // The size the next open resolves at: create() sets the layout mode and the panel scale only on open.
  const int size = static_cast<int>(std::round(launcherIconSize(launcherListStyleFrom(
      usesBrowserLayout(), m_config, shell::panel_surface::contentScale(m_config), panelCardOpacity()
  ))));
  const std::uint64_t key = (desktopEntriesVersion() << 32U) ^ (IconResolver::themeGeneration() << 16U)
      ^ static_cast<std::uint64_t>(size);
  if (key == m_iconPrefetchKey) {
    return;
  }
  std::vector<std::string> names{"application-x-executable"};
  for (const auto& result : (*apps)->query("")) {
    if (!result.iconName.empty() && result.iconPath.empty()) {
      names.push_back(result.iconName);
    }
  }
  struct Prefetch {
    std::uint64_t generation = 0;
    std::vector<std::pair<std::string, std::string>> paths;
  };
  m_iconPrefetching = true;
  SharedWorker::instance().post<Prefetch>(
      m_alive,
      [names = std::move(names), size] {
        IconResolver resolver;
        Prefetch out{.generation = IconResolver::themeGeneration(), .paths = {}};
        for (const auto& name : names) {
          out.paths.emplace_back(name, resolver.resolve(name, size));
        }
        return out;
      },
      [this, size, key](Prefetch prefetch) {
        m_iconPrefetching = false;
        const bool first = m_iconPrefetchKey == 0;
        m_iconPrefetchKey = key;
        for (auto& [name, path] : prefetch.paths) {
          m_iconResolver.prime(name, size, std::move(path), prefetch.generation);
        }
        if (first) {
          warmModeText();
        }
      }
  );
}

void LauncherPanel::warmModeText() {
  const float scale = shell::panel_surface::contentScale(m_config);
  const auto firstPage = [this](std::string_view id) {
    const auto provider = std::ranges::find(m_providers, id, [](const auto& p) { return p->id(); });
    return provider != m_providers.end() ? (*provider)->query("") : std::vector<LauncherResult>{};
  };
  constexpr std::size_t kRows = 8;
  const auto emoji = firstPage("Emoji");
  for (std::size_t i = 0; i < std::min(emoji.size(), kRows * kEmojiColumns); ++i) {
    m_textWarmup.emplace_back(emoji[i].badge, Tokens::componentLauncherEmoji * scale);
  }
  if (!emoji.empty()) {
    m_textWarmup.emplace_back(emoji.front().badge, Tokens::componentLauncherEmojiPreview * scale);
  }
  const auto symbols = firstPage("Symbols");
  for (std::size_t i = 0; i < std::min<std::size_t>(symbols.size(), 3 * kRows); ++i) {
    m_textWarmup.emplace_back(symbols[i].title, Style::fontSizeCaption * scale);
    m_textWarmup.emplace_back(symbols[i].title, Style::fontSizeBody * scale);
  }
  const auto step = [this](const auto& self) -> void {
    RenderContext* context = PanelManager::instance().renderContext();
    if (m_textWarmup.empty() || context == nullptr) {
      m_textWarmup.clear();
      return;
    }
    // Up to ~4 ms of layouts per tick, so a frame in flight keeps its budget.
    ScaledRenderer renderer(*context, 1.0F);
    const auto start = std::chrono::steady_clock::now();
    while (!m_textWarmup.empty() && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(4)) {
      const auto [text, size] = std::move(m_textWarmup.back());
      m_textWarmup.pop_back();
      (void)renderer.measureText(text, size);
    }
    m_textWarmupTimer.start(std::chrono::milliseconds(16), [this, self] { self(self); });
  };
  step(step);
}

void LauncherPanel::clearDynamicProviders() {
  std::erase_if(m_providers, [](const std::unique_ptr<LauncherProvider>& provider) { return provider->isDynamic(); });
}

void LauncherPanel::clearProvidersWithIdPrefix(std::string_view prefix) {
  std::erase_if(m_providers, [&](const std::unique_ptr<LauncherProvider>& provider) {
    return provider->id().starts_with(prefix);
  });
}

void LauncherPanel::setScopedProvider(std::string_view providerId, std::string_view placeholder) {
  m_scopedProviderId = providerId;
  m_scopedPlaceholder = placeholder;
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
  }
}

void LauncherPanel::create() {
  m_launcherRowHeight = 0.0F;
  m_browserMode = usesBrowserLayout();
  const float scale = contentScale();

  auto input = ui::input({
      .out = &m_input,
      .placeholder = m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder,
      .fontSize = (m_browserMode ? Tokens::typeTitle3.size : Style::fontSizeBody) * scale,
      .controlHeight = (m_browserMode ? Tokens::componentLauncherSearch : Style::controlHeight) * scale,
      .horizontalPadding = m_browserMode ? 0.0F : Style::spaceMd * scale,
      .clearButtonEnabled = !m_browserMode,
      .frameVisible = !m_browserMode,
      .surfaceOpacity = panelCardOpacity(),
      .onChange =
          [this](const std::string& text) {
            leaveProviderView();
            onInputChanged(text);
            if (m_input == nullptr) {
              return;
            }
            const std::string preview = singleLinePreview(text);
            if (preview != text) {
              m_input->setValue(preview);
            }
          },
      .onSubmit = [this](const std::string& /*text*/) { activateSelected(); },
      .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
  });
  if (m_input != nullptr && m_input->inputArea() != nullptr) {
    m_input->inputArea()->setTabFocusKey("launcher.search");
  }

  auto categoryFilter = ui::segmented({
      .out = &m_categoryFilter,
      .scale = scale,
      .compact = true,
      .surfaceOpacity = panelCardOpacity(),
      .equalSegmentWidths = true,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Segmented& segmented) { segmented.setAlign(FlexAlign::Center); },
  });

  auto body = ui::column({
      .out = &m_body,
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });
  if (m_browserMode) {
    body->setGap(Style::spaceSm * scale);
  }

  auto resultsViewport = ui::column({
      .out = &m_resultsViewport,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });

  const LauncherListStyle initialStyle = launcherListStyleFrom(m_browserMode, m_config, scale, panelCardOpacity());
  m_listAdapter = std::make_unique<LauncherResultAdapter>(initialStyle, m_asyncTextures);
  m_gridAdapter = std::make_unique<LauncherAppGridAdapter>(initialStyle, m_asyncTextures);
  m_listAdapter->setResults(&m_results);
  m_gridAdapter->setResults(&m_results);
  const auto onActivate = [this](std::size_t index) { activateAt(index); };
  const auto onSecondaryActivate = [this](std::size_t index, float ax, float ay) {
    (void)openAppActionsMenu(index, ax, ay);
  };
  m_launcherSectionsAdapter = std::make_unique<LauncherSectionsAdapter>(
      initialStyle, m_asyncTextures, [this](std::size_t index) { selectResult(index, false); }, onActivate,
      onSecondaryActivate
  );
  m_launcherSectionsAdapter->setResults(&m_results);
  m_listAdapter->setOnActivate(onActivate);
  m_listAdapter->setOnSecondaryActivate(onSecondaryActivate);
  m_listAdapter->setOnReorder([this](std::size_t sourceIndex, std::size_t targetIndex) {
    if (sourceIndex >= m_results.size() || targetIndex >= m_results.size()) {
      return;
    }
    reorderPinnedApplication(m_results[sourceIndex].desktopEntryPath, m_results[targetIndex].desktopEntryPath);
  });
  m_gridAdapter->setOnActivate(onActivate);
  m_gridAdapter->setOnSecondaryActivate(onSecondaryActivate);
  m_gridAdapter->setOnReorder([this](std::size_t sourceIndex, std::size_t targetIndex) {
    if (sourceIndex >= m_results.size() || targetIndex >= m_results.size()) {
      return;
    }
    reorderPinnedApplication(m_results[sourceIndex].desktopEntryPath, m_results[targetIndex].desktopEntryPath);
  });

  resultsViewport->addChild(
      ui::virtualListView({
          .out = &m_launcherSections,
          .contentScale = scale,
          .itemGap = Style::spaceXs * scale,
          .overscanItems = kRowOverscan,
          .adapter = m_launcherSectionsAdapter.get(),
          .flexGrow = 1.0F,
          .visible = false,
          .participatesInLayout = false,
          .configure = [pill = m_browserMode](VirtualListView& list) {
            list.setFillWidth(true);
            list.scrollView().setReserveScrollbarGutter(!pill);
          },
      })
  );

  // Stickers mode: pack chips under the grid; a click filters to that pack (DESIGN.md media modes).
  resultsViewport->addChild(ui::row({
      .out = &m_packStrip,
      .align = FlexAlign::Center,
      .gap = Tokens::spaceP4 * scale,
      .paddingH = Tokens::spaceP4 * scale,
      .minHeight = Tokens::componentLauncherTabs * scale,
      .fillWidth = true,
      .visible = false,
      .participatesInLayout = false,
  }));

  // Emoji mode: one button per group under the grid; a click scrolls to that group (DESIGN.md media modes).
  resultsViewport->addChild(ui::row({
      .out = &m_emojiStrip,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .paddingH = Tokens::spaceP4 * scale,
      .minHeight = Tokens::componentLauncherTabs * scale,
      .fillWidth = true,
      .visible = false,
      .participatesInLayout = false,
  }));

  resultsViewport->addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .columns = 1,
          .cellHeight = launcherRowHeightEstimate(initialStyle),
          .squareCells = false,
          .columnGap = 0.0F,
          .rowGap = Style::spaceXs * scale,
          .overscanRows = kRowOverscan,
          .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
          .adapter = m_listAdapter.get(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::optional<std::size_t> idx) {
                if (idx.has_value() && *idx < m_results.size()) {
                  m_selectedIndex = *idx;
                }
              },
          .configure = [](VirtualGridView& grid) { grid.setFillWidth(true); },
      })
  );
  m_grid->setOnPageChanged([this](std::size_t page, std::size_t pages) { updatePageDots(page, pages); });

  resultsViewport->addChild(
      ui::row({
          .out = &m_pageDotsRow,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = Tokens::componentPageDotsGap * scale,
          .minHeight = Tokens::componentPageDotsRow * scale,
          .visible = false,
          .participatesInLayout = false,
      })
  );

  auto detailScroll = ui::scrollView({
      .out = &m_detailScroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = Style::spaceSm * scale,
      .viewportPaddingV = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale, opacity = panelCardOpacity()](ScrollView& scrollView) {
        scrollView.setCardStyle(scale, opacity);
      },
  });
  auto* detailContent = detailScroll->content();
  detailContent->setDirection(FlexDirection::Vertical);
  detailContent->setAlign(FlexAlign::Stretch);
  detailContent->setGap(Style::spaceSm * scale);
  detailContent->addChild(
      ui::label({
          .out = &m_detailSubtitle,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  detailContent->addChild(
      ui::label({
          .out = &m_detailBody,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::Label1),
          .maxLines = 0,
          .flexGrow = 1.0F,
      })
  );
  resultsViewport->addChild(std::move(detailScroll));

  resultsViewport->addChild(
      ui::label({
          .out = &m_emptyLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
          .visible = false,
          .participatesInLayout = false,
      })
  );
  body->addChild(std::move(resultsViewport));

  if (m_browserMode) {
    // The panel does not pad this content (padsContent): the field fills the 56 pill edge to edge.
    auto container = ui::column({
        .out = &m_container,
        .align = FlexAlign::Stretch,
        .fillWidth = true,
        .fillHeight = true,
    });

    // Rulebook 9.14: pill h 56, symbol 20 label_2 at inset 20, g 12, title-3 field.
    input->setFlexGrow(1.0F);
    // DESIGN.md "Show all" B: fill_1 capsule h 24, footnote label_1, x symbol; a click leaves the scope.
    const auto filesToken = [this, scale] {
      auto token = ui::row({
          .out = &m_filesToken,
          .align = FlexAlign::Center,
          .gap = Tokens::spaceP4 * scale,
          .paddingH = Tokens::spaceP8 * scale,
          .fill = colorSpecFromRole(ColorRole::Fill1),
          .radius = Tokens::sizeControlS * 0.5F * scale,
          .minHeight = Tokens::sizeControlS * scale,
          .visible = filesScope(),
          .participatesInLayout = filesScope(),
      });
      token->addChild(ui::label({
          .out = &m_filesTokenLabel,
          .text = i18n::tr("launcher.files.title"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Label1),
      }));
      token->addChild(ui::glyph({.glyph = "x", .glyphSize = Tokens::sizeSymbolInline * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      area->setOnClick([this](const InputArea::PointerData&) { setFilesScope(false); });
      token->addChild(std::move(area));
      return token;
    };
    container->addChild(ui::row(
        {.align = FlexAlign::Center,
         .gap = Tokens::spaceP12 * scale,
         .paddingH = Tokens::spaceP20 * scale,
         .minHeight = Tokens::componentLauncherSearch * scale,
         .fillWidth = true},
        ui::glyph(
            {.out = &m_pillSearchGlyph,
             .glyph = "search",
             .glyphSize = Tokens::sizeSymbolL * scale,
             .color = colorSpecFromRole(ColorRole::Label2)}
        ),
        filesToken(),
        std::move(input)
    ));
    container->addChild(ui::separator({
        .out = &m_pillSeparator,
        .color = colorSpecFromRole(ColorRole::Separator),
        .thickness = 1.0F,
        .visible = false,
        .participatesInLayout = false,
    }));
    container->addChild(ui::row({
        .out = &m_modeTabs,
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP4 * scale,
        .paddingH = Tokens::spaceP12 * scale,
        .minHeight = Tokens::componentLauncherTabs * scale,
        .fillWidth = true,
        .visible = false,
        .participatesInLayout = false,
    }));
    container->addChild(ui::separator({
        .out = &m_modeTabsLine,
        .color = colorSpecFromRole(ColorRole::Separator),
        .thickness = 1.0F,
        .visible = false,
        .participatesInLayout = false,
    }));
    m_modeTabItems.clear();
    container->addChild(ui::row({
        .out = &m_kindTabs,
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP4 * scale,
        .paddingH = Tokens::spaceP12 * scale,
        .minHeight = Tokens::componentLauncherTabs * scale,
        .fillWidth = true,
        .visible = false,
        .participatesInLayout = false,
    }));
    container->addChild(ui::separator({
        .out = &m_kindTabsLine,
        .color = colorSpecFromRole(ColorRole::Separator),
        .thickness = 1.0F,
        .visible = false,
        .participatesInLayout = false,
    }));
    m_kindTabItems.clear();
    body->setPadding(Tokens::componentLauncherPadding * scale);
    body->setVisible(false);
    body->setParticipatesInLayout(false);

    // Direction C: list | preview. The preview shows the current row: icon 64, title-3, kind, actions, details.
    body->setDirection(FlexDirection::Horizontal);
    body->setAlign(FlexAlign::Stretch);
    body->setGap(0.0F);
    body->addChild(ui::separator({
        .out = &m_previewDivider,
        .color = colorSpecFromRole(ColorRole::Separator),
        .thickness = 1.0F,
        .orientation = SeparatorOrientation::VerticalRule,
        .visible = false,
        .participatesInLayout = false,
    }));
    auto preview = ui::column({
        .out = &m_previewPane,
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP8 * scale,
        .paddingV = Tokens::spaceP24 * scale,
        .paddingH = Tokens::spaceP20 * scale,
        .flexGrow = 1.0F,
        .visible = false,
        .participatesInLayout = false,
    });
    const float previewIcon = Tokens::componentLauncherIcon * scale;
    preview->addChild(ui::image({.out = &m_previewImage, .width = previewIcon, .height = previewIcon, .visible = false}));
    preview->addChild(ui::glyph({
        .out = &m_previewGlyph,
        .glyphSize = previewIcon * 0.75F,
        .color = colorSpecFromRole(ColorRole::Label1),
        .visible = false,
    }));
    preview->addChild(ui::label({
        .out = &m_previewEmoji,
        .fontSize = Tokens::componentLauncherEmojiPreview * scale,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxLines = 1,
        .visible = false,
        .participatesInLayout = false,
    }));
    preview->addChild(ui::label({
        .out = &m_previewTitle,
        .fontSize = Tokens::typeTitle3.size * scale,
        .fontWeight = FontWeight::SemiBold,
        .color = colorSpecFromRole(ColorRole::Label1),
        .maxLines = 2,
        .ellipsize = TextEllipsize::End,
    }));
    preview->addChild(ui::label({
        .out = &m_previewKind,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Label2),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
    }));
    // A new preview image holds no picture yet: the next selection decodes again.
    m_previewShotId.clear();
    preview->addChild(ui::image({
        .out = &m_previewShot,
        .width = Tokens::componentLauncherPreviewMedia * scale * 1.75F,
        .height = Tokens::componentLauncherPreviewMedia * scale,
        .visible = false,
        .participatesInLayout = false,
        .configure = [scale](Image& image) {
          image.setFit(ImageFit::Contain);
          image.setRadius(Tokens::radiusR12 * scale);
        },
    }));
    // Clipboard text: monospace in a fill_1 box (.agent/mockups/launcher-media.html clipboard).
    preview->addChild(ui::column(
        {
            .out = &m_previewBodyBox,
            .align = FlexAlign::Start,
            .padding = Tokens::spaceP12 * scale,
            .fill = colorSpecFromRole(ColorRole::Fill1),
            .radius = Tokens::radiusR8 * scale,
            .minHeight = Tokens::componentLauncherPreviewMedia * 0.75F * scale,
            .fillWidth = true,
            .visible = false,
            .participatesInLayout = false,
        },
        ui::label({
            .out = &m_previewBody,
            .fontSize = Style::fontSizeCaption * scale,
            .fontFamily = std::string("monospace"),
            .color = colorSpecFromRole(ColorRole::Label1),
            .maxLines = 6,
            .ellipsize = TextEllipsize::End,
        })
    ));
    auto tones = ui::row({
        .out = &m_previewTones,
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP4 * scale,
        .visible = false,
        .participatesInLayout = false,
    });
    for (std::size_t i = 0; i < m_previewToneLabels.size(); ++i) {
      auto cell = ui::row({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .radius = Tokens::radiusR8 * scale,
          .minWidth = Tokens::sizeControlM * scale,
          .minHeight = Tokens::sizeControlM * scale,
      });
      cell->addChild(ui::label({.out = &m_previewToneLabels[i], .fontSize = Tokens::componentLauncherEmoji * scale, .maxLines = 1}));
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      area->setFrameSize(Tokens::sizeControlM * scale, Tokens::sizeControlM * scale);
      area->setOnClick([this, i](const InputArea::PointerData&) { activateEmojiVariant(i); });
      cell->addChild(std::move(area));
      tones->addChild(std::move(cell));
    }
    preview->addChild(std::move(tones));
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * scale});
    actions->setPadding(Tokens::spaceP12 * scale, 0.0F, 0.0F, 0.0F);
    actions->addChild(ui::button({
        .out = &m_previewOpen,
        .text = i18n::tr("launcher.pill.open-hint"),
        .controlHeight = Tokens::sizeControlS * scale,
        .variant = ButtonVariant::Primary,
        .onClick = [this] { activateSelected(); },
    }));
    actions->addChild(ui::button({
        .out = &m_previewAction,
        .controlHeight = Tokens::sizeControlS * scale,
        .variant = ButtonVariant::Default,
        .visible = false,
        .participatesInLayout = false,
        .onClick = [this] { activatePreviewAction(); },
    }));
    actions->addChild(ui::button({
        .out = &m_previewDelete,
        .text = i18n::tr("launcher.clipboard.delete"),
        .controlHeight = Tokens::sizeControlS * scale,
        .variant = ButtonVariant::Default,
        .visible = false,
        .participatesInLayout = false,
        .onClick = [this] { deleteSelectedClipboardEntry(); },
    }));
    preview->addChild(std::move(actions));
    auto meta = ui::column({.align = FlexAlign::Stretch, .fillWidth = true});
    meta->setPadding(Tokens::spaceP12 * scale, 0.0F, 0.0F, 0.0F);
    for (std::size_t i = 0; i < m_previewMetaRows.size(); ++i) {
      auto metaRow = ui::row({
          .out = &m_previewMetaRows[i],
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .minHeight = Tokens::sizeControlM * scale,
          .fillWidth = true,
          .visible = false,
          .participatesInLayout = false,
      });
      metaRow->addChild(ui::label({.out = &m_previewMetaKeys[i], .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label2)}));
      metaRow->addChild(ui::label({.out = &m_previewMetaValues[i], .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1, .ellipsize = TextEllipsize::End}));
      meta->addChild(std::move(metaRow));
    }
    preview->addChild(std::move(meta));
    body->addChild(std::move(preview));
    container->addChild(std::move(body));

    // Rulebook 9.14 footer: hairline, h 36, count leading, key hints trailing.
    const auto keyHint = [scale](std::string text, std::string key, Label** out = nullptr) {
      return ui::row(
          {.align = FlexAlign::Center, .gap = Tokens::spaceP4 * scale},
          ui::label({.out = out, .text = std::move(text), .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label2)}),
          ui::row(
              {.align = FlexAlign::Center,
               .paddingH = Tokens::spaceP4 * scale,
               .fill = colorSpecFromRole(ColorRole::Fill1),
               .radius = Tokens::radiusR4 * scale},
              ui::label({.text = std::move(key), .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::Label2)})
          )
      );
    };
    auto footer = ui::column({.out = &m_pillFooter, .align = FlexAlign::Stretch, .visible = false, .participatesInLayout = false});
    footer->addChild(ui::separator({.color = colorSpecFromRole(ColorRole::Separator), .thickness = 1.0F}));
    auto footerRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Tokens::spaceP12 * scale,
        .paddingH = Tokens::spaceP20 * scale,
        .minHeight = Tokens::componentLauncherFooter * scale,
        .fillWidth = true,
    });
    footerRow->addChild(ui::label({
        .out = &m_pillCount,
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::Label3),
        .flexGrow = 1.0F,
    }));
    footerRow->addChild(keyHint(i18n::tr("launcher.pill.open"), "↩", &m_pillOpenHint));
    footerRow->addChild(keyHint(i18n::tr("launcher.pill.actions-hint"), "⌘K"));
    auto kindHint = keyHint(i18n::tr("launcher.pill.kind-hint"), "⇥");
    m_pillKindHint = kindHint.get();
    footerRow->addChild(std::move(kindHint));
    auto backHint = keyHint(i18n::tr("launcher.pill.back-hint"), "⌫");
    m_pillBackHint = backHint.get();
    footerRow->addChild(std::move(backHint));
    footerRow->addChild(keyHint(i18n::tr("launcher.pill.close-hint"), "esc"));
    footer->addChild(std::move(footerRow));
    container->addChild(std::move(footer));
    setRoot(std::move(container));
    // The rulebook 9.14 sheet has no category tabs: the filter is not in the tree and dies with this scope.
    m_categoryFilter = nullptr;
  } else {
    auto container = ui::column({
        .out = &m_container,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
    });
    container->addChild(std::move(input));
    container->addChild(std::move(categoryFilter));
    container->addChild(std::move(body));
    setRoot(std::move(container));
  }

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  m_appIconColorizeConn = shellAppIconColorizationChanged().connect([this] { refreshLauncherAppIconColorization(); });

  syncLauncherListStyle();
}

void LauncherPanel::refreshLauncherAppIconColorization() {
  if (m_listAdapter == nullptr || m_gridAdapter == nullptr || m_grid == nullptr) {
    return;
  }
  const LauncherListStyle style = launcherListStyleFrom(m_browserMode, m_config, contentScale(), panelCardOpacity());
  m_listAdapter->setListStyle(style);
  m_gridAdapter->setListStyle(style);
  if (m_launcherSectionsAdapter != nullptr) {
    m_launcherSectionsAdapter->setListStyle(style);
  }
  m_grid->notifyDataChanged();
  if (m_launcherSections != nullptr) {
    m_launcherSections->notifyDataChanged();
  }
}

void LauncherPanel::updatePageDots(std::size_t page, std::size_t pages) {
  if (m_pageDotsRow == nullptr || m_grid == nullptr) {
    return;
  }
  const bool show = m_usingAppGrid && pages > 1;
  // Called from inside the grid's layout pass, where these changes do not mark the panel dirty.
  m_pageDotsChanged = true;
  m_pageDotsRow->setVisible(show);
  m_pageDotsRow->setParticipatesInLayout(show);
  const float s = contentScale();
  const float dot = Tokens::componentPageDotsDot * s;
  if (m_pageDots.size() != pages) {
    while (!m_pageDotsRow->children().empty()) {
      m_pageDotsRow->removeChild(m_pageDotsRow->children().back().get());
    }
    m_pageDots.clear();
    for (std::size_t i = 0; i < pages; ++i) {
      auto pill = std::make_unique<Flex>();
      pill->setMinHeight(dot);
      pill->setRadius(dot * 0.5F);
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setHitTestOutset({.left = dot * 0.5F, .top = dot, .right = dot * 0.5F, .bottom = dot});
      area->setOnClick([this, i](const InputArea::PointerData&) { m_grid->scrollToPage(i); });
      m_pageDots.push_back(pill.get());
      pill->addChild(std::move(area));
      m_pageDotsRow->addChild(std::move(pill));
    }
  }
  for (std::size_t i = 0; i < m_pageDots.size(); ++i) {
    const bool current = i == page;
    const float width = current ? Tokens::componentPageDotsCurrentWidth * s : dot;
    m_pageDots[i]->setMinWidth(width);
    m_pageDots[i]->setMaxWidth(width);
    m_pageDots[i]->setFill(colorSpecFromRole(current ? ColorRole::Label1 : ColorRole::Label3));
    auto* area = static_cast<InputArea*>(m_pageDots[i]->children().front().get());
    area->setFrameSize(width, dot);
  }
}

bool LauncherPanel::shouldUseAppGrid() const {
  if (m_browserMode) {
    return isAppsQuery(m_query) && !m_results.empty() && supportsResultGrid();
  }
  return m_config != nullptr && m_config->config().shell.launcher.appGrid && !m_results.empty() && supportsResultGrid();
}

bool LauncherPanel::supportsResultGrid() const {
  if (!m_launcherShowIcons || shouldUseDetailPresentation()) {
    return false;
  }
  if (showingProviderOverview()) {
    return true;
  }
  const auto providerSupportsGrid = [this](std::string_view id) {
    const auto provider = std::ranges::find(m_providers, id, [](const auto& candidate) { return candidate->id(); });
    return provider != m_providers.end() && (*provider)->supportsGrid();
  };
  if (!m_resultProviderId.empty()) {
    return providerSupportsGrid(m_resultProviderId);
  }
  return std::ranges::all_of(m_results, [&](const LauncherResult& result) {
    return providerSupportsGrid(result.providerId);
  });
}

void LauncherPanel::queueResultsTransition(ResultsTransition transition) {
  if (!m_browserMode || transition == ResultsTransition::None) {
    return;
  }
  m_pendingResultsTransition = transition;
}

void LauncherPanel::animateResultsTransition() {
  const ResultsTransition transition = std::exchange(m_pendingResultsTransition, ResultsTransition::None);
  if (transition == ResultsTransition::None || m_resultsViewport == nullptr) {
    return;
  }

  AnimationManager* animations = m_resultsViewport->animationManager();
  if (animations == nullptr) {
    m_resultsViewport->setOpacity(1.0F);
    m_resultsViewport->setScale(1.0F);
    m_resultsTransitionActive = false;
    return;
  }

  animations->cancelForOwner(m_resultsViewport);
  if (m_resultsTransitionActive) {
    m_resultsViewport->setPosition(m_resultsTransitionBaseX, m_resultsViewport->y());
  }
  constexpr float startOpacity = 0.78F;
  Flex* const viewport = m_resultsViewport;
  const float visualDirection = transition == ResultsTransition::NavigationForward ? 1.0F : -1.0F;
  const float direction = Style::rtl() ? -visualDirection : visualDirection;
  const float travel = std::min(viewport->width() * 0.12F, 72.0F * contentScale());
  m_resultsTransitionBaseX = viewport->x();
  m_resultsTransitionActive = true;
  viewport->setOpacity(startOpacity);
  viewport->setScale(1.0F);
  viewport->setPosition(m_resultsTransitionBaseX + direction * travel, viewport->y());
  animations->animateSpring(
      0.0F, 1.0F, Style::spring(Tokens::springPage),
      [this, viewport, direction, travel](float progress) {
        viewport->setOpacity(startOpacity + (1.0F - startOpacity) * progress);
        viewport->setPosition(m_resultsTransitionBaseX + direction * travel * (1.0F - progress), viewport->y());
      },
      [this, viewport] {
        viewport->setOpacity(1.0F);
        viewport->setScale(1.0F);
        viewport->setPosition(m_resultsTransitionBaseX, viewport->y());
        m_resultsTransitionActive = false;
      },
      viewport
  , 0.0F, Unit::Progress);
}

void LauncherPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_container == nullptr || m_input == nullptr) {
    return;
  }

  if (m_categoryFilterRebuildPending) {
    rebuildCategoryFilter();
  }
  updateLauncherOptionButtons();
  syncLauncherListStyle();
  syncLauncherViewLayout(&renderer);
  if (m_browserMode) {
    bindPreview(renderer);
  }

  m_container->setSize(width, height);
  m_container->layout(renderer);
  const float previousRowHeight = m_launcherRowHeight;
  updateLauncherGridMetrics(renderer);
  if (std::abs(previousRowHeight - m_launcherRowHeight) >= 0.5F || m_pageDotsChanged) {
    m_pageDotsChanged = false;
    m_container->layout(renderer);
  }
}

void LauncherPanel::doUpdate(Renderer& renderer) {
  if (m_browserMode) {
    syncLauncherAvatar(renderer);
  }
}

void LauncherPanel::syncLauncherAvatar(Renderer& renderer) {
  if (m_launcherAvatar == nullptr || m_launcherAvatarFallback == nullptr || m_config == nullptr) {
    return;
  }

  const std::string displayPath = shell::avatarDisplayPath(m_accounts, m_config->config());
  if (displayPath.empty()) {
    m_launcherAvatarPath.clear();
    m_launcherAvatar->clear(renderer);
    m_launcherAvatar->setVisible(false);
    m_launcherAvatar->setParticipatesInLayout(false);
    m_launcherAvatarFallback->setVisible(true);
    m_launcherAvatarFallback->setParticipatesInLayout(true);
    return;
  }

  if (displayPath != m_launcherAvatarPath) {
    m_launcherAvatarPath = displayPath;
    m_launcherAvatar->clear(renderer);
    m_launcherAvatar->setVisible(false);
    m_launcherAvatar->setParticipatesInLayout(false);
    m_launcherAvatarFallback->setVisible(true);
    m_launcherAvatarFallback->setParticipatesInLayout(true);
  }

  const int targetSize = static_cast<int>(std::round(kLauncherAvatarSize * contentScale()));
  const bool ready = m_asyncTextures != nullptr
      ? m_launcherAvatar->setSourceFileAsync(renderer, *m_asyncTextures, displayPath, targetSize, true)
      : m_launcherAvatar->setSourceFile(renderer, displayPath, targetSize, true);
  if (ready) {
    m_launcherAvatar->setVisible(true);
    m_launcherAvatar->setParticipatesInLayout(true);
    m_launcherAvatarFallback->setVisible(false);
    m_launcherAvatarFallback->setParticipatesInLayout(false);
  }
}

std::string LauncherPanel::handoffContext() const {
  return m_modeProviderId.empty() ? m_query : std::string(kModeContextPrefix) + m_modeProviderId;
}

void LauncherPanel::onOpen(std::string_view context) {
  for (auto& provider : m_providers) {
    applyProviderConfig(*provider);
    provider->onLauncherOpened();
  }

  // Pick up apps installed since the last scan (notably Nix profile swaps that
  // inotify cannot observe). Cheap stat-only check; only rescans on real change.
  refreshDesktopEntriesIfSourcesChanged();

  m_activeCategoryType = All;
  m_activeCategorySlotIndex = 0;
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_categoryFilterSlots.clear();
  m_hasRecentlyUsed = false;
  m_hasPinnedApplications = false;
  m_categoryFilterRebuildPending = true;
  m_applyDefaultViewPending = m_browserMode;
  if (filesScope()) {
    setFilesScope(false);
  }
  m_view.set(launcher_view::Default{});

  // "mode:<provider>" or a mode provider's prefix (Mod+Period sends "/emo") opens that mode with an empty field.
  std::string initialValue(context);
  m_modeProviderId.clear();
  if (m_browserMode) {
    std::string_view requested;
    const bool modeContext = initialValue.starts_with(kModeContextPrefix);
    if (modeContext) {
      requested = std::string_view(initialValue).substr(kModeContextPrefix.size());
    } else {
      for (const std::string_view id : kModeProviders) {
        const auto provider = std::ranges::find(m_providers, id, [](const auto& p) { return p->id(); });
        if (provider != m_providers.end() && !(*provider)->prefix().empty()
            && StringUtils::trim(initialValue) == (*provider)->prefix()) {
          requested = id;
        }
      }
    }
    const auto known = std::ranges::find(kModeProviders, requested);
    if (!requested.empty() && known != kModeProviders.end()
        && std::ranges::any_of(m_providers, [&](const auto& p) { return p->id() == requested; })) {
      m_modeProviderId = std::string(requested);
    }
    if (modeContext || !m_modeProviderId.empty()) {
      initialValue.clear(); // an unknown mode opens plain search, never a search for "mode:..."
    }
    syncModeTabs();
  }
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
    m_input->setValue(singleLinePreview(initialValue));
  }
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->scrollView().setScrollOffset(0.0F);
  }
  onInputChanged(initialValue);
}

void LauncherPanel::onClose() {
  stopPreviewGif();
  m_iconPrefetchTimer.start(std::chrono::seconds(1), [this] { prefetchAppIcons(); });
  m_sentLiquid = -1.0F;
  if (m_actionsMenu != nullptr && m_actionsMenu->isOpen()) {
    m_actionsMenu->close();
  }
  if (m_asyncTextures != nullptr) {
    DeferredCall::callLater([asyncTextures = m_asyncTextures] { asyncTextures->trimUnused(0); });
  }

  for (auto& provider : m_providers) {
    provider->reset();
  }

  m_query.clear();
  m_results.clear();
  m_allResults.clear();
  m_scopedProviderId.clear();
  m_scopedPlaceholder.clear();
  m_view.set(launcher_view::Default{});
  m_kindIndex = 0;
  m_packIndex = 0;
  if (auto* stickers = stickersProvider()) {
    stickers->setPack({});
  }
  if (auto* files = filesProvider()) {
    files->setScopeKind({});
  }
  m_activeCategoryType = All;
  m_activeCategorySlotIndex = 0;
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_categoryFilterSlots.clear();
  m_hasRecentlyUsed = false;
  m_hasPinnedApplications = false;
  m_categoryFilterRebuildPending = false;
  m_applyDefaultViewPending = false;
  m_syncingLauncherNavigation = false;
  m_pendingResultsTransition = ResultsTransition::None;
  m_resultsTransitionActive = false;
  m_resultsTransitionBaseX = 0.0F;
  m_selectedIndex = 0;
  m_usingAppGrid = false;
  m_launcherRowHeight = 0.0F;

  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->setAdapter(nullptr);
  }
  m_listAdapter.reset();
  m_gridAdapter.reset();
  m_launcherSectionsAdapter.reset();

  // The scene tree (and all nodes) is destroyed by PanelManager after onClose().
  m_container = nullptr;
  m_input = nullptr;
  m_pillSearchGlyph = nullptr;
  m_categoryFilter = nullptr;
  m_body = nullptr;
  m_resultsViewport = nullptr;
  m_launcherToolbar = nullptr;
  m_launcherNavigation = nullptr;
  m_launcherAvatar = nullptr;
  m_launcherAvatarFallback = nullptr;
  m_launcherSettingsButton = nullptr;
  m_launcherUsageButton = nullptr;
  m_launcherGridButton = nullptr;
  m_launcherPowerButton = nullptr;
  m_grid = nullptr;
  m_pageDotsRow = nullptr;
  m_pageDots.clear();
  m_launcherSections = nullptr;
  m_detailScroll = nullptr;
  m_detailSubtitle = nullptr;
  m_detailBody = nullptr;
  m_emptyLabel = nullptr;
  m_launcherAvatarPath.clear();
  clearReleasedRoot();
}

void LauncherPanel::onIconThemeChanged() { reapplyCurrentQuery(); }

void LauncherPanel::clearUsage() {
  m_usageTracker.clear();
  if (m_input != nullptr) {
    reapplyCurrentQuery();
  }
}

void LauncherPanel::syncUsageOrderingState() {
  if (m_input != nullptr) {
    reapplyCurrentQuery();
  }
}

void LauncherPanel::reapplyCurrentQuery() {
  std::string selectedProvider;
  std::string selectedId;
  if (m_selectedIndex < m_results.size()) {
    selectedProvider = m_results[m_selectedIndex].providerId;
    selectedId = m_results[m_selectedIndex].id;
  }

  onInputChanged(m_query);

  if (!selectedId.empty()) {
    for (std::size_t i = 0; i < m_results.size(); ++i) {
      if (m_results[i].providerId == selectedProvider && m_results[i].id == selectedId) {
        m_selectedIndex = i;
        break;
      }
    }
  }
  refreshResults();
}

void LauncherPanel::setQuery(const std::string& query) {
  if (m_input == nullptr) {
    return;
  }
  leaveProviderView();
  m_input->setValue(singleLinePreview(query));
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->scrollView().setScrollOffset(0.0F);
  }
  onInputChanged(query);
}

void LauncherPanel::onProviderResultsChanged(const LauncherProvider& provider) {
  // Only re-gather while the panel is open and built; after onClose the scene
  // nodes are gone and a refresh would touch null grid/label pointers.
  if (m_input == nullptr || !m_searchBroker.acceptsAnswer(provider)) {
    return;
  }
  reapplyCurrentQuery();
}

InputArea* LauncherPanel::initialFocusArea() const { return m_input != nullptr ? m_input->inputArea() : nullptr; }

bool LauncherPanel::ignorePointerEvent() const {
  return m_config != nullptr && m_config->config().shell.launcher.ignoreMouse;
}

void LauncherPanel::onInputChanged(const std::string& text) {
  const auto desktopVersion = desktopEntriesVersion();
  if (desktopVersion != m_desktopEntriesVersion) {
    m_iconResolver.invalidateMissingCache();
    m_desktopEntriesVersion = desktopVersion;
  }

  m_query = text;
  m_resultProviderId.clear();
  if (showingProviderOverview()) {
    m_view.set(launcher_view::Default{});
  }
  if (m_browserMode && !text.empty() && m_activeCategoryType != All) {
    m_activeCategoryType = All;
    const auto allSlot = std::ranges::find(m_categoryFilterSlots, All, &CategoryFilterSlot::type);
    m_activeCategorySlotIndex =
        allSlot == m_categoryFilterSlots.end() ? 0 : static_cast<std::size_t>(allSlot - m_categoryFilterSlots.begin());
  }
  static const ShellConfig::LauncherConfig kDefaultLauncher{};
  const auto& launcher = m_config != nullptr ? m_config->config().shell.launcher : kDefaultLauncher;
  const bool trackUsage = m_config == nullptr || launcher.trackUsage;
  m_searchBroker.setOverviewPrefix(m_config != nullptr ? launcher.providerPrefix : "/");
  auto outcome = m_searchBroker.search({
      .text = m_browserMode && isAppsQuery(text) ? std::string() : text,
      .scopedProviderId = m_scopedProviderId,
      .providerViewId = filesScope() ? std::string("Files")
          : !m_modeProviderId.empty()
          ? m_modeProviderId
          : (m_scopedProviderId.empty() ? std::string(m_view.providerViewId()) : std::string()),
      .applicationsOnly = m_browserMode && m_modeProviderId.empty() && (text.empty() || isAppsQuery(text)),
      .trackUsage = trackUsage,
      .sortByUsage = trackUsage && m_config != nullptr && launcher.sortByUsage,
  });
  if (outcome.providerViewMissing) {
    leaveProviderView();
  }
  m_allResults = std::move(outcome.results);
  m_resultProviderId = std::move(outcome.resultProviderId);
  std::vector<LauncherCategory> newCategories = std::move(outcome.categories);
  bool hasRecentlyUsed = outcome.hasRecentlyUsed;
  const bool anyProviderLoading = outcome.loading;
  if (outcome.providerOverview) {
    m_allResults = providerOverviewResults(text);
    m_view.set(launcher_view::ProviderOverview{});
  }

  if (m_browserMode) {
    const auto applications =
        std::ranges::find(m_providers, kApplicationsProviderId, [](const auto& provider) { return provider->id(); });
    if (applications != m_providers.end()) {
      newCategories = (*applications)->categories();
      hasRecentlyUsed = trackUsage && m_usageTracker.getRecentlyUsedCount((*applications)->id()) > 0;
    }
  }

  const int iconTargetSize = static_cast<int>(
      std::round(launcherIconSize(launcherListStyleFrom(m_browserMode, m_config, contentScale(), panelCardOpacity())))
  );
  for (auto& result : m_allResults) {
    if (result.iconPath.empty() && !result.iconName.empty()) {
      const std::string& resolved = m_iconResolver.resolve(result.iconName, iconTargetSize);
      if (!resolved.empty()) {
        result.iconPath = resolved;
      } else if (result.iconName != "application-x-executable") {
        const std::string& fallback = m_iconResolver.resolve("application-x-executable", iconTargetSize);
        if (!fallback.empty()) {
          result.iconPath = fallback;
        }
      }
      result.iconName.clear();
    }
  }

  updatePinnedApplicationState();
  const bool hasPinnedApplications = m_config != nullptr
      && !shell::dock::pinned_apps::resolveEntries(m_config->config().shell.launcher.pinned).empty();

  bool categoriesChanged =
      (m_browserMode && m_categoryFilterSlots.empty()) || newCategories.size() != m_currentCategories.size();
  if (!categoriesChanged) {
    for (std::size_t i = 0; i < newCategories.size(); ++i) {
      if (newCategories[i].label != m_currentCategories[i].label) {
        categoriesChanged = true;
        break;
      }
    }
  }

  if (hasRecentlyUsed != m_hasRecentlyUsed) {
    m_hasRecentlyUsed = hasRecentlyUsed;
    categoriesChanged = true;
  }
  if (m_browserMode && hasPinnedApplications != m_hasPinnedApplications) {
    m_hasPinnedApplications = hasPinnedApplications;
    categoriesChanged = true;
  }

  if (categoriesChanged) {
    updateCategoryFilterModel(newCategories);
  }

  if (m_applyDefaultViewPending) {
    applyDefaultView();
  }

  m_anyProviderLoading = anyProviderLoading;

  applyActiveCategory();
}

void LauncherPanel::refreshResults() {
  uiAssertNotRendering("LauncherPanel::refreshResults");
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }

  if (m_browserMode && m_modeProviderId.empty() && m_results.empty() && !m_query.empty()) {
    // Rulebook 9.14 / docs/search.md §1: never an empty sheet, also while a slow provider answers; offer the web
    // search row.
    const auto web = std::ranges::find(m_providers, "Web", [](const auto& provider) { return provider->id(); });
    if (web != m_providers.end()) {
      for (auto& result : (*web)->query(m_query)) {
        result.providerId = (*web)->id();
        m_results.push_back(std::move(result));
      }
    }
  }
  syncLauncherViewLayout(nullptr);
  rebuildLauncherSections();
  m_grid->notifyDataChanged();
  if (m_results.empty()) {
    m_grid->setSelectedIndex(std::nullopt);
    m_grid->scrollView().setScrollOffset(0.0F);
    if (m_launcherSections != nullptr) {
      m_launcherSections->scrollView().setScrollOffset(0.0F);
    }
  } else {
    selectResult(std::min(m_selectedIndex, m_results.size() - 1), false);
  }
  bindDetailResult();
  applyEmptyState();
  animateResultsTransition();
}

void LauncherPanel::selectResult(std::size_t index, bool scrollToSelection) {
  if (index >= m_results.size()) {
    return;
  }
  m_selectedIndex = index;
  if (shouldUseLauncherSections() && m_launcherSectionsAdapter != nullptr && m_launcherSections != nullptr) {
    m_grid->setSelectedIndex(std::nullopt);
    m_launcherSectionsAdapter->setSelectedResultIndex(index);
    m_launcherSections->notifyDataChanged();
    if (scrollToSelection) {
      if (const auto row = m_launcherSectionsAdapter->rowForResult(index)) {
        m_launcherSections->scrollToIndex(*row);
      }
    }
    return;
  }
  if (m_grid != nullptr) {
    m_grid->setSelectedIndex(index);
  }
}

void LauncherPanel::applyEmptyState() {
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }
  const bool empty = m_results.empty();
  const bool detail = !empty && shouldUseDetailPresentation();
  const bool sections = !empty && !detail && shouldUseLauncherSections();
  m_grid->setVisible(!empty && !detail && !sections);
  m_grid->setParticipatesInLayout(!empty && !detail && !sections);
  if (m_launcherSections != nullptr) {
    m_launcherSections->setVisible(sections);
    m_launcherSections->setParticipatesInLayout(sections);
  }
  if (m_detailScroll != nullptr) {
    m_detailScroll->setVisible(detail);
    m_detailScroll->setParticipatesInLayout(detail);
  }
  m_emptyLabel->setVisible(empty);
  m_emptyLabel->setParticipatesInLayout(empty);
  syncEmojiStrip();
  syncPackStrip();
  if (m_browserMode && m_body != nullptr && m_pillSeparator != nullptr && m_pillFooter != nullptr) {
    const bool expanded = pillExpanded();
    m_body->setVisible(expanded);
    m_body->setParticipatesInLayout(expanded);
    m_pillSeparator->setVisible(expanded);
    m_pillSeparator->setParticipatesInLayout(expanded);
    const bool footer = expanded && !shouldUseAppGrid();
    m_pillFooter->setVisible(footer);
    m_pillFooter->setParticipatesInLayout(footer);
    m_pillCount->setText(
        filesScope() ? i18n::trp("launcher.files.count", static_cast<long>(m_results.size()))
                     : i18n::tr("launcher.pill.result-count", "count",
                                std::ranges::count_if(m_results, [](const LauncherResult& r) { return r.id != FilesProvider::kShowAllId; }))
    );
    m_pillBackHint->setVisible(filesScope());
    m_pillBackHint->setParticipatesInLayout(filesScope());
    m_pillKindHint->setVisible(filesScope());
    m_pillKindHint->setParticipatesInLayout(filesScope());
    syncKindTabs();
    if (m_filesToken != nullptr) {
      if (auto* area = dynamic_cast<InputArea*>(m_filesToken->children().back().get())) {
        area->setFrameSize(m_filesToken->width(), m_filesToken->height());
      }
    }
    const bool pasteMode = !m_modeProviderId.empty();
    m_pillOpenHint->setText(i18n::tr(pasteMode ? "launcher.pill.paste" : "launcher.pill.open"));
  }
  if (empty) {
    if (m_anyProviderLoading && !m_query.empty()) {
      m_emptyLabel->setText(i18n::tr("launcher.empty.loading"));
    } else {
      m_emptyLabel->setText(
          m_query.empty() ? i18n::tr("launcher.empty.type-to-search") : i18n::tr("launcher.empty.no-results")
      );
    }
  }
}
