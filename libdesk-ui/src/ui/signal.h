#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// A minimal signal/slot primitive for reactive UI updates.
//
// Callbacks are stored in a shared list so iteration during emit() is safe
// even if a callback disconnects its own (or another) connection: a snapshot
// of live slots is captured before dispatch.
//
// ScopedConnection is move-only RAII — it disconnects on destruction. Store
// one as a member of whatever object subscribes, and the subscription dies
// with the object.

template <class... Args> class Signal {
public:
  using Callback = std::function<void(Args...)>;

private:
  struct Slot {
    std::uint64_t id;
    Callback callback;
  };
  struct State {
    std::vector<Slot> slots;
    std::uint64_t nextId = 0;
    std::size_t dead = 0;
    void reap() {
      slots.erase(std::remove_if(slots.begin(), slots.end(), [](const Slot& s) { return !s.callback; }), slots.end());
      dead = 0;
    }
  };

public:
  class ScopedConnection {
  public:
    ScopedConnection() = default;
    ScopedConnection(std::weak_ptr<State> state, std::uint64_t id) : m_state(std::move(state)), m_id(id) {}
    ~ScopedConnection() { disconnect(); }

    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    ScopedConnection(ScopedConnection&& other) noexcept : m_state(std::move(other.m_state)), m_id(other.m_id) {
      other.m_id = 0;
    }
    ScopedConnection& operator=(ScopedConnection&& other) noexcept {
      if (this != &other) {
        disconnect();
        m_state = std::move(other.m_state);
        m_id = other.m_id;
        other.m_id = 0;
      }
      return *this;
    }

    void disconnect() {
      if (m_id == 0) {
        return;
      }
      if (auto state = m_state.lock()) {
        // Slots stay sorted by id (connect appends increasing ids): find by binary search, leave a tombstone and
        // compact in batches, so tearing down thousands of subscribed nodes stays linear, not quadratic.
        auto& slots = state->slots;
        const auto it = std::lower_bound(slots.begin(), slots.end(), m_id,
                                         [](const Slot& slot, std::uint64_t id) { return slot.id < id; });
        if (it != slots.end() && it->id == m_id && it->callback) {
          // Moved out first: its captures may disconnect from this signal (and reap) when they are destroyed.
          Callback dead;
          dead.swap(it->callback);
          if (++state->dead > 32 && state->dead * 2 > slots.size()) {
            state->reap();
          }
        }
      }
      m_id = 0;
    }

  private:
    std::weak_ptr<State> m_state;
    std::uint64_t m_id = 0;
  };

  Signal() : m_state(std::make_shared<State>()) {}

  Signal(const Signal&) = delete;
  Signal& operator=(const Signal&) = delete;
  Signal(Signal&&) = delete;
  Signal& operator=(Signal&&) = delete;

  [[nodiscard]] ScopedConnection connect(Callback callback) {
    const auto id = ++m_state->nextId;
    m_state->slots.push_back({id, std::move(callback)});
    return ScopedConnection{std::weak_ptr<State>(m_state), id};
  }

  void emit(Args... args) {
    // Snapshot live slots so connects/disconnects during dispatch are safe.
    auto snapshot = m_state->slots;
    for (auto& slot : snapshot) {
      if (slot.callback) {
        slot.callback(args...);
      }
    }
    m_state->reap();
  }

private:
  std::shared_ptr<State> m_state;
};
