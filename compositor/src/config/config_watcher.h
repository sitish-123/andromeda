#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

struct wl_event_loop;
struct wl_event_source;

namespace umbriel {

  class ConfigWatcher {
  public:
    ConfigWatcher(wl_event_loop* loop, std::function<void()> onChange);
    ~ConfigWatcher();

    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    void watch(const std::vector<std::filesystem::path>& files);

  private:
    static int onInotify(int fd, uint32_t mask, void* data);
    static int onTimer(void* data);

    int m_fd = -1;
    wl_event_source* m_fdSource = nullptr;
    wl_event_source* m_timer = nullptr;
    std::function<void()> m_onChange;
    std::unordered_map<int, std::set<std::filesystem::path>> m_dirWatches;
    std::set<std::filesystem::path> m_files;
  };

} // namespace umbriel
