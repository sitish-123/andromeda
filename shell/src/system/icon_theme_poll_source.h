#pragma once

#include "app/poll_source.h"
#include "core/inotify/inotify.h"

#include <chrono>
#include <functional>
#include <optional>

// Re-checks the icon theme when something that decides it changes: a theme installed or removed, the GTK settings
// files, or dconf's database behind GSettings. Nothing runs while nothing changes.
class IconThemePollSource final : public PollSource {
public:
  IconThemePollSource();

  void setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }

  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  using Clock = std::chrono::steady_clock;

  void rewatch();

  Inotify m_inotify;
  std::vector<int> m_watches;
  std::function<void()> m_changeCallback;
  // Changes come in bursts (a package unpacking a theme, dconf's rename); one check follows the burst.
  std::optional<Clock::time_point> m_checkAt;
};
