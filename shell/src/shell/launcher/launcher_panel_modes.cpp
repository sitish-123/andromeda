#include "shell/launcher/launcher_panel.h"

#include "shell/launcher/launcher_adapters.h"
#include "shell/launcher/launcher_panel_detail.h"


void LauncherPanel::syncModeTabs() {
  if (m_modeTabs == nullptr) {
    return;
  }
  const float scale = contentScale();
  if (m_modeTabItems.empty()) {
    for (const std::string_view id : kModeProviders) {
      const auto provider = std::ranges::find(m_providers, id, [](const auto& p) { return p->id(); });
      if (provider == m_providers.end()) {
        continue;
      }
      auto tab = ui::row({
          .align = FlexAlign::Center,
          .paddingH = Tokens::spaceP12 * scale,
          .radius = Tokens::sizeControlS * 0.5F * scale,
          .minHeight = Tokens::sizeControlS * scale,
      });
      tab->addChild(ui::label({
          .text = (*provider)->displayName(),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
      }));
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      area->setOnClick([this, modeId = std::string(id)](const InputArea::PointerData&) { setMode(modeId); });
      Flex* tabPtr = tab.get();
      tab->addChild(std::move(area));
      m_modeTabs->addChild(std::move(tab));
      m_modeTabItems.emplace_back(std::string(id), tabPtr);
    }
  }
  const bool show = m_browserMode && !m_modeProviderId.empty();
  m_modeTabs->setVisible(show);
  m_modeTabs->setParticipatesInLayout(show);
  m_modeTabsLine->setVisible(show);
  m_modeTabsLine->setParticipatesInLayout(show);
  for (auto& [id, tab] : m_modeTabItems) {
    const bool on = id == m_modeProviderId;
    tab->setFill(on ? colorSpecFromRole(ColorRole::Lens) : clearColorSpec());
    if (auto* label = dynamic_cast<Label*>(tab->children().front().get())) {
      label->setColor(colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2));
    }
    if (auto* area = dynamic_cast<InputArea*>(tab->children().back().get())) {
      area->setFrameSize(tab->width(), tab->height());
    }
  }
}

void LauncherPanel::syncPackStrip() {
  if (m_packStrip == nullptr) {
    return;
  }
  const bool show = m_browserMode && m_modeProviderId == "Stickers";
  m_packStrip->setVisible(show);
  m_packStrip->setParticipatesInLayout(show);
  if (show && m_packStrip->children().empty()) {
    const float scale = contentScale();
    for (std::size_t i = 0; i < kStickerPacks.size(); ++i) {
      m_packStrip->addChild(ui::button({
          .text = i18n::tr(std::format("launcher.stickers.packs.{}", kStickerPacks[i].second)),
          .controlHeight = Tokens::sizeControlS * scale,
          .variant = ButtonVariant::Ghost,
          .onClick = [this, i] { setStickerPack(i); },
      }));
    }
  }
  for (std::size_t i = 0; i < m_packStrip->children().size(); ++i) {
    if (auto* chip = dynamic_cast<Button*>(m_packStrip->children()[i].get())) {
      chip->setVariant(i == m_packIndex ? ButtonVariant::Tonal : ButtonVariant::Ghost);
    }
  }
}

void LauncherPanel::syncEmojiStrip() {
  if (m_emojiStrip == nullptr) {
    return;
  }
  const bool show = m_browserMode && m_modeProviderId == "Emoji" && m_query.empty();
  m_emojiStrip->setVisible(show);
  m_emojiStrip->setParticipatesInLayout(show);
  if (!show || !m_emojiStrip->children().empty()) {
    return;
  }
  const auto emoji = std::ranges::find(m_providers, "Emoji", [](const auto& p) { return p->id(); });
  if (emoji == m_providers.end()) {
    return;
  }
  const float scale = contentScale();
  std::vector<LauncherCategory> groups{{i18n::tr("launcher.pill.recent"), "clock"}};
  std::ranges::copy((*emoji)->categories(), std::back_inserter(groups));
  for (const LauncherCategory& group : groups) {
    m_emojiStrip->addChild(ui::button({
        .glyph = group.glyphName,
        .glyphSize = Tokens::sizeSymbol * scale,
        .controlHeight = Tokens::sizeControlS * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = group.label,
        .minWidth = Tokens::sizeControlS * scale,
        .onClick =
            [this, label = group.label] {
              if (m_launcherSectionsAdapter == nullptr || m_launcherSections == nullptr) {
                return;
              }
              if (const auto row = m_launcherSectionsAdapter->rowForHeader(label)) {
                m_launcherSections->scrollToIndex(*row);
              }
            },
    }));
  }
}

