#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_language.h"
#include "shell/settings/settings_window.h"

void SettingsWindow::addLanguageContent(float scale) {
  if (m_selectedSection != "language") {
    m_localeService.reset();
    m_xkbCatalog.reset();
    return;
  }
  if (m_localeService == nullptr && m_systemBus != nullptr) {
    m_localeService = std::make_unique<LocaleService>(*m_systemBus, [this]() { onLanguageChanged(); });
  }
  if (!m_xkbCatalog.has_value()) {
    m_xkbCatalog = xkb::loadCatalog();
  }
  settings::addSettingsLanguage(
      *m_contentContainer,
      settings::SettingsLanguageContext{
          .scale = scale,
          .locale = m_localeService.get(),
          .catalog = &*m_xkbCatalog,
          .setLang =
              [this](std::string lang) {
                if (m_localeService != nullptr) {
                  m_localeService->setLocale({"LANG=" + lang});
                }
              },
          .setX11Layout =
              [this](std::string layout, std::string variant) {
                if (m_localeService != nullptr) {
                  m_localeService->setX11Keyboard(layout, "", variant, "", /*convert=*/true);
                }
              },
      }
  );
}

void SettingsWindow::onLanguageChanged() { requestContentRebuild(); }
