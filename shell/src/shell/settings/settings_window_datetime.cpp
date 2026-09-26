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
  settings::addSettingsDateTime(
      *m_contentContainer,
      settings::SettingsDateTimeContext{
          .scale = scale,
          .timedate = m_timeDateService.get(),
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
          .setLocalRtc =
              [this](bool localRtc) {
                if (m_timeDateService != nullptr) {
                  m_timeDateService->setLocalRtc(localRtc, /*fixSystem=*/true);
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
      }
  );
}

void SettingsWindow::onDateTimeChanged() { requestContentRebuild(); }