void LauncherPanel::setMode(std::string_view providerId) {
  m_modeProviderId = std::string(providerId);
  m_selectedIndex = 0;
  syncModeTabs();
  if (m_input != nullptr && !m_input->value().empty()) {
    m_input->setValue("");
  }
  onInputChanged("");
}

void LauncherPanel::setFilesScope(bool on) {
  m_filesScope = on;
  m_selectedIndex = 0;
  if (!on) {
    m_kindIndex = 0;
    if (auto* files = filesProvider()) {
      files->setScopeKind({});
    }
    if (m_filesTokenLabel != nullptr) {
      m_filesTokenLabel->setText(i18n::tr("launcher.files.title"));
    }
  }
  syncKindTabs();
  if (m_filesToken != nullptr) {
    m_filesToken->setVisible(on);
    m_filesToken->setParticipatesInLayout(on);
  }
  if (m_launcherSections != nullptr) {
    m_launcherSections->scrollView().setScrollOffset(0.0F);
  }
  queueResultsTransition(on ? ResultsTransition::NavigationForward : ResultsTransition::NavigationBackward);
  onInputChanged(m_query);
}

void LauncherPanel::setFilesKind(std::size_t index) {
  m_kindIndex = index % kFileKinds.size();
  const auto& [word, key] = kFileKinds[m_kindIndex];
  if (auto* files = filesProvider()) {
    files->setScopeKind(std::string(word));
  }
  if (m_filesTokenLabel != nullptr) {
    m_filesTokenLabel->setText(
        word.empty() ? i18n::tr("launcher.files.title")
                     : i18n::tr("launcher.files.title") + " · " + i18n::tr(std::format("launcher.files.kinds.{}", key))
    );
  }
  m_selectedIndex = 0;
  syncKindTabs();
  onInputChanged(m_query);
}

void LauncherPanel::setStickerPack(std::size_t index) {
  m_packIndex = index % kStickerPacks.size();
  if (auto* stickers = stickersProvider()) {
    stickers->setPack(std::string(kStickerPacks[m_packIndex].first));
  }
  m_selectedIndex = 0;
  syncPackStrip();
  onInputChanged(m_query);
}

KlipyProvider* LauncherPanel::stickersProvider() const {
  const auto provider = std::ranges::find(m_providers, "Stickers", [](const auto& p) { return p->id(); });
  return provider == m_providers.end() ? nullptr : dynamic_cast<KlipyProvider*>(provider->get());
}

void LauncherPanel::syncKindTabs() {
  if (m_kindTabs == nullptr) {
    return;
  }
  const float scale = contentScale();
  if (m_kindTabItems.empty()) {
    for (std::size_t i = 0; i < kFileKinds.size(); ++i) {
      auto tab = ui::row({
          .align = FlexAlign::Center,
          .paddingH = Tokens::spaceP12 * scale,
          .radius = Tokens::sizeControlS * 0.5F * scale,
          .minHeight = Tokens::sizeControlS * scale,
      });
      tab->addChild(ui::label({
          .text = i18n::tr(std::format("launcher.files.kinds.{}", kFileKinds[i].second)),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::Label2),
      }));
      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      area->setOnClick([this, i](const InputArea::PointerData&) { setFilesKind(i); });
      Flex* tabPtr = tab.get();
      tab->addChild(std::move(area));
      m_kindTabs->addChild(std::move(tab));
      m_kindTabItems.push_back(tabPtr);
    }
  }
  const bool show = m_browserMode && m_filesScope;
  m_kindTabs->setVisible(show);
  m_kindTabs->setParticipatesInLayout(show);
  m_kindTabsLine->setVisible(show);
  m_kindTabsLine->setParticipatesInLayout(show);
  for (std::size_t i = 0; i < m_kindTabItems.size(); ++i) {
    Flex* tab = m_kindTabItems[i];
    const bool on = i == m_kindIndex;
    tab->setFill(on ? colorSpecFromRole(ColorRole::Lens) : clearColorSpec());
    if (auto* label = dynamic_cast<Label*>(tab->children().front().get())) {
      label->setColor(colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2));
    }
    if (auto* area = dynamic_cast<InputArea*>(tab->children().back().get())) {
      area->setFrameSize(tab->width(), tab->height());
    }
  }
}

