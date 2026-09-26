#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
} // namespace sdbus

// Client of org.freedesktop.timedate1, bound only while the Date & Time settings page is open. Timezone, NTP, and
// LocalRTC are read on bind and refreshed on PropertiesChanged; NTPSynchronized and TimeUSec have no change signal
// (native-apis.md), so they are read on bind and after every action.
class TimeDateService {
public:
  using ChangeCallback = std::function<void()>;

  explicit TimeDateService(SystemBus& bus, ChangeCallback onChange = {});
  ~TimeDateService();

  TimeDateService(const TimeDateService&) = delete;
  TimeDateService& operator=(const TimeDateService&) = delete;

  [[nodiscard]] bool ready() const noexcept { return m_ready; }
  [[nodiscard]] const std::string& timezone() const noexcept { return m_timezone; }
  [[nodiscard]] bool ntp() const noexcept { return m_ntp; }
  [[nodiscard]] bool localRtc() const noexcept { return m_localRtc; }
  [[nodiscard]] bool ntpSynchronized() const noexcept { return m_ntpSynchronized; }
  [[nodiscard]] std::int64_t timeUsec() const noexcept { return m_timeUsec; }
  [[nodiscard]] const std::vector<std::string>& timezones() const noexcept { return m_timezones; }
  [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

  void setTimezone(const std::string& zone);
  void setNtp(bool useNtp);
  // usecUtc is absolute; only meaningful while NTP is off.
  void setTime(std::int64_t usecUtc);
  void setLocalRtc(bool localRtc, bool fixSystem);

private:
  void refreshProperties();

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_proxy;
  ChangeCallback m_onChange;
  std::string m_timezone;
  bool m_ntp = false;
  bool m_localRtc = false;
  bool m_ntpSynchronized = false;
  std::int64_t m_timeUsec = 0;
  std::vector<std::string> m_timezones;
  std::string m_lastError;
  bool m_ready = false;
};
