#include "i18n/i18n.h"
#include "shell/settings/settings_content_input.h"
#include "shell/settings/settings_content_shortcuts.h"
#include "shell/settings/settings_window.h"
#include "wayland/wayland_connection.h"

#include <utility>

void SettingsWindow::ensureCompositorSettings() {
  if (m_compositorSettings != nullptr || m_wayland == nullptr) {
    return;
  }
  const auto [name, version] = m_wayland->desktopSettingsGlobal();
  if (name == 0) {
    return;
  }
  m_compositorSettings = std::make_unique<SettingsControl>(m_wayland->registry(), name, version, [this]() {
    const std::string& failure = m_compositorSettings->lastFailure();
    if (!failure.empty() && failure != m_shownCompositorFailure) {
      m_shownCompositorFailure = failure;
      showTransientStatus(failure, true);
      return;
    }
    // The first batch adds whole pages, so the sidebar rebuilds with the content.
    if (!std::exchange(m_compositorPagesShown, true)) {
      requestSceneRebuild();
      return;
    }
    requestContentRebuild(/*refreshRegistry=*/true, /*refreshFilterRow=*/true);
  });
}

void SettingsWindow::setCompositorSetting(const std::string& key, const std::string& value) {
  if (m_compositorSettings != nullptr) {
    m_shownCompositorFailure.clear();
    m_compositorSettings->set(key, value);
  }
}

void SettingsWindow::addInputContent(float scale) {
  if (m_selectedSection != "input") {
    m_xkbCatalog.reset();
    return;
  }
  if (!m_xkbCatalog.has_value()) {
    m_xkbCatalog = xkb::loadCatalog();
  }

  settings::addSettingsInput(
      *m_contentContainer,
      settings::SettingsInputContext{
          .scale = scale,
          .input = m_compositorSettings.get(),
          .catalog = &*m_xkbCatalog,
          .set = [this](std::string key, std::string value) { setCompositorSetting(key, value); },
      }
  );
}

void SettingsWindow::addShortcutsContent(float scale) {
  if (m_selectedSection != "shortcuts") {
    if (m_compositorSettings != nullptr && m_compositorSettings->capturing()) {
      m_compositorSettings->cancelCapture();
    }
    m_recordingShortcutRow.clear();
    return;
  }
  settings::addSettingsShortcuts(
      *m_contentContainer,
      settings::SettingsShortcutsContext{
          .scale = scale,
          .compositor = m_compositorSettings.get(),
          .draft = m_shortcutDraft,
          .recordingRow = m_recordingShortcutRow,
          .bind =
              [this](std::string chord, std::string action) {
                if (m_compositorSettings != nullptr) {
                  m_shownCompositorFailure.clear();
                  m_compositorSettings->bind(chord, action);
                }
              },
          .record =
              [this](std::string row, std::function<void(std::string)> onCaptured) {
                if (m_compositorSettings == nullptr) {
                  return;
                }
                m_recordingShortcutRow = std::move(row);
                m_compositorSettings->captureChord([this, onCaptured = std::move(onCaptured)](std::string chord) {
                  m_recordingShortcutRow.clear();
                  onCaptured(std::move(chord));
                  requestContentRebuild();
                });
                requestContentRebuild();
              },
          .cancelRecording =
              [this]() {
                if (m_compositorSettings != nullptr) {
                  m_compositorSettings->cancelCapture();
                }
                m_recordingShortcutRow.clear();
                requestContentRebuild();
              },
          .requestRebuild = [this]() { requestContentRebuild(); },
      }
  );
}
