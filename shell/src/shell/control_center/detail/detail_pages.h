#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/detail/detail_page.h"

#include <chrono>
#include <functional>
#include <optional>
#include <set>
#include <unordered_set>
#include <string>

class BluetoothService;
class BluetoothAgent;
class BrightnessService;
class SystemMonitorService;
class ScreenTimeService;
class MprisService;
class CompositorPlatform;
class HttpClient;
class Image;
class IpcService;
class Shortcut;
struct ShortcutServices;
class INetworkService;
class LinkService;
struct LinkDevice;
class EasyEffectsService;
class Input;
class NmHotspot;
class NotificationManager;
class PipeWireService;
class PowerProfilesService;
class Toggle;
class UPowerService;

namespace control_center {

  class WifiDetailPage final : public DetailPage {
  public:
    WifiDetailPage(INetworkService* network, NmHotspot* hotspot) : m_network(network), m_hotspot(hotspot) {}
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
    void buildWireless();
    // Ethernet and Personal Hotspot: shown whether Wi-Fi is on or off.
    void addLinks();

    INetworkService* m_network = nullptr;
    NmHotspot* m_hotspot = nullptr;
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
    BluetoothDetailPage(BluetoothService* bluetooth, BluetoothAgent* agent) : m_bluetooth(bluetooth), m_agent(agent) {}
    std::unique_ptr<Flex> createHeaderActions() override;
    void setActive(bool active) override;
    void onClose() override;

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;
    void sync() override;
    void releaseControls() override {
      m_toggle = nullptr;
      m_code = nullptr;
    }
    [[nodiscard]] bool deferRebuild() const override;
    void addPairingPrompt();
    void stopScan();
    void startScan();