void LauncherPanel::cycleMode(int direction) {
  if (m_modeTabItems.empty() || m_modeProviderId.empty()) {
    return;
  }
  const auto it = std::ranges::find(m_modeTabItems, m_modeProviderId, &std::pair<std::string, Flex*>::first);
  const auto count = static_cast<int>(m_modeTabItems.size());
  const int index = it == m_modeTabItems.end() ? 0 : static_cast<int>(it - m_modeTabItems.begin());
  setMode(m_modeTabItems[static_cast<std::size_t>((index + direction + count) % count)].first);
}

void LauncherPanel::bindPreview(Renderer& renderer) {
  if (m_previewPane == nullptr || !m_previewPane->visible()) {
    return;
  }
  // No row, no preview: the last bound result must not stay beside an empty list.
  m_previewPane->setOpacity(m_selectedIndex < m_results.size() ? 1.0F : 0.0F);
  if (m_selectedIndex >= m_results.size()) {
    return;
  }
  const LauncherResult& result = m_results[m_selectedIndex];
  const float scale = contentScale();
  const int iconSize = static_cast<int>(std::round(Tokens::componentLauncherIcon * scale));
  const bool emoji = !result.badge.empty();
  m_previewEmoji->setVisible(emoji);
  m_previewEmoji->setParticipatesInLayout(emoji);
  m_previewEmoji->setText(result.badge);
  const bool tones = emoji && result.variants.size() == m_previewToneLabels.size();
  m_previewTones->setVisible(tones);
  m_previewTones->setParticipatesInLayout(tones);
  for (std::size_t i = 0; tones && i < m_previewToneLabels.size(); ++i) {
    m_previewToneLabels[i]->setText(result.variants[i]);
  }
  bool imageReady = false;
  const bool media = isMediaPresentation(result);
  // A clipboard image row keeps its thumbnail in iconPath; the preview shows the full picture instead.
  if (!emoji && !media && !result.iconPath.empty() && result.presentation != "clipboard-image") {
    if (result.iconPath != m_previewIconPath) {
      m_previewImage->clear(renderer);
      m_previewIconPath = result.iconPath;
    }
    imageReady = m_asyncTextures != nullptr
        ? m_previewImage->setSourceFileAsync(renderer, *m_asyncTextures, result.iconPath, iconSize, true)
        : m_previewImage->setSourceFile(renderer, result.iconPath, iconSize, true);
  }
  m_previewImage->setVisible(imageReady);
  m_previewImage->setParticipatesInLayout(imageReady);
  m_previewGlyph->setGlyph(result.glyphName.empty() ? "app-window" : result.glyphName);
  m_previewGlyph->setVisible(!imageReady && !emoji);
  m_previewGlyph->setParticipatesInLayout(!imageReady && !emoji);
  m_previewOpen->setText(
      i18n::tr(
          emoji || (!m_modeProviderId.empty() && result.presentation != "action") ? "launcher.pill.paste-hint"
                                                                                  : "launcher.pill.open-hint"
      )
  );

  std::string title = singleLinePreview(result.title);
  if (emoji && !title.empty()) {
    // CLDR emoji names are lower case; the preview shows them in sentence case.
    title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
  }
  m_previewTitle->setText(title);
  m_previewTitle->setMaxWidth(m_previewPane->width() - 2.0F * Tokens::spaceP20 * scale);
  const std::string kind = result.origin.empty() ? result.subtitle : result.subtitle + " · " + result.origin;
  m_previewKind->setText(singleLinePreview(kind));
  // A clipboard entry's time already shows in its details.
  m_previewKind->setVisible(!kind.empty() && !result.presentation.starts_with("clipboard"));

  const DesktopEntry* entry = nullptr;
  if (!result.desktopEntryPath.empty()) {
    const auto& entries = desktopEntries();
    const auto it = std::ranges::find(entries, result.desktopEntryPath, &DesktopEntry::path);
    entry = it == entries.end() ? nullptr : &*it;
  }
  const bool clip = result.presentation.starts_with("clipboard");
  const bool file = result.providerId == "Files" && result.id != FilesProvider::kShowAllId;
  const bool hasAction = clip || file || (entry != nullptr && !entry->actions.empty());
  m_previewAction->setVisible(hasAction);
  m_previewAction->setParticipatesInLayout(hasAction);
  if (clip) {
    m_previewAction->setText(i18n::tr(result.pinned ? "launcher.clipboard.unpin" : "launcher.clipboard.pin"));
  } else if (file) {
    m_previewAction->setText(i18n::tr("launcher.files.reveal"));
  } else if (hasAction) {
    m_previewAction->setText(entry->actions.front().name);
  }
  m_previewDelete->setText(i18n::tr(file ? "launcher.files.copy-path" : "launcher.clipboard.delete"));
  m_previewDelete->setVisible(clip || file);
  m_previewDelete->setParticipatesInLayout(clip || file);
  const bool body = result.presentation == "clipboard-text";
  m_previewBodyBox->setVisible(body);
  m_previewBodyBox->setParticipatesInLayout(body);
  m_previewTitle->setVisible(!body);
  m_previewTitle->setParticipatesInLayout(!body);
  if (body) {
    const auto provider = std::ranges::find(m_providers, "Clipboard", [](const auto& p) { return p->id(); });
    const auto* clipboard = provider == m_providers.end() ? nullptr : dynamic_cast<const ClipboardProvider*>(provider->get());
    m_previewBody->setText(clipboard == nullptr ? result.title : clipboard->text(result));
    m_previewBody->setMaxWidth(m_previewPane->width() - 2.0F * (Tokens::spaceP20 + Tokens::spaceP12) * scale);
  }
  bool shot = result.presentation == "clipboard-image";
  std::string gif;
  if (result.presentation == "gif") {
    const auto provider = std::ranges::find(m_providers, "Gifs", [](const auto& p) { return p->id(); });
    if (auto* klipy = provider == m_providers.end() ? nullptr : dynamic_cast<KlipyProvider*>(provider->get())) {
      gif = klipy->previewGif(result);
    }
  }
  if (!gif.empty() && playPreviewGif(renderer, gif)) {
    m_previewShotId.clear();
    shot = true;
  } else if (media && m_asyncTextures != nullptr) {
    stopPreviewGif();
    m_previewShotId.clear();
    shot = !result.iconPath.empty() && m_previewShot->setSourceFileAsync(
        renderer, *m_asyncTextures, result.iconPath,
        static_cast<int>(std::round(Tokens::componentLauncherPreviewMedia * 1.75F * scale)), true
    );
  }
  m_previewShot->setVisible(shot);
  m_previewShot->setParticipatesInLayout(shot);
  ClipboardProvider* clipboard = nullptr;
  if (clip) {
    m_previewOpen->setText(i18n::tr("launcher.pill.paste-hint"));
    const auto provider = std::ranges::find(m_providers, "Clipboard", [](const auto& p) { return p->id(); });
    clipboard = provider == m_providers.end() ? nullptr : dynamic_cast<ClipboardProvider*>(provider->get());
  }
  // Decode only when the selection moves to another image: the payload may load from disk.
  if (!media) {
    stopPreviewGif();
  }
  if (shot && clipboard != nullptr && m_previewShotId != result.id) {
    m_previewShotId = result.id;
    const auto bytes = clipboard->imageBytes(result);
    if (bytes.empty() || !m_previewShot->setSourceBytes(renderer, bytes.data(), bytes.size())) {
      m_previewShot->clear(renderer);
    }
  }
  if (shot || media || body) {
    m_previewGlyph->setVisible(false);
    m_previewGlyph->setParticipatesInLayout(false);
  }

  std::array<std::pair<std::string, std::string>, 3> meta{};
  std::size_t count = 0;
  if (entry != nullptr) {
    const int uses = m_usageTracker.getCount(result.providerId, result.id);
    if (const auto last = m_usageTracker.getLastUsed(result.providerId, result.id)) {
      meta[count++] = {i18n::tr("launcher.pill.last-opened"),
                       formatTimeAgo(std::chrono::system_clock::time_point(std::chrono::seconds(*last)))};
    }
    if (uses > 0) {
      meta[count++] = {i18n::tr("launcher.pill.opened"), i18n::trp("launcher.pill.times", uses)};
    }
    if (!result.category.empty()) {
      meta[count++] = {i18n::tr("launcher.pill.category"), result.category};
    }
  } else if (clip) {
    meta[count++] = {i18n::tr("launcher.clipboard.copied"), result.subtitle};
    if (clipboard != nullptr) {
      if (const std::size_t bytes = clipboard->byteSize(result); bytes > 0) {
        meta[count++] = {i18n::tr("launcher.clipboard.size"), formatByteSize(bytes)};
      }
    }
  } else if (file) {
    // One stat on selection: the index answer carries no sizes, and the file may have changed since.
    std::error_code ec;
    const auto status = std::filesystem::status(result.id, ec);
    if (!ec && std::filesystem::is_regular_file(status)) {
      meta[count++] = {i18n::tr("launcher.files.size"), formatByteSize(std::filesystem::file_size(result.id, ec))};
    }
    if (const auto written = std::filesystem::last_write_time(result.id, ec); !ec) {
      meta[count++] = {i18n::tr("launcher.files.modified"),
                       formatTimeAgo(std::chrono::clock_cast<std::chrono::system_clock>(written))};
    }
  } else if (emoji) {
    meta[count++] = {i18n::tr("launcher.pill.category"), result.category};
    if (const int uses = m_usageTracker.getCount(result.providerId, result.id); uses > 0) {
      meta[count++] = {i18n::tr("launcher.pill.used"), i18n::trp("launcher.pill.times", uses)};
    }
    std::string codepoints;
    for (std::size_t i = 0; i < result.badge.size();) {
      char32_t cp = 0;
      const auto c = static_cast<unsigned char>(result.badge[i]);
      const std::size_t len = c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
      cp = len == 1 ? c : (c & (0xFF >> (len + 1)));
      for (std::size_t k = 1; k < len && i + k < result.badge.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(result.badge[i + k]) & 0x3F);
      }
      i += len;
      if (cp != 0xFE0F && cp != 0x200D) {
        codepoints += std::format("{}U+{:04X}", codepoints.empty() ? "" : " ", static_cast<std::uint32_t>(cp));
      }
    }
    meta[count++] = {i18n::tr("launcher.pill.unicode"), codepoints};
  }
  for (std::size_t i = 0; i < m_previewMetaRows.size(); ++i) {
    const bool show = i < count;
    m_previewMetaRows[i]->setVisible(show);
    m_previewMetaRows[i]->setParticipatesInLayout(show);
    if (show) {
      m_previewMetaKeys[i]->setText(meta[i].first);
      m_previewMetaValues[i]->setText(meta[i].second);
    }
  }
}

