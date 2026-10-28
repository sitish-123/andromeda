#pragma once

#include "app/poll_source.h"

#include <array>
#include <chrono>
#include <functional>
#include <string>

namespace compositors::andromeda {

  class AndromedaRuntime;

  class AndromedaAudioProvider final : public PollSource {
  public:
    using Bands = std::array<float, 64>;
    using SnapshotSource = std::function<Bands()>;
    AndromedaAudioProvider(AndromedaRuntime& runtime, SnapshotSource source);
    ~AndromedaAudioProvider() override;
    AndromedaAudioProvider(const AndromedaAudioProvider&) = delete;
    AndromedaAudioProvider& operator=(const AndromedaAudioProvider&) = delete;

    void setEnabled(bool enabled);
    void notifyChanged();
    [[nodiscard]] int pollTimeoutMs() const override;
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    using Clock = std::chrono::steady_clock;
    void disconnect();
    bool connectSocket();
    bool readReply();
    bool writeSnapshot();

    AndromedaRuntime& m_runtime;
    SnapshotSource m_source;
    bool m_enabled = false;
    bool m_dirty = true;
    bool m_awaitingReply = false;
    int m_fd = -1;
    std::string m_output;
    std::size_t m_written = 0;
    std::string m_input;
    Clock::time_point m_due;
    Clock::time_point m_lastSentAt;
    std::chrono::milliseconds m_backoff{100};
  };

} // namespace compositors::andromeda
