#include "shell/launcher/launcher_panel.h"

#include "shell/launcher/launcher_adapters.h"
#include "shell/launcher/launcher_panel_detail.h"


bool LauncherPanel::shouldUseLauncherSections() const {
  return m_browserMode && pillExpanded() && !shouldUseAppGrid();
}

namespace {

  std::vector<LauncherSectionsAdapter::Section> providerSections(
      const std::vector<LauncherResult>& results, const std::vector<std::unique_ptr<LauncherProvider>>& providers
  ) {
  // docs/search.md §1 order: Applications, Commands, Settings, Windows, Files, then the others as they come.
  const auto rank = [](std::string_view id) -> int {
    if (id == kApplicationsProviderId) {
      return 0;
    }
    if (id == "Session" || id == "Panels") {
      return 1;
    }
    if (id == "Settings") {
      return 2;
    }
    if (id == "Windows") {
      return 3;
    }
    if (id == "Files") {
      return 4;
    }
    return 5;
  };
  std::vector<std::pair<int, LauncherSectionsAdapter::Section>> sections;
  const auto displayName = [&](std::string_view id) {
    const auto provider = std::ranges::find(providers, id, [](const auto& candidate) { return candidate->id(); });
    return provider == providers.end() ? std::string(id) : (*provider)->displayName();
  };
  for (std::size_t i = 0; i < results.size(); ++i) {
    const std::string& id = results[i].providerId;
    const int r = rank(id);
    const std::string label = r == 1 ? i18n::tr("launcher.browser.sections.commands") : displayName(id);
    auto it = std::ranges::find_if(sections, [&](const auto& entry) { return entry.second.label == label; });
    if (it == sections.end()) {
      sections.push_back({r, {.label = label, .glyphName = {}, .resultIndices = {}}});
      it = std::prev(sections.end());
    }
    it->second.resultIndices.push_back(i);
  }
  std::ranges::stable_sort(sections, {}, &std::pair<int, LauncherSectionsAdapter::Section>::first);
  std::vector<LauncherSectionsAdapter::Section> ordered;
  ordered.reserve(sections.size());
  for (auto& entry : sections) {
    ordered.push_back(std::move(entry.second));
  }
  return ordered;
}

  // One section per result category, in the order the provider returns them (clipboard: Pinned, Today, ...).
  std::vector<LauncherSectionsAdapter::Section> categorySections(const std::vector<LauncherResult>& results) {
    std::vector<LauncherSectionsAdapter::Section> sections;
    for (std::size_t i = 0; i < results.size(); ++i) {
      auto it = std::ranges::find(sections, results[i].category, &LauncherSectionsAdapter::Section::label);
      if (it == sections.end()) {
        sections.push_back({.label = results[i].category, .glyphName = {}, .resultIndices = {}});
        it = std::prev(sections.end());
      }
      it->resultIndices.push_back(i);
    }
    return sections;
  }

  // Emoji mode: "Recent" first (no query), then the Unicode groups in order; a query shows one "Results" section.
  std::vector<LauncherSectionsAdapter::Section> emojiSections(
      const std::vector<LauncherResult>& results, const std::vector<std::unique_ptr<LauncherProvider>>& providers,
      bool searching
  ) {
    std::vector<LauncherSectionsAdapter::Section> sections;
    if (searching) {
      LauncherSectionsAdapter::Section all{.label = i18n::tr("launcher.pill.results"), .glyphName = {}, .resultIndices = {}};
      for (std::size_t i = 0; i < results.size(); ++i) {
        all.resultIndices.push_back(i);
      }
      sections.push_back(std::move(all));
      return sections;
    }
    std::vector<std::size_t> recent;
    for (std::size_t i = 0; i < results.size(); ++i) {
      if (results[i].recentlyUsedIndex > 0) {
        recent.push_back(i);
      }
    }
    std::ranges::sort(recent, std::ranges::greater{}, [&](std::size_t i) { return results[i].recentlyUsedIndex; });
    if (recent.size() > kEmojiColumns * 2) {
      recent.resize(kEmojiColumns * 2);
    }
    sections.push_back({.label = i18n::tr("launcher.pill.recent"), .glyphName = {}, .resultIndices = std::move(recent)});
    const auto emoji = std::ranges::find(providers, "Emoji", [](const auto& p) { return p->id(); });
    if (emoji == providers.end()) {
      return sections;
    }
    for (const LauncherCategory& category : (*emoji)->categories()) {
      LauncherSectionsAdapter::Section section{.label = category.label, .glyphName = {}, .resultIndices = {}};
      for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].category == category.label) {
          section.resultIndices.push_back(i);
        }
      }
      sections.push_back(std::move(section));
    }
    return sections;
  }

} // namespace

