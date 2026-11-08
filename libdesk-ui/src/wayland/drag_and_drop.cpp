#include "wayland/drag_and_drop.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>

namespace {
  constexpr Logger kLog("dnd");
  constexpr std::uint32_t kUnset = 0xffffffffU;
  constexpr int kReadTimeoutMs = 5000;
  // set_actions and finish are wl_data_device_manager v3.
  bool v3(void* proxy) { return wl_proxy_get_version(static_cast<wl_proxy*>(proxy)) >= 3; }

  DragAndDrop* self(void* data) { return static_cast<DragAndDrop*>(data); }

  const wl_data_source_listener kSourceListener = {
      .target = [](void*, wl_data_source*, const char*) {},
      .send = [](void* data, wl_data_source*, const char* mime, int32_t fd) { self(data)->onSourceSend(mime, fd); },
      .cancelled = [](void* data, wl_data_source*) { self(data)->onSourceDone(false); },
      .dnd_drop_performed = [](void*, wl_data_source*) {},
      .dnd_finished = [](void* data, wl_data_source*) { self(data)->onSourceDone(true); },
      .action = [](void* data, wl_data_source*, uint32_t action) { self(data)->onSourceAction(action); },
  };

  const wl_data_source_listener kSelectionListener = {
      .target = [](void*, wl_data_source*, const char*) {},
      .send = [](void* data, wl_data_source*, const char* mime, int32_t fd) { self(data)->onSelectionSend(mime, fd); },
      .cancelled = [](void* data, wl_data_source*) { self(data)->onSelectionCancelled(); },
      .dnd_drop_performed = [](void*, wl_data_source*) {},
      .dnd_finished = [](void*, wl_data_source*) {},
      .action = [](void*, wl_data_source*, uint32_t) {},
  };

  // Reads a pipe the source writes from its own loop, off the main thread; gives up after kReadTimeoutMs of silence.
  std::string readPipe(int fd) {
    std::string bytes;
    char buf[4096];
    while (true) {
      pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&pfd, 1, kReadTimeoutMs);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        bytes.clear();
        break;
      }
      const ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      bytes.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return bytes;
  }

  const wl_data_offer_listener kOfferListener = {
      .offer = [](void* data, wl_data_offer* offer, const char* mime) { self(data)->onOfferMime(offer, mime); },
      .source_actions = [](void*, wl_data_offer*, uint32_t) {},
      .action =
          [](void* data, wl_data_offer* offer, uint32_t action) { self(data)->onOfferAction(offer, action); },
  };

  const wl_data_device_listener kDeviceListener = {
      .data_offer = [](void* data, wl_data_device*, wl_data_offer* offer) { self(data)->onDataOffer(offer); },
      .enter =
          [](void* data, wl_data_device*, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y,
             wl_data_offer* offer) {
            self(data)->onEnter(serial, surface, static_cast<float>(wl_fixed_to_double(x)),
                                static_cast<float>(wl_fixed_to_double(y)), offer);
          },
      .leave = [](void* data, wl_data_device*) { self(data)->onLeave(); },
      .motion =
          [](void* data, wl_data_device*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
            self(data)->onMotion(static_cast<float>(wl_fixed_to_double(x)), static_cast<float>(wl_fixed_to_double(y)));
          },
      .drop = [](void* data, wl_data_device*) { self(data)->onDrop(); },
      .selection = [](void* data, wl_data_device*, wl_data_offer* offer) { self(data)->onSelection(offer); },
  };

  void writeAll(int fd, const std::string& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        break;
      }
      done += static_cast<std::size_t>(n);
    }
  }
} // namespace

std::unique_ptr<DragAndDrop> DragAndDrop::create(WaylandConnection& connection, wl_data_device_manager* manager) {
  return std::unique_ptr<DragAndDrop>(new DragAndDrop(connection, manager));
}

DragAndDrop::DragAndDrop(WaylandConnection& connection, wl_data_device_manager* manager)
    : m_connection(connection), m_manager(manager) {
  m_device = wl_data_device_manager_get_data_device(manager, connection.seat());
  wl_data_device_add_listener(m_device, &kDeviceListener, this);
}

DragAndDrop::~DragAndDrop() {
  *m_alive = false;
  if (m_source != nullptr) {
    wl_data_source_destroy(m_source);
  }
  if (m_selectionSource != nullptr) {
    wl_data_source_destroy(m_selectionSource);
  }
  for (auto& [offer, mimes] : m_offers) {
    wl_data_offer_destroy(offer);
  }
  if (m_device != nullptr) {
    wl_data_device_release(m_device);
  }
}