void LauncherPanel::activateEmojiVariant(std::size_t tone) {
  if (m_selectedIndex >= m_results.size() || tone >= m_results[m_selectedIndex].variants.size()) {
    return;
  }
  LauncherResult result = m_results[m_selectedIndex];
  result.id = "emoji-" + result.variants[tone];
  for (auto& provider : m_providers) {
    if (provider->id() == std::string_view(result.providerId) && provider->activate(result)) {
      finishActivation(*provider, result.id, provider->supportsAutoPaste());
      return;
    }
  }
}

void LauncherPanel::deleteSelectedClipboardEntry() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }
  if (auto* files = filesProvider(); files != nullptr && m_results[m_selectedIndex].providerId == "Files") {
    files->copyPath(m_results[m_selectedIndex]);
    finishActivation(*files, m_results[m_selectedIndex].id, false);
    return;
  }
  const auto provider = std::ranges::find(m_providers, "Clipboard", [](const auto& p) { return p->id(); });
  if (provider != m_providers.end()) {
    if (auto* clipboard = dynamic_cast<ClipboardProvider*>(provider->get()); clipboard != nullptr
        && clipboard->remove(m_results[m_selectedIndex])) {
      reapplyCurrentQuery();
    }
  }
}

FilesProvider* LauncherPanel::filesProvider() const {
  const auto provider = std::ranges::find(m_providers, "Files", [](const auto& p) { return p->id(); });
  return provider == m_providers.end() ? nullptr : dynamic_cast<FilesProvider*>(provider->get());
}

