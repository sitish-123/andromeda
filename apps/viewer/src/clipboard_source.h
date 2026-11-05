#pragma once

#include <string>

class WaylandConnection;
struct wl_data_device;
struct wl_data_device_manager;
struct wl_data_source;

namespace viewer {

  // Owns the clipboard selection for copied text (wl_data_device). libdesk-app has no clipboard yet.
  class ClipboardSource {
  public:
    explicit ClipboardSource(WaylandConnection& wayland);
    ~ClipboardSource();
    ClipboardSource(const ClipboardSource&) = delete;
    ClipboardSource& operator=(const ClipboardSource&) = delete;

    // Offers `text` as the selection; uses the serial of the last input event. False without a data device.
    bool setText(std::string text);

  private:
    static void onSend(void* data, wl_data_source* source, const char* mime, int fd);
    static void onCancelled(void* data, wl_data_source* source);

    WaylandConnection& m_wayland;
    wl_data_device_manager* m_manager = nullptr;
    wl_data_device* m_device = nullptr;
    wl_data_source* m_source = nullptr;
    std::string m_text;
  };

} // namespace viewer
