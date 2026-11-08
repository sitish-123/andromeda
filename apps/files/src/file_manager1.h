#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IObject;
}

// org.freedesktop.FileManager1 (FB-BASICS): other apps ask Files to show folders, reveal items or show their
// properties (Firefox "Show in Folder", Chromium downloads, portals' OpenDirectory).
class FileManager1 {
public:
  enum class Show : std::uint8_t { Folders, Items, Properties };
  using Handler = std::function<void(Show show, const std::vector<std::string>& uris, const std::string& startupId)>;

  FileManager1(SessionBus& bus, Handler handler);
  ~FileManager1();
  FileManager1(const FileManager1&) = delete;
  FileManager1& operator=(const FileManager1&) = delete;

  [[nodiscard]] bool owned() const noexcept { return m_owned; } // false: another file manager has the name

private:
  SessionBus& m_bus;
  Handler m_handler;
  std::unique_ptr<sdbus::IObject> m_object;
  bool m_owned = false;
};
