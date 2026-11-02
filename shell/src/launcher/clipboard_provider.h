#pragma once

#include "launcher/launcher_provider.h"

#include <cstdint>
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

  ClipboardService* m_clipboard = nullptr;
};
