#pragma once

#include <functional>
#include <optional>
#include <string>

// Minimal text-clipboard abstraction. UI controls (e.g. Input) copy and paste
// through this interface so they stay agnostic of the Wayland data-control
// transport and the clipboard-history service that implements it.
class TextClipboard {
public:
  virtual ~TextClipboard() = default;

  // Most recent plain-text clipboard selection currently known, if any.
  [[nodiscard]] virtual std::optional<std::string> clipboardText() = 0;

  // Replace the system clipboard selection with the given text.
  virtual void setClipboardText(std::string text) = 0;

  // The primary selection (select to copy, middle click to paste); hosts without one keep these defaults.
  [[nodiscard]] virtual std::optional<std::string> primaryText() { return std::nullopt; }
  virtual void setPrimaryText(std::string /*text*/) {}
  // The primary text once known: a middle click can arrive in the same dispatch as the selection it pastes.
  virtual void requestPrimaryText(std::function<void(std::optional<std::string>)> done) { done(primaryText()); }
};
