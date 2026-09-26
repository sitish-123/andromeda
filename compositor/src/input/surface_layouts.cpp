#include "input/surface_layouts.h"

#include "wlr.h"

namespace umbriel {

  SurfaceLayoutMemory::~SurfaceLayoutMemory() { clear(); }

  void SurfaceLayoutMemory::destroyEntry(Entry* entry) {
    wl_list_remove(&entry->destroy.link);
    entry->owner->m_entries.erase(entry->surface);
    delete entry;
  }

  void SurfaceLayoutMemory::onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    Entry* entry = nullptr;
    entry = wl_container_of(listener, entry, destroy);
    destroyEntry(entry);
  }

  void SurfaceLayoutMemory::remember(wlr_surface* surface, std::string_view layout) {
    if (surface == nullptr || layout.empty()) {
      return;
    }
    const auto existing = m_entries.find(surface);
    if (existing != m_entries.end()) {
      existing->second->layout = layout;
      return;
    }
    auto* entry = new Entry{.owner = this, .surface = surface, .layout = std::string(layout), .destroy = {}};
    entry->destroy.notify = onSurfaceDestroy;
    wl_signal_add(&surface->events.destroy, &entry->destroy);
    m_entries.emplace(surface, entry);
  }

  std::optional<std::string_view> SurfaceLayoutMemory::recall(wlr_surface* surface) const {
    if (surface == nullptr) {
      return std::nullopt;
    }
    const auto found = m_entries.find(surface);
    return found == m_entries.end() ? std::nullopt : std::optional<std::string_view>{found->second->layout};
  }

  void SurfaceLayoutMemory::forget(wlr_surface* surface) {
    if (surface == nullptr) {
      return;
    }
    const auto found = m_entries.find(surface);
    if (found != m_entries.end()) {
      destroyEntry(found->second);
    }
  }

  void SurfaceLayoutMemory::clear() {
    while (!m_entries.empty()) {
      destroyEntry(m_entries.begin()->second);
    }
  }

} // namespace umbriel
