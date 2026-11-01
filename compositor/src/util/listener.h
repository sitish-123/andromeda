#pragma once

#include <functional>
#include <utility>

#include <wayland-server-core.h>

namespace andromeda {

  // Owns a wl_listener and its callback. The destructor disconnects. The object is not movable because the wl_list
  // node address must stay fixed. disconnect() is safe inside the callback when the signal is emitted with
  // wl_signal_emit_mutable. Destroying the Listener inside its own callback is safe if the callback then touches no
  // capture.
  class Listener {
  public:
    Listener() { m_listener.link.next = nullptr; }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    ~Listener() { disconnect(); }

    void connect(wl_signal* signal, std::function<void(void*)> fn) {
      disconnect();
      m_fn = std::move(fn);
      m_listener.notify = &Listener::trampoline;
      wl_signal_add(signal, &m_listener);
    }

    // Calls handler(self, data) on emit.
    template <class T> void connect(wl_signal* signal, T* self, void (*handler)(T*, void*)) {
      connect(signal, [self, handler](void* data) { handler(self, data); });
    }

    // The signal is the destroy signal of the client.
    void connectClient(wl_client* client, std::function<void(void*)> fn) {
      disconnect();
      m_fn = std::move(fn);
      m_listener.notify = &Listener::trampoline;
      wl_client_add_destroy_listener(client, &m_listener);
    }

    void disconnect() {
      if (m_listener.link.next != nullptr) {
        wl_list_remove(&m_listener.link);
        m_listener.link.next = nullptr;
        m_listener.link.prev = nullptr;
      }
    }

    bool connected() const { return m_listener.link.next != nullptr; }

  private:
    static void trampoline(wl_listener* listener, void* data) {
      Listener* self = wl_container_of(listener, self, m_listener);
      self->m_fn(data);
    }

    wl_listener m_listener{};
    std::function<void(void*)> m_fn;
  };

}
