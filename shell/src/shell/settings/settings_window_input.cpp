#include "i18n/i18n.h"
#include "shell/settings/settings_content_input.h"
#include "shell/settings/settings_window.h"
#include "wayland/wayland_connection.h"

void SettingsWindow::addInputContent(float scale) {
  if (m_selectedSection != "input") {
    m_inputControl.reset();
    m_xkbCatalog.reset();
    return;
  }
  if (m_inputControl == nullptr && m_wayland != nullptr) {
    const auto [name, version] = m_wayland->desktopInputGlobal();
    m_inputControl =
        std::make_unique<InputControl>(m_wayland->registry(), name, version, [this]() { onInputChanged(); });
  }
  if (!m_xkbCatalog.has_value()) {
    m_xkbCatalog = xkb::loadCatalog();
  }

  settings::addSettingsInput(
      *m_contentContainer,
      settings::SettingsInputContext{
          .scale = scale,
          .input = m_inputControl.get(),
          .catalog = &*m_xkbCatalog,
          .set = [this](std::string key, std::string value) {
            if (m_inputControl != nullptr) {
              m_inputControl->set(key, value);
            }
          },
      }
  );
}

void SettingsWindow::onInputChanged() { requestContentRebuild(); }
