#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <sdbus-c++/Types.h>
#include <string>
#include <string_view>
#include <vector>

struct LinkDevice {
  std::string id;
  std::string name;
  bool connected = false;
};

// What a connected phone reports about itself.
struct LinkStatus {
  std::uint32_t battery = 0;
  bool charging = false;
  // wifi, cellular, ethernet, none, or other.
  std::string network;
  // Cellular bars, 0 to 4; -1 without service.
  std::int32_t signal = -1;
};

struct LinkCallStatus {
  std::string deviceId;
  std::string caller;
  // The phone's name, after the app for an app call: "Messenger · OnePlus 12".
  std::string device;
  bool ringing = false;
  // When the call was answered; unset while it rings.
  std::optional<std::chrono::steady_clock::time_point> since;
};

// The org.andromeda.Link1 state the phone row and the call card read: paired phones, their status, grants and
// notification counts, and the call. LinkService fills it in the shell, LinkLockClient in `noctalia lock`.
class LinkPhoneState {
public:
  virtual ~LinkPhoneState() = default;

  [[nodiscard]] const std::vector<LinkDevice>& devices() const noexcept { return m_devices; }
  // The device's status while it is connected and has reported one.
  [[nodiscard]] const LinkStatus* status(const std::string& deviceId) const;
  [[nodiscard]] bool granted(const std::string& deviceId, std::string_view feature) const;
  // The phone notifications on show, those held while locked included.
  [[nodiscard]] std::uint32_t notificationCount(const std::string& deviceId) const;
  // The phone call that is ringing or active; empty when none.
  [[nodiscard]] std::optional<LinkCallStatus> activeCall() const { return m_call; }
  virtual void declineCall(const std::string& deviceId) = 0;

protected:
  LinkPhoneState() = default;
  LinkPhoneState(const LinkPhoneState&) = delete;
  LinkPhoneState& operator=(const LinkPhoneState&) = delete;

  // Devices, DeviceStatus, Grants and NotificationCounts, from GetAll or PropertiesChanged.
  void applyPhoneProperties(const std::map<std::string, sdbus::Variant>& properties);
  void clearPhoneState();
  // A Call signal: ringing or active sets the call, any other state ends that phone's call.
  void applyCall(
      const std::string& deviceId, const std::string& state, const std::string& number, const std::string& name,
      const std::string& app
  );
  [[nodiscard]] virtual std::string deviceName(const std::string& deviceId) const;

  std::vector<LinkDevice> m_devices;
  std::map<std::string, LinkStatus> m_status;
  std::map<std::string, std::vector<std::string>> m_grants;
  std::map<std::string, std::uint32_t> m_notificationCounts;
  std::optional<LinkCallStatus> m_call;
};