void LauncherPanel::activatePreviewAction() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }
  if (auto* files = filesProvider(); files != nullptr && m_results[m_selectedIndex].providerId == "Files") {
    files->reveal(m_results[m_selectedIndex]);
    finishActivation(*files, m_results[m_selectedIndex].id, false);
    return;
  }
  if (m_results[m_selectedIndex].presentation.starts_with("clipboard")) {
    const auto provider = std::ranges::find(m_providers, "Clipboard", [](const auto& p) { return p->id(); });
    if (provider != m_providers.end()) {
      if (auto* clipboard = dynamic_cast<ClipboardProvider*>(provider->get()); clipboard != nullptr
          && clipboard->togglePinned(m_results[m_selectedIndex])) {
        reapplyCurrentQuery();
      }
    }
    return;
  }
  LauncherResult result = m_results[m_selectedIndex];
  const auto& entries = desktopEntries();
  const auto entry = std::ranges::find(entries, result.desktopEntryPath, &DesktopEntry::path);
  if (entry == entries.end() || entry->actions.empty()) {
    return;
  }
  const DesktopAction& action = entry->actions.front();
  result.id = AppProvider::actionResultId(result.desktopEntryPath, action.id);
  result.desktopActionId = action.id;
  for (auto& provider : m_providers) {
    if (provider->id() == std::string_view(result.providerId) && provider->activate(result)) {
      finishActivation(*provider, result.id, provider->supportsAutoPaste(), true);
      return;
    }
  }
}

