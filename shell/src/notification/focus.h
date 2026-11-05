#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Focus modes (docs/focus.md §3): what may break through while a Focus is on, and which Focus is on.
namespace focus {

  enum class AppMode : std::uint8_t { Allow, Deny };
  enum class People : std::uint8_t { Everyone, Contacts, Favourites, Nobody };

  struct Focus {
    std::string id;
    std::string name;
    std::string glyph;
    AppMode appMode = AppMode::Allow;
    std::vector<std::string> apps; // app identities (desktop ids without ".desktop")
    People calls = People::Nobody;
    bool repeatedCalls = true; // the same caller twice in 3 min breaks through
    People messages = People::Nobody;
    bool timeSensitive = true;
  };

  struct Active {
    std::string focusId;
    std::optional<std::int64_t> untilEpochSec; // nullopt: until the user or its trigger ends it
    std::string startedBy;                     // "user", "schedule", "fullscreen", "phone", ...
  };

  enum class LinkKind : std::uint8_t { None, Call, Message };

  struct Incoming {
    std::string app; // identity(): desktop entry, portal app id, scope app id, else app name
    bool critical = false;      // urgency critical
    std::string portalPriority; // "low", "normal", "high", "urgent"; empty for org.freedesktop.Notifications
    std::string portalCategory; // spec v2 category; empty when none
    LinkKind link = LinkKind::None;
    bool contact = false;
    bool favourite = false;
    bool repeatedCall = false;
  };

  enum class Verdict : std::uint8_t {
    Rules, // no Focus: today's notification rules decide
    Show,
    Hold, // history only, no sound, no banner, counted for the summary
  };

  struct Decision {
    Verdict verdict = Verdict::Rules;
    bool hideContent = false; // the screen is shared: the banner hides the content, history keeps it
  };

  // The app a notification belongs to, in §3 order; ".desktop" is dropped so ids compare equal.
  [[nodiscard]] std::string identity(std::string_view desktopEntry, std::string_view portalAppId,
                                     std::string_view scopeAppId, std::string_view appName);
  [[nodiscard]] bool timeSensitive(const Incoming& n);
  // Pure: `active` is the Focus on now, nullptr when none.
  [[nodiscard]] Decision decide(const Focus* active, const Incoming& n, bool screenShared);

  // The Focus list and the one on now. Saving is the owner's: it reads active() after each change callback.
  class FocusService {
  public:
    FocusService();

    [[nodiscard]] const std::vector<Focus>& focuses() const noexcept { return m_focuses; }
    [[nodiscard]] const Focus* find(std::string_view id) const;
    [[nodiscard]] const std::optional<Active>& active() const noexcept { return m_active; }
    // The active Focus, nullptr when none or when its end time is past at `nowEpochSec`.
    [[nodiscard]] const Focus* current(std::int64_t nowEpochSec) const;

    // False for an unknown id.
    bool start(std::string_view id, std::optional<std::int64_t> untilEpochSec, std::string startedBy);
    void end();
    // Ends a Focus whose end time is past; true when it ended one.
    bool expire(std::int64_t nowEpochSec);
    // A saved Active from the last session; dropped when its Focus no longer exists.
    void restore(Active active);

    void setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }

  private:
    void changed();

    std::vector<Focus> m_focuses;
    std::optional<Active> m_active;
    std::function<void()> m_changed;
  };

} // namespace focus