void LauncherPanel::rebuildLauncherSections() {
  if (m_launcherSectionsAdapter == nullptr || m_launcherSections == nullptr) {
    return;
  }
  m_launcherSectionsAdapter->setPillStyle(m_browserMode);
  const bool emojiMode = m_modeProviderId == "Emoji";
  const auto thumbs = [this](std::string_view presentation) {
    return std::ranges::any_of(m_results, [&](const LauncherResult& r) { return r.presentation == presentation; });
  };
  m_launcherSectionsAdapter->setCellGrid(
      emojiMode                                          ? CellGrid::Emoji
          : m_modeProviderId == "Gifs" && thumbs("gif")         ? CellGrid::Gif
          : m_modeProviderId == "Stickers" && thumbs("sticker") ? CellGrid::Sticker
                                                                : CellGrid::None
  );
  if (m_browserMode && shouldUseLauncherSections() && emojiMode) {
    (void)m_launcherSectionsAdapter->setGridView(false);
    m_launcherSectionsAdapter->rebuildSections(emojiSections(m_results, m_providers, !m_query.empty()));
    m_launcherSectionsAdapter->setSelectedResultIndex(m_selectedIndex);
  } else if (m_browserMode && shouldUseLauncherSections() && !m_modeProviderId.empty()) {
    (void)m_launcherSectionsAdapter->setGridView(false);
    m_launcherSectionsAdapter->rebuildSections(categorySections(m_results));
    m_launcherSectionsAdapter->setSelectedResultIndex(m_selectedIndex);
  } else if (m_browserMode && shouldUseLauncherSections()) {
    (void)m_launcherSectionsAdapter->setGridView(false);
    m_launcherSectionsAdapter->rebuildSections(providerSections(m_results, m_providers));
    m_launcherSectionsAdapter->setSelectedResultIndex(m_selectedIndex);
  } else if (shouldUseLauncherSections()) {
    (void)m_launcherSectionsAdapter->setGridView(
        m_config != nullptr && m_config->config().shell.launcher.appGrid && m_launcherShowIcons
    );
    m_launcherSectionsAdapter->rebuild(m_currentCategories, i18n::tr("launcher.browser.sections.other-apps"));
    m_launcherSectionsAdapter->setSelectedResultIndex(m_selectedIndex);
  } else {
    m_launcherSectionsAdapter->rebuild({}, "");
    m_launcherSectionsAdapter->setSelectedResultIndex(std::nullopt);
  }
  m_launcherSections->notifyDataChanged();
}

void LauncherPanel::syncLauncherViewLayout(Renderer* renderer) {
  if (m_grid == nullptr || m_listAdapter == nullptr || m_gridAdapter == nullptr) {
    return;
  }

  const bool useGrid = shouldUseAppGrid();
  const float scale = contentScale();
  const LauncherListStyle style = launcherListStyleFrom(m_browserMode, m_config, scale, panelCardOpacity());
  m_grid->setScale(scale);
  m_listAdapter->setListStyle(style);
  m_gridAdapter->setListStyle(style);
  if (m_launcherSectionsAdapter != nullptr) {
    m_launcherSectionsAdapter->setListStyle(style);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->setContentScale(scale);
  }
  const bool reorderEnabled = m_query.empty() && m_scopedProviderId.empty() && m_activeCategoryType == Pinned;
  const bool listReorderEnabledChanged = m_listAdapter->setReorderEnabled(reorderEnabled);
  const bool gridReorderEnabledChanged = m_gridAdapter->setReorderEnabled(reorderEnabled);
  const bool reorderEnabledChanged = listReorderEnabledChanged || gridReorderEnabledChanged;
  if (renderer != nullptr) {
    m_listAdapter->setRenderer(renderer);
    m_gridAdapter->setRenderer(renderer);
  }

  const bool modeChanged = useGrid != m_usingAppGrid;
  m_usingAppGrid = useGrid;
  if (modeChanged) {
    m_launcherRowHeight = 0.0F;
  }

  if (m_browserMode && m_body != nullptr && m_resultsViewport != nullptr && m_previewPane != nullptr) {
    // Rulebook 9.14: the grid sits at p 16; direction C puts the list in a 360 pane, lens at inset 8.
    const bool split = !useGrid && pillExpanded();
    m_body->setPadding(useGrid ? Tokens::componentLauncherPadding * scale : 0.0F);
    m_resultsViewport->setPadding(split ? Tokens::spaceP8 * scale : 0.0F);
    m_resultsViewport->setFlexGrow(split ? 0.0F : 1.0F);
    m_resultsViewport->setMinWidth(split ? Tokens::componentLauncherListPane * scale : 0.0F);
    m_resultsViewport->setMaxWidth(split ? Tokens::componentLauncherListPane * scale : 0.0F);
    m_previewPane->setVisible(split);
    m_previewPane->setParticipatesInLayout(split);
    m_previewDivider->setVisible(split);
    m_previewDivider->setParticipatesInLayout(split);
    if (m_launcherSections != nullptr) {
      m_launcherSections->setItemGap(0.0F);
    }
  }
  if (useGrid) {
    m_grid->setColumns(kAppGridColumns);
    m_grid->setSquareCells(false);
    m_grid->setColumnGap(Style::spaceSm * scale);
    m_grid->setRowGap(Style::spaceSm * scale);
    m_grid->setCellHeight(Tokens::componentLauncherCellHeight * scale);
    m_grid->setPaged(static_cast<std::size_t>(Tokens::componentLauncherRows), Tokens::spaceP32 * scale);
    if (modeChanged) {
      m_grid->setAdapter(m_gridAdapter.get());
    }
  } else {
    m_grid->setPaged(0, 0.0F);
    if (m_pageDotsRow != nullptr) {
      m_pageDotsRow->setVisible(false);
      m_pageDotsRow->setParticipatesInLayout(false);
    }
    m_grid->setColumns(1);
    m_grid->setColumnGap(0.0F);
    m_grid->setRowGap(Style::spaceXs * scale);
    const float listCellHeight = m_launcherRowHeight > 0.0F ? m_launcherRowHeight
        : renderer != nullptr                               ? launcherRowHeight(*renderer, style)
                                                            : launcherRowHeightEstimate(style);
    m_grid->setCellHeight(listCellHeight);
    if (modeChanged) {
      m_grid->setAdapter(m_listAdapter.get());
    }
  }

  if (modeChanged || reorderEnabledChanged) {
    m_grid->notifyDataChanged();
  }
}

