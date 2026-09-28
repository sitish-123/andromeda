#include "input/lid_state.h"

#include <algorithm>

namespace umbriel {

  LidStateCoordinator::SourceId LidStateCoordinator::addSource(LidState initial) {
    const SourceId id = m_nextSource++;
    m_sources.push_back({.id = id, .state = initial});
    return id;
  }

  void LidStateCoordinator::updateSource(SourceId source, LidState state) {
    for (Source& entry : m_sources) {
      if (entry.id == source) {
        entry.state = state;
        return;
      }
    }
  }

  void LidStateCoordinator::removeSource(SourceId source) {
    std::erase_if(m_sources, [source](const Source& entry) { return entry.id == source; });
  }

  std::optional<LidState> LidStateCoordinator::aggregate() const {
    if (m_sources.empty()) {
      return std::nullopt;
    }
    for (const Source& source : m_sources) {
      if (source.state == LidState::Closed) {
        return LidState::Closed;
      }
    }
    return LidState::Open;
  }

  std::optional<LidState> LidStateCoordinator::takeTransition() {
    if (!m_ready) {
      return std::nullopt;
    }
    const std::optional<LidState> current = aggregate();
    if (!current || current == m_published) {
      return std::nullopt;
    }
    m_published = current;
    return current;
  }

} // namespace umbriel
