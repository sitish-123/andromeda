#include "system/icon_theme_poll_source.h"

#include "system/icon_resolver.h"

#include <algorithm>
#include <sys/inotify.h>

namespace {

  constexpr auto kSettle = std::chrono::milliseconds(500);
  constexpr Inotify::WatchMask kMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB;

} // namespace

IconThemePollSource::IconThemePollSource() {
  (void)IconResolver::checkThemeChanged();
  rewatch();
}

void IconThemePollSource::rewatch() {
  for (const int wd : m_watches) {
    m_inotify.unwatch(wd);
  }
  m_watches.clear();
  for (const auto& dir : IconResolver::themeInputDirs()) {
    if (const auto wd = m_inotify.watch(dir, kMask)) {
      m_watches.push_back(*wd);
    }
  }
}

int IconThemePollSource::pollTimeoutMs() const {
  if (!m_checkAt.has_value()) {
    return -1;
  }
  const auto left = std::chrono::ceil<std::chrono::milliseconds>(*m_checkAt - Clock::now()).count();
  return static_cast<int>(std::max<std::int64_t>(0, left));
}

void IconThemePollSource::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_inotify.fd() >= 0) {
    fds.push_back({.fd = m_inotify.fd(), .events = POLLIN, .revents = 0});
  }
}

void IconThemePollSource::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_inotify.fd() >= 0 && startIdx < fds.size() && (fds[startIdx].revents & POLLIN) != 0) {
    m_inotify.drain();
    m_checkAt = Clock::now() + kSettle;
  }
  if (!m_checkAt.has_value() || Clock::now() < *m_checkAt) {
    return;
  }
  m_checkAt.reset();
  if (IconResolver::checkThemeChanged()) {
    rewatch();
    if (m_changeCallback) {
      m_changeCallback();
    }
  }
}
