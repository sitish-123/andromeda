#include "clipboard_source.h"

#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>

namespace viewer {

  namespace {
    void onGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
      if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        *static_cast<wl_data_device_manager**>(data) = static_cast<wl_data_device_manager*>(
            wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3U)));
      }
    }
    void onGlobalRemove(void*, wl_registry*, std::uint32_t) {}
    constexpr wl_registry_listener kRegistryListener{.global = onGlobal, .global_remove = onGlobalRemove};

    // The text goes out on the compositor's pipe; a reader that closes early gets the rest dropped.
    void writeAll(int fd, const std::string& text) {
      std::size_t done = 0;
      while (done < text.size()) {
        const ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          return;
        }
        done += static_cast<std::size_t>(n);
      }
    }
  } // namespace

  ClipboardSource::ClipboardSource(WaylandConnection& wayland) : m_wayland(wayland) {
    wl_registry* registry = wl_display_get_registry(wayland.display());
    wl_registry_add_listener(registry, &kRegistryListener, &m_manager);
    wl_display_roundtrip(wayland.display());
    wl_registry_destroy(registry);
    if (m_manager != nullptr && wayland.seat() != nullptr) {
      m_device = wl_data_device_manager_get_data_device(m_manager, wayland.seat());
    }
  }

  ClipboardSource::~ClipboardSource() {
    if (m_source != nullptr) {
      wl_data_source_destroy(m_source);
    }
    if (m_device != nullptr) {
      wl_data_device_release(m_device);
    }
    if (m_manager != nullptr) {
      wl_data_device_manager_destroy(m_manager);
    }
  }

  bool ClipboardSource::setText(std::string text) {
    if (m_device == nullptr) {
      return false;
    }
    if (m_source != nullptr) {
      wl_data_source_destroy(m_source);
    }
    m_text = std::move(text);
    m_source = wl_data_device_manager_create_data_source(m_manager);
    static constexpr wl_data_source_listener kListener{
        .target = [](void*, wl_data_source*, const char*) {},
        .send = &ClipboardSource::onSend,
        .cancelled = &ClipboardSource::onCancelled,
        .dnd_drop_performed = [](void*, wl_data_source*) {},
        .dnd_finished = [](void*, wl_data_source*) {},
        .action = [](void*, wl_data_source*, std::uint32_t) {},
    };
    wl_data_source_add_listener(m_source, &kListener, this);
    for (const char* mime : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"}) {
      wl_data_source_offer(m_source, mime);
    }
    wl_data_device_set_selection(m_device, m_source, m_wayland.lastInputSerial());
    wl_display_flush(m_wayland.display());
    return true;
  }

  void ClipboardSource::onSend(void* data, wl_data_source*, const char*, int fd) {
    // Off the main thread: a reader that is slow, stuck, or this app itself must not block the event loop.
    std::thread([fd, text = static_cast<ClipboardSource*>(data)->m_text] {
      writeAll(fd, text);
      close(fd);
    }).detach();
  }

  void ClipboardSource::onCancelled(void* data, wl_data_source* source) {
    auto* self = static_cast<ClipboardSource*>(data);
    if (self->m_source == source) {
      self->m_source = nullptr;
    }
    wl_data_source_destroy(source);
  }

} // namespace viewer
