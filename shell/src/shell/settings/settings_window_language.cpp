#include "core/shared_worker.h"
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
  if (!m_localeNamesRequested) {
    m_localeNamesRequested = true;
    SharedWorker::instance().post<std::vector<std::pair<std::string, std::string>>>(
        std::weak_ptr<void>(m_aliveGuard), [] { return settings::loadLocaleNames(); },
        [this](std::vector<std::pair<std::string, std::string>> names) {
          m_localeNames = std::move(names);
          if (isOpen() && m_selectedSection == "language") {
            requestContentRebuild();
          }
        }
    );
  }
  settings::addSettingsLanguage(
      *m_contentContainer,
      settings::SettingsLanguageContext{
          .scale = scale,
          .locale = m_localeService.get(),
          .catalog = &*m_xkbCatalog,
          .locales = m_localeNames,
          .setLocale =
              [this](std::vector<std::string> assignments) {
                if (m_localeService != nullptr) {
                  m_localeService->setLocale(assignments);
                }
              },
          .setX11Layout =
              [this](std::string layout, std::string variant) {
                if (m_localeService != nullptr) {
                  m_localeService->setX11Keyboard(layout, "", variant, "", /*convert=*/true);
                }
              },
          .openKeyboard = [this] { navigateTo("keyboard"); },
      }
  );
}

void SettingsWindow::onLanguageChanged() { requestContentRebuild(); }
