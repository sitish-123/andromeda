#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_datetime.h"
#include "shell/settings/settings_window.h"

#include <ctime>

namespace {

  // Parses "YYYY-MM-DD HH:MM:SS" as UTC; returns nullopt on malformed input.
  std::optional<std::int64_t> parseUtcTime(const std::string& text) {
    std::tm tm{};
    if (strptime(text.c_str(), "%Y-%m-%d %H:%M:%S", &tm) == nullptr) {
      return std::nullopt;
    }
    const std::time_t seconds = timegm(&tm);
    if (seconds < 0) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(seconds) * 1'000'000;
  }

} // namespace

void SettingsWindow::addDateTimeContent(float scale) {
  if (m_selectedSection != "date-time") {
    m_timeDateService.reset();
    return;
  }
  if (m_timeDateService == nullptr && m_systemBus != nullptr) {
    m_timeDateService = std::make_unique<TimeDateService>(*m_systemBus, [this]() { onDateTimeChanged(); });
  }
  std::vector<settings::SettingsDateTimeContext::Account> accounts;
  if (m_config != nullptr) {
    using Action = CalendarService::AccountStatus::Action;
    for (const CalendarConfig::Account& account : m_config->config().calendar.accounts) {
      const Action action =
          m_calendarService != nullptr ? m_calendarService->accountStatus(account).action : Action{};
      accounts.push_back({
          .name = account.displayName.empty() ? account.id : account.displayName,
          .status = m_calendarService != nullptr ? m_calendarService->accountStatusText(account)
                                                 : i18n::tr("calendar.sync.not-synced"),
          .open =
              [this, id = account.id, action] {
                if (action == Action::Retry && m_calendarService != nullptr) {
                  m_calendarService->requestRefresh();
                  return;
                }
                openCalendarAccountEditor(id);
              },
      });
    }
  }
  settings::addSettingsDateTime(
      *m_contentContainer,
      settings::SettingsDateTimeContext{
          .scale = scale,
          .timedate = m_timeDateService.get(),
          .config = m_config != nullptr ? &m_config->config() : nullptr,
          .setTimezone =
              [this](std::string zone) {
                if (m_timeDateService != nullptr) {
                  m_timeDateService->setTimezone(zone);
                }
              },
          .setNtp =
              [this](bool useNtp) {
                if (m_timeDateService != nullptr) {
                  m_timeDateService->setNtp(useNtp);
                }
              },
          .setTimeText =
              [this](std::string text) {
                if (m_timeDateService == nullptr) {
                  return;
                }
                if (const auto usec = parseUtcTime(text)) {
                  m_timeDateService->setTime(*usec);
                }
              },
          .setOverride = [this](std::vector<std::string> path,
                                ConfigOverrideValue value) { setSettingOverride(std::move(path), std::move(value)); },
          .accounts = std::move(accounts),
          .addAccount = [this] { openCalendarAccountEditor(std::nullopt); },
          .openRegion = [this] { navigateTo("language"); },
      }
  );
}

void SettingsWindow::onDateTimeChanged() { requestContentRebuild(); }
