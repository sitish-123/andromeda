#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

struct wl_event_loop;
struct wlr_buffer;

namespace andromeda {
  class MainLoopWorker;
}

namespace andromeda::ov {

  // App icons for the overview's title capsules, by app id: the .desktop file's Icon= (or the app id itself) looked up
  // in the GTK icon theme, Adwaita and hicolor, drawn on a worker thread and kept for the session.
  class AppIcons {
  public:
    // `changed` runs on the event loop when an icon finished loading.
    AppIcons(wl_event_loop* loop, std::function<void()> changed);
    ~AppIcons();
    AppIcons(const AppIcons&) = delete;
    AppIcons& operator=(const AppIcons&) = delete;

    // The icon at `px` device pixels square, or null while it loads or when the app has none. The buffer stays owned
    // by this cache.
    [[nodiscard]] wlr_buffer* get(std::string_view appId, int px);

  private:
    // "<app id>@<px>" -> icon; null while loading or when the app has none.
    std::unordered_map<std::string, wlr_buffer*> m_entries;
    std::function<void()> m_changed;
    std::unique_ptr<MainLoopWorker> m_worker; // last: joined before the entries its results write go away
  };

} // namespace andromeda::ov