void LauncherPanel::syncLauncherListStyle() {
  const bool showIcons = m_config == nullptr || m_config->config().shell.launcher.showIcons;
  const bool showAppOriginIndicator = m_config == nullptr || m_config->config().shell.launcher.showAppOriginIndicator;
  const bool compact = m_config != nullptr && m_config->config().shell.launcher.compact;
  const bool appGrid = m_config != nullptr && m_config->config().shell.launcher.appGrid;
  const bool listItemBackground = m_config != nullptr && m_config->config().shell.panel.listItemBackground;
  if (showIcons == m_launcherShowIcons
      && showAppOriginIndicator == m_launcherShowAppOriginIndicator
      && compact == m_launcherCompact
      && appGrid == m_launcherAppGrid
      && listItemBackground == m_launcherListItemBackground
      && m_listAdapter != nullptr) {
    return;
  }
  m_launcherShowIcons = showIcons;
  m_launcherShowAppOriginIndicator = showAppOriginIndicator;
  m_launcherCompact = compact;
  m_launcherAppGrid = appGrid;
  m_launcherListItemBackground = listItemBackground;
  m_launcherRowHeight = 0.0F;
  syncLauncherViewLayout(nullptr);
  rebuildLauncherSections();
  if (m_grid != nullptr) {
    m_grid->notifyDataChanged();
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->notifyDataChanged();
  }
}

void LauncherPanel::updateLauncherGridMetrics(Renderer& renderer) {
  if (m_grid == nullptr) {
    return;
  }

  const LauncherListStyle style = launcherListStyleFrom(m_browserMode, m_config, contentScale(), panelCardOpacity());
  float cellHeight = launcherRowHeight(renderer, style);
  if (m_usingAppGrid) {
    cellHeight = Tokens::componentLauncherCellHeight * contentScale();
  }
  if (std::abs(cellHeight - m_launcherRowHeight) < 0.5F) {
    return;
  }

  m_launcherRowHeight = cellHeight;
  m_grid->setCellHeight(cellHeight);
  if (m_launcherSectionsAdapter != nullptr) {
    m_launcherSectionsAdapter->setApplicationRowHeight(cellHeight);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->notifyDataChanged();
  }
}

void LauncherPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
  if (m_categoryFilter != nullptr) {
    m_categoryFilter->setSurfaceOpacity(opacity);
  }
  if (m_launcherNavigation != nullptr) {
    m_launcherNavigation->setSurfaceOpacity(opacity);
  }
  if (m_launcherToolbar != nullptr) {
    m_launcherToolbar->setCardStyle(contentScale(), opacity);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->notifyDataChanged();
  }
  for (Button* button : {
           m_launcherUsageButton,
           m_launcherGridButton,
           m_launcherSettingsButton,
           m_launcherPowerButton,
       }) {
    if (button != nullptr) {
      button->setSurfaceOpacity(opacity);
    }
  }
  if (m_detailScroll != nullptr) {
    m_detailScroll->setCardStyle(contentScale(), opacity);
  }
}

