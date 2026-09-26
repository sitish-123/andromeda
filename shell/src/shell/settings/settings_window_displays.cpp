#include "i18n/i18n.h"
#include "shell/settings/settings_content_displays.h"
#include "shell/settings/settings_window.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <chrono>

namespace {

  constexpr int kDisplayConfirmSeconds = 15;

  std::vector<OutputHeadConfig> currentDisplayConfigs(const OutputManagement& outputs) {
    std::vector<OutputHeadConfig> configs;
    configs.reserve(outputs.heads().size());
    for (const OutputHead& head : outputs.heads()) {
      configs.push_back(OutputManagement::currentConfig(head));
    }
    return configs;
  }

  // An edit survives a state change only while its head and mode still exist.
  bool editsStillValid(const std::vector<OutputHeadConfig>& edits, const OutputManagement& outputs) {
    const auto& heads = outputs.heads();
    if (edits.size() != heads.size()) {
      return false;
    }
    for (std::size_t i = 0; i < heads.size(); ++i) {
      if (edits[i].name != heads[i].name
          || (edits[i].mode != nullptr && !std::ranges::contains(heads[i].modes, edits[i].mode, &OutputMode::handle))) {
        return false;
      }
    }
    return true;
  }

} // namespace

void SettingsWindow::addDisplaysContent(float scale) {
  if (m_selectedSection != "displays") {
    if (m_displayConfirmSecondsLeft == 0) {
      m_outputManagement.reset();
      m_mirrorControl.reset();
      m_displayEdits.clear();
    }
    return;
  }
  if (m_outputManagement == nullptr && m_wayland != nullptr) {
    const auto [name, version] = m_wayland->outputManagerGlobal();
    m_outputManagement =
        std::make_unique<OutputManagement>(m_wayland->registry(), name, version, [this]() { onDisplaysChanged(); });
    if (const auto [mirrorName, mirrorVersion] = m_wayland->desktopOutputGlobal(); mirrorName != 0) {
      m_mirrorControl = std::make_unique<MirrorControl>(m_wayland->registry(), mirrorName, mirrorVersion, [this]() {
        m_displayError = m_mirrorControl->lastFailure();
        requestContentRebuild();
      });
    }
  }

  const bool dirty = m_outputManagement != nullptr
      && m_outputManagement->ready()
      && m_displayEdits != currentDisplayConfigs(*m_outputManagement);
  settings::addSettingsDisplays(
      *m_contentContainer,
      settings::SettingsDisplaysContext{
          .scale = scale,
          .outputs = m_outputManagement.get(),
          .mirrors = m_mirrorControl.get(),
          .edits = m_displayEdits,
          .dirty = dirty,
          .confirmSecondsLeft = m_displayConfirmSecondsLeft,
          .error = m_displayError,
          .edit = [this](OutputHeadConfig config) { editDisplay(std::move(config)); },
          .apply = [this]() { applyDisplays(m_displayEdits, /*confirm=*/true); },
          .discard =
              [this]() {
                m_displayEdits = currentDisplayConfigs(*m_outputManagement);
                m_displayError.clear();
                requestContentRebuild();
              },
          .keep = [this]() { finishDisplayConfirm(/*keep=*/true); },
          .revert = [this]() { finishDisplayConfirm(/*keep=*/false); },
          .setMirror =
              [this](std::string target, std::string source) {
                if (source.empty()) {
                  m_mirrorControl->clearMirror(target);
                } else {
                  m_mirrorControl->setMirror(target, source);
                }
              },
      }
  );
}

void SettingsWindow::onDisplaysChanged() {
  if (!m_outputManagement->ready() || !editsStillValid(m_displayEdits, *m_outputManagement)) {
    m_displayEdits = currentDisplayConfigs(*m_outputManagement);
  }
  requestContentRebuild();
}

void SettingsWindow::editDisplay(OutputHeadConfig config) {
  const auto it = std::ranges::find(m_displayEdits, config.name, &OutputHeadConfig::name);
  if (it == m_displayEdits.end()) {
    return;
  }
  *it = std::move(config);
  m_displayError.clear();
  requestContentRebuild();
}

void SettingsWindow::applyDisplays(std::vector<OutputHeadConfig> config, bool confirm) {
  if (m_outputManagement == nullptr) {
    return;
  }
  std::vector<OutputHeadConfig> previous = currentDisplayConfigs(*m_outputManagement);
  const bool sent = m_outputManagement->apply(
      config, /*testOnly=*/false, [this, previous = std::move(previous), confirm](OutputApplyResult result) mutable {
        if (result == OutputApplyResult::Succeeded && confirm) {
          m_displayRevertTo = std::move(previous);
          m_displayConfirmSecondsLeft = kDisplayConfirmSeconds;
          m_displayConfirmTimer.startRepeating(std::chrono::seconds(1), [this]() {
            if (--m_displayConfirmSecondsLeft <= 0) {
              finishDisplayConfirm(/*keep=*/false);
              return;
            }
            requestContentRebuild();
          });
        } else if (result == OutputApplyResult::Failed) {
          m_displayError = i18n::tr("settings.displays.failed");
        } else if (result == OutputApplyResult::Cancelled) {
          m_displayError = i18n::tr("settings.displays.cancelled");
        }
        requestContentRebuild();
      }
  );
  if (!sent) {
    m_displayError = i18n::tr("settings.displays.busy");
    requestContentRebuild();
  }
}

void SettingsWindow::finishDisplayConfirm(bool keep) {
  m_displayConfirmTimer.stop();
  m_displayConfirmSecondsLeft = 0;
  if (!keep) {
    applyDisplays(std::exchange(m_displayRevertTo, {}), /*confirm=*/false);
  }
  m_displayRevertTo.clear();
  requestContentRebuild();
}