bool LauncherPanel::playPreviewGif(Renderer& renderer, const std::string& path) {
  if (path == m_previewGifPath && !m_previewFrames.empty()) {
    return true;
  }
  stopPreviewGif();
  const std::vector<std::uint8_t> bytes = FileUtils::readBinaryFile(path);
  auto decoded = decodeAnimatedGif(bytes.data(), bytes.size(), kMaxPreviewGifFrames, kMaxPreviewGifBytes);
  if (!decoded || decoded->frames.empty()) {
    return false;
  }
  for (const auto& frame : decoded->frames) {
    const TextureHandle handle = renderer.textureManager().loadFromRgba(frame.rgba.data(), decoded->width, decoded->height, false);
    if (handle.id == 0) {
      break;
    }
    m_previewFrames.push_back({handle, std::chrono::milliseconds(std::max<std::uint32_t>(frame.durationMs, 20))});
  }
  if (m_previewFrames.empty()) {
    return false;
  }
  m_previewRenderer = &renderer;
  m_previewGifPath = path;
  m_previewFrame = 0;
  m_previewShot->setExternalTexture(renderer, m_previewFrames.front().handle);
  if (m_previewFrames.size() > 1) {
    m_previewFrameTimer.start(m_previewFrames.front().duration, [this] { showNextPreviewFrame(); });
  }
  return true;
}

void LauncherPanel::showNextPreviewFrame() {
  if (m_previewFrames.size() <= 1 || m_previewRenderer == nullptr || m_previewShot == nullptr) {
    return;
  }
  m_previewFrame = (m_previewFrame + 1) % m_previewFrames.size();
  m_previewShot->setExternalTexture(*m_previewRenderer, m_previewFrames[m_previewFrame].handle);
  PanelManager::instance().requestRedraw();
  m_previewFrameTimer.start(m_previewFrames[m_previewFrame].duration, [this] { showNextPreviewFrame(); });
}

void LauncherPanel::stopPreviewGif() {
  m_previewFrameTimer.stop();
  if (m_previewFrames.empty()) {
    return;
  }
  if (m_previewShot != nullptr && m_previewRenderer != nullptr) {
    m_previewShot->clear(*m_previewRenderer);
  }
  for (PreviewFrame& frame : m_previewFrames) {
    m_previewRenderer->textureManager().unload(frame.handle);
  }
  m_previewFrames.clear();
  m_previewGifPath.clear();
}

bool LauncherPanel::shouldUseDetailPresentation() const {
  return m_results.size() == 1 && isDetailPresentation(m_results.front());
}

void LauncherPanel::bindDetailResult() {
  if (!shouldUseDetailPresentation()
      || m_detailScroll == nullptr
      || m_detailSubtitle == nullptr
      || m_detailBody == nullptr) {
    return;
  }

  const LauncherResult& result = m_results.front();
  const bool hasSubtitle = !result.subtitle.empty();
  m_detailSubtitle->setVisible(hasSubtitle);
  m_detailSubtitle->setParticipatesInLayout(hasSubtitle);
  m_detailSubtitle->setText(singleLinePreview(result.subtitle));
  m_detailBody->setText(result.title.empty() ? result.id : result.title);
  m_detailScroll->setScrollOffset(0.0F);
}