bool LauncherPanel::isContextActive(std::string_view context) const {
  if (m_modeProviderId.empty()) {
    return false;
  }
  if (context.starts_with(kModeContextPrefix)) {
    return context.substr(kModeContextPrefix.size()) == m_modeProviderId;
  }
  const auto provider = std::ranges::find(m_providers, std::string_view(m_modeProviderId), [](const auto& p) { return p->id(); });
  return provider != m_providers.end() && StringUtils::trim(context) == (*provider)->prefix();
}

bool LauncherPanel::pillExpanded() const { return !m_query.empty() || !m_modeProviderId.empty() || m_filesScope; }

void LauncherPanel::showProviderOverview() {
  const std::string prefix = m_config != nullptr ? m_config->config().shell.launcher.providerPrefix : "/";
  if (!prefix.empty()) {
    queueResultsTransition(ResultsTransition::NavigationForward);
    setQuery(prefix);
  }
}

void LauncherPanel::showSessionActions() {
  if (m_launcherProviderViewId == kSessionProviderId && m_sessionReturnView.has_value()) {
    BrowserViewState previous = std::move(*m_sessionReturnView);
    m_sessionReturnView.reset();
    m_activeCategoryType = previous.categoryType;
    m_activeCategorySlotIndex = previous.categorySlotIndex;
    m_activeCategory = std::move(previous.category);
    m_launcherProviderViewId = std::move(previous.providerViewId);
    queueResultsTransition(ResultsTransition::NavigationBackward);
    if (m_input != nullptr) {
      m_input->setValue(singleLinePreview(previous.query));
    }
    if (m_grid != nullptr) {
      m_grid->scrollView().setScrollOffset(0.0F);
    }
    if (m_launcherSections != nullptr) {
      m_launcherSections->scrollView().setScrollOffset(0.0F);
    }
    onInputChanged(previous.query);
    return;
  }

  const auto provider =
      std::ranges::find(m_providers, kSessionProviderId, [](const auto& candidate) { return candidate->id(); });
  if (provider == m_providers.end() || (*provider)->prefix().empty()) {
    return;
  }
  m_sessionReturnView = BrowserViewState{
      .categoryType = m_activeCategoryType,
      .categorySlotIndex = m_activeCategorySlotIndex,
      .category = m_activeCategory,
      .query = m_query,
      .providerViewId = m_launcherProviderViewId,
  };
  queueResultsTransition(ResultsTransition::NavigationForward);
  m_launcherProviderViewId = std::string((*provider)->id());
  if (m_input != nullptr) {
    m_input->setValue("");
  }
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->scrollView().setScrollOffset(0.0F);
  }
  onInputChanged("");
}

void LauncherPanel::applyPinnedApplicationOrder() {
  if (m_config == nullptr || m_config->config().shell.launcher.pinned.empty()) {
    return;
  }

  const auto pinnedEntries = shell::dock::pinned_apps::resolveEntries(m_config->config().shell.launcher.pinned);
  if (pinnedEntries.empty()) {
    return;
  }

  std::vector<std::string> pinnedPaths;
  pinnedPaths.reserve(pinnedEntries.size());
  for (const DesktopEntry& entry : pinnedEntries) {
    if (!entry.path.empty()) {
      pinnedPaths.push_back(entry.path);
    }
  }

  std::ranges::stable_sort(m_results, [&pinnedPaths](const LauncherResult& a, const LauncherResult& b) {
    const auto rank = [&pinnedPaths](const LauncherResult& result) {
      if (result.providerId != kApplicationsProviderId) {
        return pinnedPaths.size();
      }
      const auto it = std::ranges::find(pinnedPaths, result.desktopEntryPath);
      return it == pinnedPaths.end() ? pinnedPaths.size() : static_cast<std::size_t>(it - pinnedPaths.begin());
    };
    return rank(a) < rank(b);
  });
}

void LauncherPanel::updatePinnedApplicationState() {
  for (LauncherResult& result : m_allResults) {
    result.pinned = false;
  }
  if (m_config == nullptr || m_config->config().shell.launcher.pinned.empty()) {
    return;
  }

  const auto pinnedEntries = shell::dock::pinned_apps::resolveEntries(m_config->config().shell.launcher.pinned);
  for (LauncherResult& result : m_allResults) {
    if (result.providerId != kApplicationsProviderId) {
      continue;
    }
    result.pinned = std::ranges::any_of(pinnedEntries, [&result](const DesktopEntry& entry) {
      return entry.path == result.desktopEntryPath;
    });
  }
}

