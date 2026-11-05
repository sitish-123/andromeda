#pragma once

#include <string_view>

struct wl_display;
struct wl_surface;
struct zxdg_importer_v2;
struct zxdg_imported_v2;

namespace desk {

  // Makes a window a child of another client's window through xdg-foreign v2. The compositor floats a parented
  // toplevel and centres it over its parent.
  class ForeignParent {
  public:
    explicit ForeignParent(wl_display* display);
    ~ForeignParent();
    ForeignParent(const ForeignParent&) = delete;
    ForeignParent& operator=(const ForeignParent&) = delete;

    // `handle` is the portal form "wayland:<exported handle>"; any other form leaves the window unparented.
    void setParent(wl_surface* child, std::string_view handle);
    void clear();

  private:
    zxdg_importer_v2* m_importer = nullptr;
    zxdg_imported_v2* m_imported = nullptr;
  };

} // namespace desk
