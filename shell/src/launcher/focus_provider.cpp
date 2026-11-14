#include "launcher/focus_provider.h"

#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <ctime>

namespace {

  constexpr std::string_view kStart = "start:";
  constexpr std::string_view kHour = "hour:";
  constexpr std::string_view kEnd = "end";

} // namespace

std::string FocusProvider::displayName() const { return i18n::tr("launcher.providers.focus.title"); }

std::vector<LauncherResult> FocusProvider::query(std::string_view text) const { return results(text, false); }

std::vector<LauncherResult> FocusProvider::queryPrefixed(std::string_view text) const { return results(text, true); }

std::vector<LauncherResult> FocusProvider::results(std::string_view text, bool emptyShowsAll) const {
  const std::string query = StringUtils::toLower(StringUtils::trim(text));
  if (m_notifications == nullptr || (query.empty() && !emptyShowsAll)) {
    return {};
  }
  std::vector<LauncherResult> out;
  const auto add = [&](std::string id, std::string title, std::string glyph) {
    const double score = query.empty() ? 0.0 : FuzzyMatch::score(query, StringUtils::toLower(title));
    if (!query.empty() && !FuzzyMatch::isMatch(score)) {
      return;
    }
    LauncherResult result;
    result.id = std::move(id);
    result.title = std::move(title);
    result.subtitle = i18n::tr("launcher.providers.focus.subtitle");
    result.glyphName = std::move(glyph);
    result.score = score;
    out.push_back(std::move(result));
  };
  const auto& active = m_notifications->focus().active();
  if (active.has_value()) {
    add(std::string(kEnd), i18n::tr("launcher.providers.focus.end"), "moon-off");
  }
  for (const focus::Focus& f : m_notifications->focus().focuses()) {
    if (active.has_value() && active->focusId == f.id && !active->untilEpochSec.has_value()) {
      continue;
    }
    add(std::string(kStart) + f.id, i18n::tr("launcher.providers.focus.start", "name", f.name), f.glyph);
    add(std::string(kHour) + f.id, i18n::tr("launcher.providers.focus.start-hour", "name", f.name), f.glyph);
  }
  return out;
}

bool FocusProvider::activate(const LauncherResult& result) {
  if (m_notifications == nullptr) {
    return false;
  }
  const std::string_view id = result.id;
  if (id == kEnd) {
    m_notifications->endFocus();
    return true;
  }
  if (id.starts_with(kStart)) {
    return m_notifications->startFocus(id.substr(kStart.size()), std::nullopt, "user");
  }
  if (id.starts_with(kHour)) {
    return m_notifications->startFocus(id.substr(kHour.size()), std::time(nullptr) + 3600, "user");
  }
  return false;
}
