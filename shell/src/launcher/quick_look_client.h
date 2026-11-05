#pragma once

#include <memory>
#include <string>

class SessionBus;

namespace sdbus {
  class IProxy;
}

// Opens Quick Look (andromeda-preview, org.gnome.NautilusPreviewer2; docs/search.md §5) for a launcher result.
// D-Bus activation starts the previewer on the first call. Calls are async; a failure is logged, not shown.
class QuickLookClient {
public:
  explicit QuickLookClient(SessionBus& bus);
  ~QuickLookClient();
  QuickLookClient(const QuickLookClient&) = delete;
  QuickLookClient& operator=(const QuickLookClient&) = delete;

  // Shows a local file. A second call for the shown file closes the preview (Ctrl+Y toggles, as Spotlight Cmd+Y).
  void toggle(const std::string& path);
  void close();

private:
  sdbus::IProxy& proxy();

  SessionBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_proxy;
};
