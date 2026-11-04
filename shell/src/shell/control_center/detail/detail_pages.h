#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/detail/detail_page.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

class BluetoothService;
class INetworkService;
class LinkService;
struct LinkDevice;
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

} // namespace control_center