bool DragAndDrop::startDrag(wl_surface* origin, std::uint32_t serial, Drag drag) {
  if (m_source != nullptr) {
    onSourceDone(false);
  }
  m_drag = std::move(drag);
  m_sourceAction = None;
  m_source = wl_data_device_manager_create_data_source(m_manager);
  wl_data_source_add_listener(m_source, &kSourceListener, this);
  for (const std::string& mime : m_drag.mimeTypes) {
    wl_data_source_offer(m_source, mime.c_str());
  }
  if (v3(m_source)) {
    wl_data_source_set_actions(m_source, m_drag.actions);
  }
  wl_data_device_start_drag(m_device, m_source, origin, m_drag.icon, serial);
  kLog.debug("drag started with {} types", m_drag.mimeTypes.size());
  return true;
}

void DragAndDrop::onSourceSend(const char* mime, int fd) {
  const std::string bytes = m_drag.data ? m_drag.data(mime) : std::string();
  writeAll(fd, bytes);
  ::close(fd);
}

void DragAndDrop::onSourceDone(bool dropped) {
  if (m_source == nullptr) {
    return;
  }
  wl_data_source_destroy(m_source);
  m_source = nullptr;
  auto finished = std::exchange(m_drag.finished, {});
  m_drag = {};
  kLog.debug("drag {} action {}", dropped ? "dropped" : "cancelled", m_sourceAction);
  if (finished) {
    finished(dropped ? m_sourceAction : None, dropped);
  }
}

void DragAndDrop::setTarget(wl_surface* surface, Target target) { m_targets[surface] = std::move(target); }

void DragAndDrop::clearTarget(wl_surface* surface) {
  m_targets.erase(surface);
  if (m_over == surface) {
    m_over = nullptr;
  }
}

void DragAndDrop::onDataOffer(wl_data_offer* offer) {
  m_offers[offer] = {};
  wl_data_offer_add_listener(offer, &kOfferListener, this);
}

void DragAndDrop::onOfferMime(wl_data_offer* offer, const char* mime) { m_offers[offer].emplace_back(mime); }

void DragAndDrop::releaseOffer(wl_data_offer* offer) {
  if (offer == nullptr) {
    return;
  }
  m_offers.erase(offer);
  wl_data_offer_destroy(offer);
}

void DragAndDrop::onSelection(wl_data_offer* offer) {
  // The new clipboard replaces the old one; null = empty. Our own source comes back as an offer too.
  if (m_selectionOffer != nullptr && m_selectionOffer != offer) {
    releaseOffer(m_selectionOffer);
  }
  m_selectionOffer = offer != m_offer ? offer : nullptr;
}

bool DragAndDrop::setSelection(std::uint32_t serial, Selection selection) {
  if (m_selectionSource != nullptr) {
    wl_data_source_destroy(m_selectionSource);
  }
  m_selection = std::move(selection);
  m_selectionSource = wl_data_device_manager_create_data_source(m_manager);
  wl_data_source_add_listener(m_selectionSource, &kSelectionListener, this);
  for (const std::string& mime : m_selection.mimeTypes) {
    wl_data_source_offer(m_selectionSource, mime.c_str());
  }
  wl_data_device_set_selection(m_device, m_selectionSource, serial);
  wl_display_flush(m_connection.display());
  kLog.debug("selection set with {} types", m_selection.mimeTypes.size());
  return true;
}

void DragAndDrop::onSelectionSend(const char* mime, int fd) {
  writeAll(fd, m_selection.data ? m_selection.data(mime) : std::string());
  ::close(fd);
}

void DragAndDrop::onSelectionCancelled() {
  // Another client took the clipboard.
  if (m_selectionSource != nullptr) {
    wl_data_source_destroy(m_selectionSource);
    m_selectionSource = nullptr;
  }
  m_selection = {};
}

std::vector<std::string> DragAndDrop::selectionTypes() const {
  if (m_selectionOffer == nullptr) {
    return {};
  }
  const auto it = m_offers.find(m_selectionOffer);
  return it != m_offers.end() ? it->second : std::vector<std::string>{};
}

