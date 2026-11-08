#pragma once

#include "ui/controls/glyph_picker.h"

#include <functional>
#include <optional>
#include <string>

struct GlyphPickerDialogOptions {
  std::optional<std::string> initialGlyph;
  std::string title;
};

class GlyphPickerDialogPresenter {
public:
  virtual ~GlyphPickerDialogPresenter() = default;

  [[nodiscard]] virtual bool openGlyphPicker() = 0;
  virtual void closeGlyphPickerWithoutResult() = 0;
};

class GlyphPickerDialog {
public:
  using CompletionCallback = std::function<void(std::optional<GlyphPickerResult>)>;

  static void setPresenter(GlyphPickerDialogPresenter* presenter) noexcept;
  [[nodiscard]] static bool open(GlyphPickerDialogOptions options, CompletionCallback callback);
  static void complete(const GlyphPickerResult& result);
  // Ends the request now (a cancel or a new open no longer sees it) and returns the call that hands `result` to its
  // callback, for the popup to run once its fade is over.
  [[nodiscard]] static std::function<void()> settle(const GlyphPickerResult& result);
  static void cancelIfPending();

  [[nodiscard]] static const GlyphPickerDialogOptions& currentOptions();
  [[nodiscard]] static std::optional<GlyphPickerResult> lastResult() noexcept;
};
