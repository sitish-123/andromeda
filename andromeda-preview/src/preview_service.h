#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>

namespace preview {

  // org.gnome.NautilusPreviewer2 on /org/gnome/NautilusPreviewer (the GNOME Sushi interface), so the space bar
  // in Nautilus, our apps and the launcher all open the same preview (docs/search.md §5).
  class PreviewService {
  public:
    struct Handlers {
      std::function<void(const std::string& uri, const std::string& windowHandle, bool closeIfShown)> showFile;
      std::function<void()> close;
    };

    PreviewService(sdbus::IConnection& connection, Handlers handlers);
    ~PreviewService();
    PreviewService(const PreviewService&) = delete;
    PreviewService& operator=(const PreviewService&) = delete;

    // Owns org.gnome.NautilusPreviewer; false when another previewer holds it.
    bool start();

    // Arrow keys in the preview: the caller moves its selection and calls ShowFile again.
    void emitSelectionEvent(std::int32_t direction);

  private:
    sdbus::IConnection& m_connection;
    Handlers m_handlers;
    std::unique_ptr<sdbus::IObject> m_object;
  };

  // Keyboard directions for SelectionEvent (GtkDirectionType values, as Sushi sends them).
  namespace direction {
    inline constexpr std::int32_t Up = 2;
    inline constexpr std::int32_t Down = 3;
    inline constexpr std::int32_t Left = 4;
    inline constexpr std::int32_t Right = 5;
  } // namespace direction

} // namespace preview