    BluetoothService* m_bluetooth = nullptr;
    BluetoothAgent* m_agent = nullptr;
    Toggle* m_toggle = nullptr;
    Input* m_code = nullptr; // PIN or passkey the device asks for
    bool m_active = false;
    bool m_startedScan = false; // this page started the running scan
    std::chrono::steady_clock::time_point m_scanAttempt;
    bool m_showNearby = false;
    bool m_showAllPaired = false;
    std::string m_error;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0); // guards async BlueZ replies
  };

  class SoundDetailPage final : public DetailPage {
  public:
    SoundDetailPage(PipeWireService* audio, EasyEffectsService* effects) : m_audio(audio), m_effects(effects) {}
    void setActive(bool active) override;

  private:
    // The EasyEffects profile list that is open inline.
    enum class EffectsList : std::uint8_t { None, Output, Input };

    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;
    void addEffectsRows(Flex& card, EffectsList list);

    PipeWireService* m_audio = nullptr;
    EasyEffectsService* m_effects = nullptr;
    EffectsList m_openEffects = EffectsList::None;
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

  // `/` on the quick sheet: modules with a detail page and Wi-Fi networks that match the typed text.
  class SearchDetailPage final : public DetailPage {
  public:
    SearchDetailPage(INetworkService* network, std::function<void(std::string_view)> openTab)
        : m_network(network), m_openTab(std::move(openTab)) {}
    void setActive(bool active) override;

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override { return {}; }
    std::unique_ptr<Node> createTop() override;
    void sync() override;
    void releaseControls() override { m_input = nullptr; }

    INetworkService* m_network = nullptr;
    std::function<void(std::string_view)> m_openTab;
    Input* m_input = nullptr;
    std::string m_query;
    bool m_focusPending = false;
    std::function<void()> m_firstResult;
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

  // The paired phone (Link): status chips, actions and its latest notifications. Link has no connect call, so the
  // page shows the state and cannot change it.
  class PhoneDetailPage final : public DetailPage {
  public:
    PhoneDetailPage(LinkService* link, NotificationManager* notifications)
        : m_link(link), m_notifications(notifications) {}
    [[nodiscard]] std::string headerTitle() const override;
    std::unique_ptr<Flex> createHeaderActions() override;
    void setActive(bool active) override;

  private:
    struct PhoneNotification {
      std::uint32_t id = 0;
      std::string title;
      std::string body;
      bool canReply = false;
      std::shared_ptr<const NotificationImageData> image;
      std::vector<std::string> icons;
    };

    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;
    [[nodiscard]] bool deferRebuild() const override;
    void releaseControls() override {
      m_reply = nullptr;
      m_toggle = nullptr;
    }
    void sync() override;
    [[nodiscard]] std::string statusText(const LinkDevice& phone) const;
    void showMessage(std::string text);
    [[nodiscard]] const LinkDevice* device() const;
    [[nodiscard]] std::vector<PhoneNotification> phoneNotifications(const std::string& deviceId) const;
    void addReplyForm(Flex& card, std::uint32_t id);

    LinkService* m_link = nullptr;
    NotificationManager* m_notifications = nullptr;
    Input* m_reply = nullptr;
    Toggle* m_toggle = nullptr;
    std::uint32_t m_replyId = 0;
    bool m_active = false;
    bool m_photosAsked = false;
    std::size_t m_thumbsReady = 0;
    std::string m_message;
    Timer m_messageTimer;
    // The toggle position the user chose, held until Paused follows it, the call fails, or the time runs out.
    std::optional<bool> m_pendingOn;
    std::chrono::steady_clock::time_point m_pendingSince;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  };

  // Brightness per output, Dark Mode and Night Light (.agent/DESIGN.md "Control Center pages", Display).
  class DisplayDetailPage final : public DetailPage {
  public:
    DisplayDetailPage(BrightnessService* brightness, const ShortcutServices& shortcuts);
    ~DisplayDetailPage() override;

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;

    BrightnessService* m_brightness = nullptr;
    std::unique_ptr<Shortcut> m_darkMode;
    std::unique_ptr<Shortcut> m_nightLight;
  };

  // System B "Rings + rates" (.agent/DESIGN.md "Control Center pages"): CPU, Memory, GPU rings, then disk, network and
  // temperature rows. Samples once a second only while the page is shown.
  class SystemDetailPage final : public DetailPage {
  public:
    SystemDetailPage(SystemMonitorService* monitor, IpcService* ipc) : m_monitor(monitor), m_ipc(ipc) {}
    ~SystemDetailPage() override;
    void setActive(bool active) override;

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;
    void retain(bool on);

    SystemMonitorService* m_monitor = nullptr;
    IpcService* m_ipc = nullptr;
    bool m_retained = false;
    Timer m_tick;
  };

  // Screen Time A "Week + most used" (.agent/DESIGN.md "Control Center pages").
  class ScreenTimeDetailPage final : public DetailPage {
  public:
    explicit ScreenTimeDetailPage(ScreenTimeService* screenTime) : m_screenTime(screenTime) {}
    void setActive(bool active) override;

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;

    ScreenTimeService* m_screenTime = nullptr;
    Timer m_tick;
  };

  // Media (rulebook 9.3 "Media"): now-playing card with seek line and controls, then the players list when 2 or more.
  class MediaDetailPage final : public DetailPage {
  public:
    MediaDetailPage(MprisService* mpris, HttpClient* http) : m_mpris(mpris), m_http(http) {}
    void setActive(bool active) override;

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override { return {}; }

    MprisService* m_mpris = nullptr;
    HttpClient* m_http = nullptr;
    std::unordered_set<std::string> m_pendingArt;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
    Timer m_tick;
  };

  // Notification history (rulebook 9.11 notification center in the sheet): groups by app, newest first, 2 rows per
  // group until "Show all", Clear per group and Clear All.
  class NotificationsDetailPage final : public DetailPage {
  public:
    NotificationsDetailPage(NotificationManager* notifications, CompositorPlatform* platform)
        : m_notifications(notifications), m_platform(platform) {}
    void setActive(bool active) override;

  private:
    [[nodiscard]] std::string structureKey() const override;
    void build() override;
    [[nodiscard]] std::vector<FooterButton> footerButtons() override;

    NotificationManager* m_notifications = nullptr;
    CompositorPlatform* m_platform = nullptr;
    std::set<std::string> m_expanded;
  };

} // namespace control_center
