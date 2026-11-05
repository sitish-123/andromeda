#include "notification/focus.h"

#include <algorithm>

namespace focus {

  namespace {

    std::string_view withoutDesktopSuffix(std::string_view id) {
      constexpr std::string_view kSuffix = ".desktop";
      return id.ends_with(kSuffix) ? id.substr(0, id.size() - kSuffix.size()) : id;
    }

    bool peopleAllow(People rule, const Incoming& n) {
      switch (rule) {
      case People::Everyone:
        return true;
      case People::Contacts:
        return n.contact || n.favourite;
      case People::Favourites:
        return n.favourite;
      case People::Nobody:
        return false;
      }
      return false;
    }

    std::vector<Focus> builtIns() {
      return {
          {.id = "dnd", .name = "Do Not Disturb", .glyph = "moon"},
          {.id = "work", .name = "Work", .glyph = "briefcase", .calls = People::Contacts, .messages = People::Contacts},
          {.id = "personal", .name = "Personal", .glyph = "user", .calls = People::Everyone,
           .messages = People::Everyone},
          {.id = "sleep", .name = "Sleep", .glyph = "bed", .calls = People::Favourites},
          {.id = "gaming", .name = "Gaming", .glyph = "device-gamepad-2", .calls = People::Favourites},
      };
    }

  } // namespace

  std::string identity(std::string_view desktopEntry, std::string_view portalAppId, std::string_view scopeAppId,
                       std::string_view appName) {
    for (const std::string_view candidate : {desktopEntry, portalAppId, scopeAppId, appName}) {
      if (!candidate.empty()) {
        return std::string(withoutDesktopSuffix(candidate));
      }
    }
    return {};
  }

  bool timeSensitive(const Incoming& n) {
    const std::string_view c = n.portalCategory;
    return n.critical || n.portalPriority == "urgent" || c == "alarm.ringing" || c == "call.incoming"
        || c == "weather.warning.extreme" || c.starts_with("cellbroadcast.danger.");
  }

  Decision decide(const Focus* active, const Incoming& n, bool screenShared) {
    Verdict verdict = Verdict::Hold;
    if (active == nullptr) {
      verdict = Verdict::Rules;
    } else if (active->timeSensitive && timeSensitive(n)) {
      verdict = Verdict::Show;
    } else if (n.link == LinkKind::Call) {
      const bool show = peopleAllow(active->calls, n) || (active->repeatedCalls && n.repeatedCall);
      verdict = show ? Verdict::Show : Verdict::Hold;
    } else if (n.link == LinkKind::Message) {
      verdict = peopleAllow(active->messages, n) ? Verdict::Show : Verdict::Hold;
    } else {
      const std::string_view app = withoutDesktopSuffix(n.app);
      const bool listed = std::ranges::any_of(active->apps, [app](const std::string& id) { return withoutDesktopSuffix(id) == app; });
      const bool show = active->appMode == AppMode::Allow ? listed : !listed;
      verdict = show ? Verdict::Show : Verdict::Hold;
    }
    return {.verdict = verdict, .hideContent = screenShared && verdict != Verdict::Hold};
  }

  FocusService::FocusService() : m_focuses(builtIns()) {}

  const Focus* FocusService::find(std::string_view id) const {
    const auto it = std::ranges::find(m_focuses, id, &Focus::id);
    return it != m_focuses.end() ? &*it : nullptr;
  }

  const Focus* FocusService::current(std::int64_t nowEpochSec) const {
    if (!m_active.has_value() || (m_active->untilEpochSec.has_value() && *m_active->untilEpochSec <= nowEpochSec)) {
      return nullptr;
    }
    return find(m_active->focusId);
  }

  bool FocusService::start(std::string_view id, std::optional<std::int64_t> untilEpochSec, std::string startedBy) {
    if (find(id) == nullptr) {
      return false;
    }
    m_active = Active{.focusId = std::string(id), .untilEpochSec = untilEpochSec, .startedBy = std::move(startedBy)};
    changed();
    return true;
  }

  void FocusService::end() {
    if (m_active.has_value()) {
      m_active.reset();
      changed();
    }
  }

  bool FocusService::expire(std::int64_t nowEpochSec) {
    if (m_active.has_value() && current(nowEpochSec) == nullptr) {
      end();
      return true;
    }
    return false;
  }

  void FocusService::restore(Active active) {
    if (find(active.focusId) != nullptr) {
      m_active = std::move(active);
    }
  }

  void FocusService::changed() {
    if (m_changed) {
      m_changed();
    }
  }

} // namespace focus
