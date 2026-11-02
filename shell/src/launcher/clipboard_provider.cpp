#include "launcher/clipboard_provider.h"

#include "clipboard/clipboard_service.h"
#include "i18n/i18n.h"
#include "time/time_format.h"
#include "util/string_utils.h"

#include <chrono>

namespace {

  constexpr std::string_view kIdPrefix = "clip-";

  std::string firstLine(std::string_view text) {
    const auto trimmed = StringUtils::trim(text);
    const auto end = trimmed.find('\n');
    return std::string(end == std::string_view::npos ? trimmed : trimmed.substr(0, end));
  }

  // Groups in the order the launcher draws them.
  std::string dayGroup(const ClipboardEntry& entry) {
    if (entry.pinned) {
      return i18n::tr("launcher.clipboard.pinned");
    }
    const auto age = std::chrono::system_clock::now() - entry.capturedAt;
    if (age < std::chrono::hours(24)) {
      return i18n::tr("launcher.clipboard.today");
    }
    if (age < std::chrono::hours(48)) {
      return i18n::tr("launcher.clipboard.yesterday");
    }
    return i18n::tr("launcher.clipboard.earlier");
  }

  std::string kindGlyph(const ClipboardEntry& entry, std::string_view text) {
    if (entry.isImage()) {
      return "photo";
    }
    if (text.starts_with("http://") || text.starts_with("https://")) {
      return "link";
    }
    if (text.size() == 7 && text.front() == '#') {
      return "palette";
    }
    return "file-text";
  }

} // namespace

std::string ClipboardProvider::displayName() const { return i18n::tr("launcher.providers.clipboard.title"); }

std::vector<LauncherResult> ClipboardProvider::query(std::string_view text) const {
  std::vector<LauncherResult> results;
  if (m_clipboard == nullptr) {
    return results;
  }
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  const auto& history = m_clipboard->history();
  results.reserve(history.size());
  // Pinned entries first, then newest first; history() keeps newest at the front.
  for (const bool pinnedPass : {true, false}) {
    for (const ClipboardEntry& entry : history) {
      if (entry.pinned != pinnedPass) {
        continue;
      }
      const std::string line = entry.isImage() ? i18n::tr("launcher.clipboard.image") : firstLine(entry.textPreview);
      if (!needle.empty() && !StringUtils::toLower(entry.textPreview).contains(needle)) {
        continue;
      }
      LauncherResult r;
      r.id = std::string(kIdPrefix) + entry.storageId;
      r.title = line;
      r.subtitle = formatTimeAgo(entry.capturedAt);
      r.glyphName = kindGlyph(entry, entry.textPreview);
      r.category = dayGroup(entry);
      r.presentation = entry.isImage() ? "clipboard-image" : "clipboard-text";
      r.pinned = entry.pinned;
      results.push_back(std::move(r));
    }
  }
  return results;
}

std::size_t ClipboardProvider::indexOf(const LauncherResult& result) const {
  if (m_clipboard == nullptr || !result.id.starts_with(kIdPrefix)) {
    return static_cast<std::size_t>(-1);
  }
  const std::string_view storageId = std::string_view(result.id).substr(kIdPrefix.size());
  const auto& history = m_clipboard->history();
  for (std::size_t i = 0; i < history.size(); ++i) {
    if (history[i].storageId == storageId) {
      return i;
    }
  }
  return static_cast<std::size_t>(-1);
}

bool ClipboardProvider::activate(const LauncherResult& result) {
  const std::size_t index = indexOf(result);
  if (index == static_cast<std::size_t>(-1) || !m_clipboard->ensureEntryLoaded(index)) {
    return false;
  }
  const ClipboardEntry entry = m_clipboard->history()[index];
  if (!entry.pinned) {
    (void)m_clipboard->promoteEntry(index);
  }
  return m_clipboard->copyEntry(entry);
}

bool ClipboardProvider::togglePinned(const LauncherResult& result) {
  const std::size_t index = indexOf(result);
  return index != static_cast<std::size_t>(-1) && m_clipboard->setEntryPinned(index, !m_clipboard->history()[index].pinned);
}

bool ClipboardProvider::remove(const LauncherResult& result) {
  const std::size_t index = indexOf(result);
  return index != static_cast<std::size_t>(-1) && m_clipboard->removeHistoryEntry(index);
}

std::string ClipboardProvider::text(const LauncherResult& result) const {
  const std::size_t index = indexOf(result);
  return index == static_cast<std::size_t>(-1) ? std::string() : m_clipboard->history()[index].textPreview;
}