void DragAndDrop::readSelection(const std::vector<std::string>& accepted,
                                std::function<void(const std::string& mime, std::string bytes)> done) {
  const std::vector<std::string> offered = selectionTypes();
  const auto mime = std::ranges::find_if(accepted, [&](const std::string& m) { return std::ranges::contains(offered, m); });
  int fds[2];
  if (mime == accepted.end() || ::pipe2(fds, O_CLOEXEC) != 0) {
    DeferredCall::callLater([done = std::move(done)] { done({}, {}); });
    return;
  }
  wl_data_offer_receive(m_selectionOffer, mime->c_str(), fds[1]);
  ::close(fds[1]);
  wl_display_flush(m_connection.display());
  std::thread([fd = fds[0], mime = *mime, done = std::move(done), alive = std::weak_ptr<bool>(m_alive)]() mutable {
    std::string bytes = readPipe(fd);
    DeferredCall::callLater([mime, done = std::move(done), alive, bytes = std::move(bytes)]() mutable {
      if (!alive.expired()) {
        done(mime, std::move(bytes));
      }
    });
  }).detach();
}

void DragAndDrop::onEnter(std::uint32_t serial, wl_surface* surface, float x, float y, wl_data_offer* offer) {
  if (m_offer != nullptr && m_offer != offer) {
    releaseOffer(m_offer);
  }
  m_offer = offer;
  m_over = surface;
  m_enterSerial = serial;
  m_mime.clear();
  m_action = kUnset; // a new offer: the first motion always answers
  m_offerAction = None;
  if (offer == nullptr) {
    return;
  }
  const auto& mimes = m_offers[offer];
  for (const std::string& want : m_accepted) {
    if (std::ranges::contains(mimes, want)) {
      m_mime = want;
      break;
    }
  }
  auto it = m_targets.find(surface);
  if (it == m_targets.end() || m_mime.empty() || (it->second.enter && !it->second.enter(mimes))) {
    wl_data_offer_accept(offer, serial, nullptr);
    if (v3(offer)) {
      wl_data_offer_set_actions(offer, None, None);
    }
    m_mime.clear();
    return;
  }
  updateAction(x, y);
}

void DragAndDrop::updateAction(float x, float y) {
  m_x = x;
  m_y = y;
  auto it = m_targets.find(m_over);
  if (m_offer == nullptr || m_mime.empty() || it == m_targets.end()) {
    return;
  }
  const std::uint32_t action = it->second.motion ? it->second.motion(x, y) : static_cast<std::uint32_t>(Copy);
  if (action == m_action) {
    return;
  }
  m_action = action;
  wl_data_offer_accept(m_offer, m_enterSerial, action == None ? nullptr : m_mime.c_str());
  if (v3(m_offer)) {
    wl_data_offer_set_actions(m_offer, action == None ? None : (Copy | Move | Ask), action);
  }
}

void DragAndDrop::onMotion(float x, float y) { updateAction(x, y); }

void DragAndDrop::onLeave() {
  if (auto it = m_targets.find(m_over); it != m_targets.end() && it->second.leave) {
    it->second.leave();
  }
  releaseOffer(m_offer);
  m_offer = nullptr;
  m_over = nullptr;
  m_action = kUnset;
}

void DragAndDrop::onDrop() {
  auto it = m_targets.find(m_over);
  if (m_offer == nullptr || m_mime.empty() || m_action == None || m_action == kUnset || it == m_targets.end()) {
    onLeave();
    return;
  }
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    onLeave();
    return;
  }
  wl_data_offer_receive(m_offer, m_mime.c_str(), fds[1]);
  ::close(fds[1]);
  wl_display_flush(m_connection.display());
  wl_data_offer* offer = std::exchange(m_offer, nullptr);
  const float x = m_x;
  const float y = m_y;
  // The compositor's negotiated action (v3) wins over our preference: a source that only copies turns Move into Copy.
  const std::uint32_t preferred = std::exchange(m_action, None);
  const std::uint32_t negotiated = std::exchange(m_offerAction, None);
  const std::uint32_t action = negotiated != None ? negotiated : preferred;
  wl_surface* surface = std::exchange(m_over, nullptr);
  const std::string mime = m_mime;
  // The source writes into the pipe from its own event loop: read off the main thread, deliver on it.
  std::thread([this, fds0 = fds[0], offer, x, y, action, surface, mime, alive = std::weak_ptr<bool>(m_alive)] {
    std::string bytes = readPipe(fds0);
    DeferredCall::callLater([this, offer, x, y, action, surface, mime, alive, bytes = std::move(bytes)]() mutable {
      if (alive.expired()) {
        return;
      }
      if (v3(offer)) {
        if (action == Ask) {
          // finish under Ask is a protocol error; Copy keeps the source's files (the target links or asks).
          wl_data_offer_set_actions(offer, Copy, Copy);
        }
        wl_data_offer_finish(offer);
      }
      releaseOffer(offer);
      if (auto t = m_targets.find(surface); t != m_targets.end() && t->second.drop) {
        t->second.drop(x, y, action, mime, std::move(bytes));
      }
    });
  }).detach();
}
