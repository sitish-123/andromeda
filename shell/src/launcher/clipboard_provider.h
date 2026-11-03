#pragma once

#include "core/timer_manager.h"
#include "launcher/launcher_provider.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class ClipboardService;

// Clipboard history as a launcher mode (DESIGN.md "Launcher media modes"): newest first, pinned on top.
class ClipboardProvider : public LauncherProvider {
public:
  explicit ClipboardProvider(ClipboardService* clipboard) : m_clipboard(clipboard) {}

  [[nodiscard]] std::string_view defaultPrefix() const override { return "clip"; }
  [[nodiscard]] std::string_view id() const override { return "Clipboard"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "clipboard"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  void setResultsChangedCallback(std::function<void()> callback) override { m_onResultsChanged = std::move(callback); }

  bool activate(const LauncherResult& result) override;
  // Pin or unpin, and delete, by result id; the launcher's preview buttons call these.
  bool togglePinned(const LauncherResult& result);
  bool remove(const LauncherResult& result);
  // The stored text preview of a text entry (the row title is only its first line).
  [[nodiscard]] std::string text(const LauncherResult& result) const;
  // Encoded image bytes of an image entry (loads the payload from disk on first use); empty if none.
  [[nodiscard]] std::vector<std::uint8_t> imageBytes(const LauncherResult& result);
  // Payload size in bytes, for the preview details.
  [[nodiscard]] std::size_t byteSize(const LauncherResult& result) const;

private:
  [[nodiscard]] std::size_t indexOf(const LauncherResult& result) const;
  void loadNextThumbnail() const;

  ClipboardService* m_clipboard = nullptr;
  std::function<void()> m_onResultsChanged;
  // Row thumbnails as data URIs (the texture cache decodes them off the main thread), by storage id. They load one
  // per timer tick after the first frame: a payload read decrypts it from disk.
  mutable std::unordered_map<std::string, std::string> m_thumbs;
  mutable std::vector<std::string> m_pendingThumbs;
  mutable Timer m_thumbTimer;
};
