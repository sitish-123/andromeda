#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/detail/detail_page.h"

#include <chrono>
#include <optional>
#include <string>

class BluetoothService;
class INetworkService;
class Input;
class NotificationManager;
class PipeWireService;
class PowerProfilesService;
class Toggle;
class UPowerService;

namespace control_center {

  class WifiDetailPage final : public DetailPage {
  public:
    explicit WifiDetailPage(INetworkService* network) : m_network(network) {}
    std::unique_ptr<Flex> createHeaderActions() override;
    void setActive(bool active) override;

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;
    void sync() override;
    [[nodiscard]] bool deferRebuild() const override;
    void releaseControls() override;
    [[nodiscard]] bool available() const;

    INetworkService* m_network = nullptr;
    Toggle* m_toggle = nullptr;
    Input* m_password = nullptr;
    bool m_active = false;
    bool m_showAll = false;
    std::string m_passwordSsid;
    std::string m_error;
    std::optional<bool> m_pendingEnabled;
    std::chrono::steady_clock::time_point m_pendingSince;
  };

  class BluetoothDetailPage final : public DetailPage {
  public:
    explicit BluetoothDetailPage(BluetoothService* bluetooth) : m_bluetooth(bluetooth) {}
    std::unique_ptr<Flex> createHeaderActions() override;
    void setActive(bool active) override;
    void onClose() override;

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;
    void sync() override;
    void releaseControls() override { m_toggle = nullptr; }
    void stopScan();
    void startScan();

    BluetoothService* m_bluetooth = nullptr;
    Toggle* m_toggle = nullptr;
    bool m_active = false;
    bool m_startedScan = false; // this page started the running scan
    std::chrono::steady_clock::time_point m_scanAttempt;
    bool m_showNearby = false;
    bool m_showAllPaired = false;
  };

  class SoundDetailPage final : public DetailPage {
  public:
    explicit SoundDetailPage(PipeWireService* audio) : m_audio(audio) {}

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;

    PipeWireService* m_audio = nullptr;
    bool m_showAllOutputs = false;
    bool m_showAllApps = false;
  };

  class PowerDetailPage final : public DetailPage {
  public:
    PowerDetailPage(PowerProfilesService* profiles, UPowerService* upower) : m_profiles(profiles), m_upower(upower) {}

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;

    PowerProfilesService* m_profiles = nullptr;
    UPowerService* m_upower = nullptr;
  };

  // Only Do Not Disturb, with "Until" choices. The timer lives in the shell; a restart drops it.
  class FocusDetailPage final : public DetailPage {
  public:
    explicit FocusDetailPage(NotificationManager* notifications) : m_notifications(notifications) {}
    std::unique_ptr<Flex> createHeaderActions() override;

  private:
    enum class Until : std::uint8_t { None, OneHour, Tomorrow, Manual };

    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;
    void sync() override;
    void releaseControls() override { m_toggle = nullptr; }
    void enable(Until until);
    void disable();

    NotificationManager* m_notifications = nullptr;
    Toggle* m_toggle = nullptr;
    Until m_until = Until::None;
    std::chrono::system_clock::time_point m_endsAt;
    std::uint64_t m_revision = 0; // DND revision right after this page set the timed choice
    Timer m_timer;
  };

} // namespace control_center
