#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class ClipboardService;
class IpcService;
class NotificationManager;
class SessionBus;

namespace sdbus {
  class IProxy;
  class Variant;
} // namespace sdbus

// Client of umbriel-linkd's org.umbriel.Link1.QuickShare: the visibility switch, and a notification for every offer
// (Accept, Decline) and every result (Open, Show in folder, Copy).
class QuickShareService {
public:
  using ChangeCallback = std::function<void()>;

  QuickShareService(SessionBus& bus, NotificationManager& notifications, ClipboardService& clipboard);
  ~QuickShareService();

  QuickShareService(const QuickShareService&) = delete;
  QuickShareService& operator=(const QuickShareService&) = delete;

  void setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }
  void setVisible(bool visible);
  void registerIpc(IpcService& ipc);

  [[nodiscard]] bool available() const noexcept { return m_available; }
  [[nodiscard]] bool visible() const noexcept { return m_visible; }
  // The name nearby devices see.
  [[nodiscard]] const std::string& name() const noexcept { return m_name; }

private:
  struct Pending {
    std::string sender;
  };
  struct Result {
    std::vector<std::string> files;
    std::string text;
    bool link = false;
  };

  void refresh();
  void apply(const std::map<std::string, sdbus::Variant>& properties);
  void onOffer(
      std::uint64_t id, const std::string& sender, const std::string& pin,
      const std::vector<std::pair<std::string, std::int64_t>>& files, const std::vector<std::string>& texts
  );
  void onFinished(
      std::uint64_t id, const std::string& status, const std::vector<std::string>& files,
      const std::vector<std::pair<std::string, std::string>>& texts, const std::string& error
  );
  void onAction(std::uint32_t notification, const std::string& action, const std::string& activationToken);
  void call(const std::string& method, std::uint64_t id);
  void notify();

  NotificationManager& m_notifications;
  ClipboardService& m_clipboard;
  std::unique_ptr<sdbus::IProxy> m_bus;
  std::unique_ptr<sdbus::IProxy> m_proxy;
  std::unique_ptr<sdbus::IProxy> m_fileManager;
  ChangeCallback m_changeCallback;
  // Keyed by the offer id, then by the notification that shows it.
  std::map<std::uint64_t, Pending> m_offers;
  std::map<std::uint32_t, std::uint64_t> m_offerNotifications;
  std::map<std::uint32_t, Result> m_results;
  std::string m_name;
  bool m_available = false;
  bool m_visible = false;
};
