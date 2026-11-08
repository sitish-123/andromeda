#pragma once

#include "core/text_clipboard.h"

#include <memory>
#include <optional>
#include <string>

class WaylandConnection;

// Text copy and paste for Input controls in desk apps, over the wl_data_device selection. Paste is synchronous in
// Input, so the text of each new clipboard is read ahead, off the main thread, when the compositor announces it.
class AppClipboard final : public TextClipboard {
public:
  explicit AppClipboard(WaylandConnection& wayland);
  ~AppClipboard() override;
  AppClipboard(const AppClipboard&) = delete;
  AppClipboard& operator=(const AppClipboard&) = delete;

  [[nodiscard]] std::optional<std::string> clipboardText() override { return m_text; }
  void setClipboardText(std::string text) override;

private:
  void refresh();

  WaylandConnection& m_wayland;
  std::optional<std::string> m_text;
  unsigned m_generation = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