void LauncherPanel::reorderPinnedApplication(std::string_view sourcePath, std::string_view targetPath) {
  if (m_config == nullptr || sourcePath.empty() || targetPath.empty() || sourcePath == targetPath) {
    return;
  }

  std::vector<std::string> pinned = m_config->config().shell.launcher.pinned;
  const auto entries = shell::dock::pinned_apps::resolveEntries(pinned);
  const auto indexForPath = [&entries](std::string_view path) -> std::optional<std::size_t> {
    const auto it = std::ranges::find(entries, path, &DesktopEntry::path);
    if (it == entries.end()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(it - entries.begin());
  };

  const auto sourceIndex = indexForPath(sourcePath);
  const auto targetIndex = indexForPath(targetPath);
  if (!sourceIndex.has_value() || !targetIndex.has_value() || *sourceIndex == *targetIndex) {
    return;
  }

  std::string moved = std::move(pinned[*sourceIndex]);
  pinned.erase(pinned.begin() + static_cast<std::ptrdiff_t>(*sourceIndex));
  pinned.insert(pinned.begin() + static_cast<std::ptrdiff_t>(*targetIndex), std::move(moved));

  if (m_config->setOverride({"shell", "launcher", "pinned"}, std::move(pinned))) {
    reapplyCurrentQuery();
  }
}

void LauncherPanel::updateCategoryFilterModel(const std::vector<LauncherCategory>& categories) {
  m_currentCategories = categories;
  m_categoryFilterSlots.clear();
  if (!m_browserMode && categories.empty() && !m_hasRecentlyUsed) {
    m_categoryFilterRebuildPending = true;
    PanelManager::instance().requestLayout();
    return;
  }

  if (m_browserMode) {
    if (m_hasRecentlyUsed) {
      m_categoryFilterSlots.push_back({RecentlyUsed, 0});
    }
    if (m_hasPinnedApplications) {
      m_categoryFilterSlots.push_back({Pinned, 0});
    }
    m_categoryFilterSlots.push_back({All, 0});
    if (!categories.empty()) {
      m_categoryFilterSlots.push_back({Category, 0});
    }
    const auto defaultSlot = std::ranges::find(m_categoryFilterSlots, defaultCategoryType(), &CategoryFilterSlot::type);
    if (defaultSlot != m_categoryFilterSlots.end()) {
      std::ranges::rotate(m_categoryFilterSlots.begin(), defaultSlot, defaultSlot + 1);
    }
  } else {
    m_categoryFilterSlots.push_back({All, 0});
    if (m_hasRecentlyUsed) {
      m_categoryFilterSlots.push_back({RecentlyUsed, 0});
    }
    for (std::size_t i = 0; i < categories.size(); ++i) {
      m_categoryFilterSlots.push_back({Category, i});
    }
  }

  auto activeSlot = m_categoryFilterSlots.end();
  if (m_activeCategoryType == Category) {
    const auto activeCategory = std::ranges::find(categories, m_activeCategory, &LauncherCategory::label);
    if (activeCategory != categories.end()) {
      if (m_browserMode) {
        activeSlot = std::ranges::find(m_categoryFilterSlots, Category, &CategoryFilterSlot::type);
      } else {
        const std::size_t categoryIndex = static_cast<std::size_t>(activeCategory - categories.begin());
        activeSlot = std::ranges::find_if(m_categoryFilterSlots, [categoryIndex](const CategoryFilterSlot& slot) {
          return slot.type == Category && slot.categoryIndex == categoryIndex;
        });
      }
    }
  } else {
    activeSlot = std::ranges::find(m_categoryFilterSlots, m_activeCategoryType, &CategoryFilterSlot::type);
  }

  if (activeSlot == m_categoryFilterSlots.end()) {
    m_activeCategoryType = All;
    m_activeCategory.clear();
    activeSlot = std::ranges::find(m_categoryFilterSlots, All, &CategoryFilterSlot::type);
  }
  m_activeCategorySlotIndex = activeSlot == m_categoryFilterSlots.end()
      ? 0
      : static_cast<std::size_t>(activeSlot - m_categoryFilterSlots.begin());

  m_categoryFilterRebuildPending = true;
  PanelManager::instance().requestLayout();
}

void LauncherPanel::rebuildCategoryFilter() {
  uiAssertNotRendering("LauncherPanel::rebuildCategoryFilter");
  m_categoryFilterRebuildPending = false;

  if (m_categoryFilter == nullptr) {
    rebuildLauncherNavigation();
    return;
  }

  m_categoryFilter->setOnChange({});
  m_categoryFilter->clearOptions();
  const bool categoryIconsOnly = m_config != nullptr && m_config->config().shell.launcher.categoryIconsOnly;
  if (m_browserMode) {
    std::size_t selectedCategory = 0;
    for (std::size_t i = 0; i < m_currentCategories.size(); ++i) {
      const LauncherCategory& category = m_currentCategories[i];
      m_categoryFilter->addOption(categoryIconsOnly ? "" : category.label, category.glyphName);
      m_categoryFilter->setOptionTooltip(i, category.label);
      if (category.label == m_activeCategory) {
        selectedCategory = i;
      }
    }
    m_categoryFilter->setSelectedIndex(selectedCategory);
    m_categoryFilter->setOnChange([this](std::size_t idx) { setActiveBrowserCategory(idx); });
  } else {
    for (std::size_t i = 0; i < m_categoryFilterSlots.size(); ++i) {
      const auto& slot = m_categoryFilterSlots[i];
      switch (slot.type) {
      case All:
        m_categoryFilter->addOption("", "layout-grid");
        m_categoryFilter->setOptionTooltip(i, i18n::tr("launcher.categories.all"));
        break;
      case Pinned:
        m_categoryFilter->addOption("", "pin-filled");
        m_categoryFilter->setOptionTooltip(i, i18n::tr("launcher.browser.navigation.pinned"));
        break;
      case RecentlyUsed:
        m_categoryFilter->addOption("", "history");
        m_categoryFilter->setOptionTooltip(i, i18n::tr("launcher.categories.recently-used"));
        break;
      case Category: {
        const LauncherCategory& category = m_currentCategories[slot.categoryIndex];
        m_categoryFilter->addOption(categoryIconsOnly ? "" : category.label, category.glyphName);
        m_categoryFilter->setOptionTooltip(i, m_currentCategories[slot.categoryIndex].label);
        break;
      }
      }
    }
    m_categoryFilter->setSelectedIndex(m_activeCategorySlotIndex);
    m_categoryFilter->setOnChange([this](std::size_t idx) { setActiveCategorySlot(idx); });
  }
  setCategoryFilterVisible(true);
  rebuildLauncherNavigation();
  updateCategorySelectionChrome();
}

void LauncherPanel::rebuildLauncherNavigation() {
  uiAssertNotRendering("LauncherPanel::rebuildLauncherNavigation");
  if (!m_browserMode || m_launcherNavigation == nullptr) {
    return;
  }

  m_launcherNavigation->setOnChange({});
  m_launcherNavigation->clearOptions();
  const float settingsButtonWidth =
      m_launcherSettingsButton != nullptr ? Style::controlHeight + Style::spaceXs : 0.0F;
  const bool iconOnly = m_config != nullptr
      && (!m_config->config().shell.launcher.showNavigationLabels
          || static_cast<float>(m_config->config().shell.launcher.width)
              < static_cast<float>(kCompactNavigationWidth) + settingsButtonWidth);

  std::size_t navigationIndex = 0;
  for (const CategoryFilterSlot& slot : m_categoryFilterSlots) {
    std::string label;
    std::string glyph;
    switch (slot.type) {
    case All:
      label = i18n::tr("launcher.browser.navigation.all");
      glyph = "apps";
      break;
    case Pinned:
      label = i18n::tr("launcher.browser.navigation.pinned");
      glyph = "pin-filled";
      break;
    case RecentlyUsed:
      label = i18n::tr("launcher.browser.navigation.recent");
      glyph = "history";
      break;
    case Category:
      label = i18n::tr("launcher.browser.navigation.categories");
      glyph = "tag";
      break;
    }
    m_launcherNavigation->addOption(iconOnly ? "" : label, glyph);
    m_launcherNavigation->setOptionTooltip(navigationIndex, label);
    ++navigationIndex;
  }
  const std::string providersLabel = i18n::tr("launcher.browser.navigation.providers");
  m_launcherNavigation->addOption(iconOnly ? "" : providersLabel, "sparkles");
  m_launcherNavigation->setOptionTooltip(navigationIndex, providersLabel);
  const std::size_t selectedIndex =
      m_showingProviderOverview ? m_categoryFilterSlots.size() : m_activeCategorySlotIndex;
  m_launcherNavigation->setSelectedIndex(selectedIndex);
  m_launcherNavigation->setOnChange([this](std::size_t index) {
    if (m_syncingLauncherNavigation) {
      return;
    }
    DeferredCall::callLater([this, index] {
      if (index < m_categoryFilterSlots.size()) {
        setActiveCategorySlot(index);
      } else {
        showProviderOverview();
      }
    });
  });

  updateCategorySelectionChrome();
}

LauncherPanel::ActiveCategoryType LauncherPanel::defaultCategoryType() const {
  if (m_config != nullptr) {
    switch (m_config->config().shell.launcher.defaultView) {
    case ShellConfig::LauncherDefaultView::Recent:
      return RecentlyUsed;
    case ShellConfig::LauncherDefaultView::Pinned:
      return Pinned;
    case ShellConfig::LauncherDefaultView::All:
      return All;
    case ShellConfig::LauncherDefaultView::Categories:
      return Category;
    }
  }
  return All;
}

void LauncherPanel::applyDefaultView() {
  m_applyDefaultViewPending = false;
  if (!m_browserMode || !m_query.empty()) {
    return;
  }

  auto slot = std::ranges::find(m_categoryFilterSlots, All, &CategoryFilterSlot::type);
  if (slot == m_categoryFilterSlots.end()) {
    return;
  }

  m_activeCategorySlotIndex = static_cast<std::size_t>(slot - m_categoryFilterSlots.begin());
  m_activeCategoryType = slot->type;
  if (m_activeCategoryType == Category && !m_currentCategories.empty()) {
    m_activeCategory = m_currentCategories.front().label;
  } else {
    m_activeCategory.clear();
  }
}

void LauncherPanel::setActiveCategorySlot(std::size_t slotIndex) {
  if (m_launcherProviderViewId == kSessionProviderId) {
    m_sessionReturnView.reset();
  }
  const bool leavingProvider = m_browserMode && (!m_query.empty() || !m_launcherProviderViewId.empty());
  const std::size_t previousSlotIndex = m_activeCategorySlotIndex;
  if (leavingProvider) {
    setQuery("");
  }
  if (slotIndex >= m_categoryFilterSlots.size()) {
    return;
  }

  const auto& slot = m_categoryFilterSlots[slotIndex];
  m_activeCategorySlotIndex = slotIndex;
  m_activeCategoryType = slot.type;
  if (slot.type == Category && !m_currentCategories.empty()) {
    if (!m_browserMode && slot.categoryIndex < m_currentCategories.size()) {
      m_activeCategory = m_currentCategories[slot.categoryIndex].label;
    } else if (
        std::ranges::find(m_currentCategories, m_activeCategory, &LauncherCategory::label) == m_currentCategories.end()
    ) {
      m_activeCategory = m_currentCategories.front().label;
    }
  } else if (!m_browserMode) {
    m_activeCategory.clear();
  }
  if (!m_browserMode && m_categoryFilter != nullptr && m_categoryFilter->selectedIndex() != slotIndex) {
    m_categoryFilter->setSelectedIndex(slotIndex);
  }
  updateCategorySelectionChrome();
  if (leavingProvider || slotIndex < previousSlotIndex) {
    queueResultsTransition(ResultsTransition::NavigationBackward);
  } else if (slotIndex > previousSlotIndex) {
    queueResultsTransition(ResultsTransition::NavigationForward);
  }
  applyActiveCategory();
}

void LauncherPanel::setActiveBrowserCategory(std::size_t categoryIndex) {
  if (!m_browserMode || categoryIndex >= m_currentCategories.size()) {
    return;
  }
  const auto previousCategory = std::ranges::find(m_currentCategories, m_activeCategory, &LauncherCategory::label);
  const std::size_t previousCategoryIndex = previousCategory == m_currentCategories.end()
      ? categoryIndex
      : static_cast<std::size_t>(previousCategory - m_currentCategories.begin());
  m_activeCategoryType = Category;
  m_activeCategory = m_currentCategories[categoryIndex].label;
  const auto slot = std::ranges::find(m_categoryFilterSlots, Category, &CategoryFilterSlot::type);
  if (slot != m_categoryFilterSlots.end()) {
    m_activeCategorySlotIndex = static_cast<std::size_t>(slot - m_categoryFilterSlots.begin());
  }
  updateCategorySelectionChrome();
  if (categoryIndex < previousCategoryIndex) {
    queueResultsTransition(ResultsTransition::NavigationBackward);
  } else if (categoryIndex > previousCategoryIndex) {
    queueResultsTransition(ResultsTransition::NavigationForward);
  }
  applyActiveCategory();
}

void LauncherPanel::updateCategorySelectionChrome() {
  if (m_launcherNavigation != nullptr && !m_categoryFilterSlots.empty()) {
    const std::size_t selectedIndex =
        m_showingProviderOverview ? m_categoryFilterSlots.size() : m_activeCategorySlotIndex;
    if (m_launcherNavigation->selectedIndex() != selectedIndex) {
      m_syncingLauncherNavigation = true;
      m_launcherNavigation->setSelectedIndex(selectedIndex);
      m_syncingLauncherNavigation = false;
    }
  }
  if (m_launcherPowerButton != nullptr) {
    m_launcherPowerButton->setSelected(m_launcherProviderViewId == kSessionProviderId);
  }
  setCategoryFilterVisible(true);
}

void LauncherPanel::updateLauncherOptionButtons() {
  if (m_config == nullptr) {
    return;
  }
  const auto& launcher = m_config->config().shell.launcher;
  if (m_launcherUsageButton != nullptr) {
    const bool orderingAvailable = launcher.trackUsage
        && !m_showingProviderOverview
        && m_launcherProviderViewId.empty()
        && (m_activeCategoryType == All || m_activeCategoryType == Category);
    m_launcherUsageButton->setVisible(orderingAvailable);
    m_launcherUsageButton->setParticipatesInLayout(orderingAvailable);
    m_launcherUsageButton->setEnabled(orderingAvailable);
    m_launcherUsageButton->setSelected(false);
    m_launcherUsageButton->setGlyph(launcher.sortByUsage ? "sort-ascending-letters" : "sort-descending-small-big");
    m_launcherUsageButton->setTooltip(
        i18n::tr(
            launcher.sortByUsage ? "launcher.browser.options.use-default-order"
                                 : "launcher.browser.options.sort-by-frequency"
        )
    );
  }
  if (m_launcherGridButton != nullptr) {
    const bool available = supportsResultGrid();
    m_launcherGridButton->setVisible(available);
    m_launcherGridButton->setParticipatesInLayout(available);
    m_launcherGridButton->setEnabled(available);
    m_launcherGridButton->setSelected(false);
    m_launcherGridButton->setGlyph(launcher.appGrid ? "list" : "layout-grid");
    m_launcherGridButton->setTooltip(
        i18n::tr(launcher.appGrid ? "launcher.browser.options.show-list" : "launcher.browser.options.show-grid")
    );
  }
}

void LauncherPanel::setCategoryFilterVisible(bool visible) {
  if (m_categoryFilter == nullptr) {
    return;
  }
  const bool show = m_browserMode ? visible
          && m_activeCategoryType == Category
          && m_query.empty()
          && m_launcherProviderViewId.empty()
          && !m_currentCategories.empty()
                                  : visible
          && !m_categoryFilterSlots.empty()
          && (!m_scopedProviderId.empty() || m_config == nullptr || m_config->config().shell.launcher.categories);
  m_categoryFilter->setVisible(show);
  m_categoryFilter->setParticipatesInLayout(show);
  if (m_container != nullptr) {
    m_container->markLayoutDirty();
  }
}

bool LauncherPanel::startsWithLauncherPrefix(std::string_view text) const {
  const std::string& prefix = m_config != nullptr ? m_config->config().shell.launcher.providerPrefix : "/";
  return text.starts_with(prefix);
}

std::vector<LauncherResult> LauncherPanel::providerOverviewResults(std::string_view text) const {
  std::string filter;
  if (startsWithLauncherPrefix(text)) {
    const std::string& prefix = m_config != nullptr ? m_config->config().shell.launcher.providerPrefix : "/";
    filter = StringUtils::toLower(StringUtils::trim(text.substr(prefix.size())));
  }

  std::vector<LauncherResult> results;
  results.reserve(m_providers.size());
  for (const auto& provider : m_providers) {
    const std::string_view prefix = provider->prefix();
    if (prefix.empty()) {
      continue;
    }

    const std::string title(provider->displayName());
    const std::string prefixText(prefix);
    const std::string searchable = StringUtils::toLower(std::format("{} {}", title, prefixText));
    const double score = filter.empty() ? 0.0 : FuzzyMatch::score(filter, searchable);
    if (!filter.empty() && !FuzzyMatch::isMatch(score)) {
      continue;
    }

    LauncherResult result;
    result.id = providerOverviewId(prefix);
    result.providerId = std::string(kProviderOverviewProviderId);
    result.title = title;
    result.subtitle = prefixText;
    result.glyphName = std::string(provider->defaultGlyphName());
    result.score = score;
    results.push_back(std::move(result));
  }

  if (!filter.empty()) {
    search_ranker::sort(results);
  }
  return results;
}

void LauncherPanel::applyActiveCategory() {
  m_results.clear();
  if (m_launcherProviderViewId == kSessionProviderId) {
    m_results = m_allResults;
    m_selectedIndex = 0;
    updateCategorySelectionChrome();
    refreshResults();
    return;
  }
  switch (m_activeCategoryType) {
  case All:
    m_results = m_allResults;
    if (!m_browserMode && m_scopedProviderId.empty() && m_query.empty()) {
      applyPinnedApplicationOrder();
    }
    break;
  case Pinned:
    std::ranges::copy_if(m_allResults, std::back_inserter(m_results), [](const LauncherResult& result) {
      return result.pinned;
    });
    applyPinnedApplicationOrder();
    break;
  case RecentlyUsed:
    std::ranges::copy_if(m_allResults, std::back_inserter(m_results), [](const LauncherResult& r) {
      return r.recentlyUsedIndex > 0;
    });
    std::ranges::sort(m_results, [](const LauncherResult& a, const LauncherResult& b) {
      return a.recentlyUsedIndex > b.recentlyUsedIndex
          || (a.recentlyUsedIndex == b.recentlyUsedIndex
              && std::tie(a.providerId, a.id) < std::tie(b.providerId, b.id));
    });
    break;
  case Category:
    for (const auto& r : m_allResults) {
      if (r.category == m_activeCategory) {
        m_results.push_back(r);
      }
    }
    break;
  }
  m_selectedIndex = 0;
  updateCategorySelectionChrome();
  refreshResults();
}
