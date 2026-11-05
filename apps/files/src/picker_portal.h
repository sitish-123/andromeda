#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SessionBus;

namespace sdbus {
  class IObject;
}

// org.freedesktop.impl.portal.FileChooser on org.freedesktop.impl.portal.desktop.andromeda.Files, served by
// `andromeda-files --picker`. Each call stays pending until the window answers it or the portal closes its Request.
class PickerPortal {
public:
  struct Filter {
    std::string name;
    std::vector<std::pair<std::uint32_t, std::string>> patterns; // 0 = glob, 1 = MIME type
  };

  struct Request {
    enum class Kind : std::uint8_t { Open, Save, SaveFiles };
    Kind kind = Kind::Open;
    std::string handle;
    std::string appId;
    std::string parentWindow; // "wayland:<handle>", "x11:<xid>" or ""
    std::string title;
    std::string acceptLabel;
    bool multiple = false;
    bool directory = false;
    std::vector<Filter> filters;
    std::optional<std::size_t> currentFilter;
    std::string currentName;
    std::filesystem::path currentFolder;
    std::filesystem::path currentFile;
    std::vector<std::filesystem::path> files; // SaveFiles: names to save into the chosen folder
  };

  struct Answer {
    std::uint32_t response = 1; // 0 success, 1 cancelled, 2 other
    std::vector<std::string> uris;
    std::optional<std::size_t> filter;
  };

  using RequestHandler = std::function<void(std::uint64_t id, const Request& request)>;
  using CloseHandler = std::function<void(std::uint64_t id)>;

  PickerPortal(SessionBus& bus, RequestHandler onRequest, CloseHandler onClose);
  ~PickerPortal();

  PickerPortal(const PickerPortal&) = delete;
  PickerPortal& operator=(const PickerPortal&) = delete;

  void answer(std::uint64_t id, const Answer& answer);
  [[nodiscard]] bool idle() const noexcept { return m_pending.empty(); }

  // Matches a name against one portal filter (globs case-insensitive, MIME types through GIO).
  [[nodiscard]] static bool matches(const Filter& filter, const std::string& name, const std::string& contentType);

private:
  struct Pending;

  SessionBus& m_bus;
  RequestHandler m_onRequest;
  CloseHandler m_onClose;
  std::unique_ptr<sdbus::IObject> m_object;
  std::map<std::uint64_t, std::unique_ptr<Pending>> m_pending;
  std::uint64_t m_nextId = 1;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
