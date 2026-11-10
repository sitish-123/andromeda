#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

// A live activity hides the banner, except a timed call that sits compact (not hovered, not restless).
[[nodiscard]] constexpr bool activityHidesBanner(bool active, bool timed, bool hovered, bool restless) noexcept {
  return active && (!timed || hovered || restless);
}

// What the island shows, and the two timers that end it. An OSD outranks everything; an activity that hides the
// banner (activityHidesBanner) holds it with its timer paused; banners take turns from the queue.
// advance() is the one place that promotes a banner and decides whether the banner timer runs.
// BannerT needs `std::uint32_t id` and `bool sticky`.
template <typename BannerT, typename OsdT> class IslandQueue {
public:
  enum class Mode : std::uint8_t { Rest, Osd, Activity, Banner };
  enum class TimerCmd : std::uint8_t { Keep, Start, Stop };
  struct Step {
    TimerCmd banner = TimerCmd::Keep;
    TimerCmd osd = TimerCmd::Keep;
    // The OSD, the current banner or the activity changed; the island must redraw.
    bool changed = false;
  };

  [[nodiscard]] Mode mode() const noexcept {
    if (m_osd.has_value()) {
      return Mode::Osd;
    }
    if (m_activity) {
      return Mode::Activity;
    }
    return m_banner.has_value() ? Mode::Banner : Mode::Rest;
  }
  [[nodiscard]] const std::optional<OsdT>& osd() const noexcept { return m_osd; }
  [[nodiscard]] const std::optional<BannerT>& banner() const noexcept { return m_banner; }
  [[nodiscard]] const std::deque<BannerT>& queued() const noexcept { return m_queue; }
  [[nodiscard]] bool bannerTimerRunning() const noexcept { return m_bannerTiming; }

  // A new OSD value restarts the OSD timer; the current banner goes back to the queue front and restarts later.
  Step showOsd(OsdT content) {
    if (m_banner.has_value()) {
      m_queue.push_front(std::move(*m_banner));
      m_banner.reset();
    }
    m_osd = std::move(content);
    Step step = advance(true);
    step.osd = TimerCmd::Start;
    return step;
  }
  // The OSD timer fired, or the island can no longer show the OSD.
  Step endOsd() {
    const bool had = m_osd.has_value();
    m_osd.reset();
    Step step = advance(had);
    step.osd = had ? TimerCmd::Stop : TimerCmd::Keep;
    return step;
  }
  // `hidesBanner`: an activity hides the banner on every island (activityHidesBanner).
  Step setActivity(bool hidesBanner) {
    const bool changed = m_activity != hidesBanner;
    m_activity = hidesBanner;
    return advance(changed);
  }
  Step push(BannerT banner) {
    m_queue.push_back(std::move(banner));
    return advance(false);
  }
  // Replaces the current or queued banner with this id; false when neither holds it.
  bool update(BannerT banner, Step& step) {
    if (m_banner.has_value() && m_banner->id == banner.id) {
      m_banner = std::move(banner);
      step = advance(true);
      return true;
    }
    for (auto& queued : m_queue) {
      if (queued.id == banner.id) {
        queued = std::move(banner);
        return true;
      }
    }
    return false;
  }
  // The banner timer fired, or the user dismissed or acted on the current banner.
  Step endBanner() {
    const bool had = m_banner.has_value();
    m_banner.reset();
    return advance(had);
  }
  // Drops every banner, current and queued, that matches.
  template <typename Pred> Step dropIf(Pred pred) {
    std::erase_if(m_queue, pred);
    const bool current = m_banner.has_value() && pred(*m_banner);
    if (current) {
      m_banner.reset();
    }
    return advance(current);
  }

private:
  Step advance(bool changed) {
    bool promoted = false;
    if (!m_osd.has_value() && !m_banner.has_value() && !m_queue.empty()) {
      m_banner = std::move(m_queue.front());
      m_queue.pop_front();
      promoted = true;
    }
    const bool want = mode() == Mode::Banner && !m_banner->sticky;
    Step step{.changed = changed || promoted};
    if (want && (!m_bannerTiming || promoted)) {
      step.banner = TimerCmd::Start;
    } else if (!want && m_bannerTiming) {
      step.banner = TimerCmd::Stop;
    }
    m_bannerTiming = want;
    return step;
  }

  std::optional<OsdT> m_osd;
  std::optional<BannerT> m_banner;
  std::deque<BannerT> m_queue;
  bool m_activity = false;
  bool m_bannerTiming = false;
};
