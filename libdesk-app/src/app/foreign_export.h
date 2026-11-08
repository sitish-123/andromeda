#pragma once

#include <functional>
#include <string>

struct wl_display;
struct wl_surface;
struct zxdg_exporter_v2;
struct zxdg_exported_v2;

namespace desk {

  // Exports a window through xdg-foreign v2, so another client (Quick Look, a portal dialog) can parent a window to
  // it. The handle arrives asynchronously, in the portal form "wayland:<handle>".
  class ForeignExport {
  public:
    explicit ForeignExport(wl_display* display);
    ~ForeignExport();
    ForeignExport(const ForeignExport&) = delete;
    ForeignExport& operator=(const ForeignExport&) = delete;

    // Calls `ready` with the handle, at once when `surface` is already exported, else when the compositor answers.
    // An empty handle means the compositor has no xdg-foreign.
    void handle(wl_surface* surface, std::function<void(const std::string&)> ready);
    void onHandle(const char* handle);

  private:
    zxdg_exporter_v2* m_exporter = nullptr;
    zxdg_exported_v2* m_exported = nullptr;
    wl_surface* m_surface = nullptr;
    std::string m_handle;
    std::function<void(const std::string&)> m_ready;
  };

} // namespace desk
