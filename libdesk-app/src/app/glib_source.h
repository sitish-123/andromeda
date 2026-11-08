#pragma once

#include "core/poll_source.h"

#include <glib.h>
#include <vector>

// Runs the GLib default main context inside the app loop, so GIO async calls (gvfs mounts, network:/// browsing)
// complete on the main thread.
class GlibSource final : public PollSource {
public:
  GlibSource();
  [[nodiscard]] int pollTimeoutMs() const override { return m_timeoutMs; }
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  GMainContext* m_context;
  std::vector<GPollFD> m_fds;
  gint m_priority = G_PRIORITY_DEFAULT;
  int m_timeoutMs = -1;
};
