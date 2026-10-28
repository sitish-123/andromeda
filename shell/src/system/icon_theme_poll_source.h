#pragma once

#include "app/poll_source.h"

#include <functional>
#include <memory>

// Re-checks the icon theme when something that decides it changes: a theme installed or removed, the GTK settings
// files, or dconf's database behind GSettings. Nothing runs while nothing changes.
class IconThemePollSource final : public PollSource {
public:
  IconThemePollSource();
  ~IconThemePollSource() override;

  IconThemePollSource(const IconThemePollSource&) = delete;
  IconThemePollSource& operator=(const IconThemePollSource&) = delete;

  void setChangeCallback(std::function<void()> callback);
  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
