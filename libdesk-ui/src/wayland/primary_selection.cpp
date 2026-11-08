#include "wayland/primary_selection.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "wayland/pipe_io.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <wayland-client.h>

namespace {
  constexpr Logger kLog("primary");
  constexpr std::size_t kMaxTextBytes = 1U << 20U;
  constexpr std::array<const char*, 4> kTextTypes{"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING"};

  PrimarySelection* self(void* data) { return static_cast<PrimarySelection*>(data); }

  const zwp_primary_selection_offer_v1_listener kOfferListener = {
      .offer = [](void* data, zwp_primary_selection_offer_v1* offer, const char* mime) {
        self(data)->onOfferMime(offer, mime);
      },
  };

  const zwp_primary_selection_device_v1_listener kDeviceListener = {
      .data_offer = [](void* data, zwp_primary_selection_device_v1*, zwp_primary_selection_offer_v1* offer) {
        self(data)->onDataOffer(offer);
      },
      .selection = [](void* data, zwp_primary_selection_device_v1*, zwp_primary_selection_offer_v1* offer) {
        self(data)->onSelection(offer);
      },
  };

  const zwp_primary_selection_source_v1_listener kSourceListener = {
      .send = [](void* data, zwp_primary_selection_source_v1*, const char* mime, int32_t fd) {
        self(data)->onSend(mime, fd);
      },
      .cancelled = [](void* data, zwp_primary_selection_source_v1*) { self(data)->onCancelled(); },
  };
} // namespace

PrimarySelection::PrimarySelection(WaylandConnection& connection, zwp_primary_selection_device_manager_v1* manager)
    : m_connection(connection), m_manager(manager) {
  m_device = zwp_primary_selection_device_manager_v1_get_device(m_manager, connection.seat());
  zwp_primary_selection_device_v1_add_listener(m_device, &kDeviceListener, this);
}

PrimarySelection::~PrimarySelection() {
  *m_alive = false;
  for (auto* offer : {m_pending, m_offer}) {
    if (offer != nullptr) {
      zwp_primary_selection_offer_v1_destroy(offer);
    }
  }
  if (m_source != nullptr) {
    zwp_primary_selection_source_v1_destroy(m_source);
  }
  zwp_primary_selection_device_v1_destroy(m_device);
}

void PrimarySelection::onDataOffer(zwp_primary_selection_offer_v1* offer) {
  if (m_pending != nullptr && m_pending != m_offer) {
    zwp_primary_selection_offer_v1_destroy(m_pending);
  }
  m_pending = offer;
  m_pendingMimes.clear();
  zwp_primary_selection_offer_v1_add_listener(offer, &kOfferListener, this);
}

void PrimarySelection::onOfferMime(zwp_primary_selection_offer_v1* offer, const char* mime) {
  if (offer == m_pending) {
    m_pendingMimes.emplace_back(mime);
  }
}

void PrimarySelection::onSelection(zwp_primary_selection_offer_v1* offer) {
  if (m_offer != nullptr && m_offer != offer) {
    zwp_primary_selection_offer_v1_destroy(m_offer);
  }
  m_offer = offer;
  std::vector<std::string> mimes;
  if (offer != nullptr && offer == m_pending) {
    mimes = std::move(m_pendingMimes);
    m_pending = nullptr;
  }
  const unsigned generation = ++m_generation;
  m_reading = false;
  if (m_source != nullptr) {
    m_text = m_ownText; // our own selection comes back as an offer
    finishRead();
    return;
  }
  m_text.reset();
  const auto mime = std::ranges::find_if(kTextTypes, [&](const char* t) { return std::ranges::contains(mimes, t); });
  int fds[2];
  if (offer == nullptr || mime == kTextTypes.end() || ::pipe2(fds, O_CLOEXEC) != 0) {
    finishRead();
    return;
  }
  m_reading = true;
  zwp_primary_selection_offer_v1_receive(offer, *mime, fds[1]);
  ::close(fds[1]);
  wl_display_flush(m_connection.display());
  std::thread([this, fd = fds[0], generation, alive = std::weak_ptr<bool>(m_alive)] {
    std::string bytes = pipe_io::readPipe(fd, kMaxTextBytes);
    DeferredCall::callLater([this, generation, alive, bytes = std::move(bytes)]() mutable {
      if (alive.expired() || generation != m_generation) {
        return; // gone, or a newer selection is being read
      }
      if (!bytes.empty()) {
        m_text = std::move(bytes);
      }
      m_reading = false;
      finishRead();
    });
  }).detach();
}

void PrimarySelection::whenText(std::function<void(std::optional<std::string>)> done) {
  if (m_reading) {
    m_waiters.push_back(std::move(done));
  } else {
    done(m_text);
  }
}

void PrimarySelection::finishRead() {
  for (auto& done : std::exchange(m_waiters, {})) {
    done(m_text);
  }
}

void PrimarySelection::setText(std::uint32_t serial, std::string text) {
  if (m_source != nullptr && text == m_ownText) {
    return;
  }
  if (m_source != nullptr) {
    zwp_primary_selection_source_v1_destroy(m_source);
  }
  m_ownText = std::move(text);
  m_text = m_ownText;
  m_reading = false;
  finishRead();
  m_source = zwp_primary_selection_device_manager_v1_create_source(m_manager);
  zwp_primary_selection_source_v1_add_listener(m_source, &kSourceListener, this);
  for (const char* mime : kTextTypes) {
    zwp_primary_selection_source_v1_offer(m_source, mime);
  }
  zwp_primary_selection_device_v1_set_selection(m_device, m_source, serial);
  wl_display_flush(m_connection.display());
  kLog.debug("primary selection set, {} bytes", m_ownText.size());
}

void PrimarySelection::onSend(const char* /*mime*/, int fd) {
  pipe_io::writeAll(fd, m_ownText);
  ::close(fd);
}

void PrimarySelection::onCancelled() {
  if (m_source != nullptr) {
    zwp_primary_selection_source_v1_destroy(m_source);
    m_source = nullptr;
  }
  m_ownText.clear();
  m_text.reset(); // another client took the selection; its offer brings the new text
}
