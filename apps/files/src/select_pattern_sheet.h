#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Button;
class Label;
class Node;

// Select by Pattern (Ctrl+S; mockup files-v2-live.html §16, FILES-UI-SPEC §18): a wildcard ("*.jpg | *.png") or a
// regular expression over the shown names, a live match count, and Select / Deselect that add to or take from the
// selection.
class SelectPatternSheet {
public:
  enum class Mode : std::uint8_t { Wildcard, Regex };
  // names = every shown item; apply(matches, select): select = add them, else take them out of the selection.
  using Apply = std::function<void(const std::vector<std::string>& matches, bool select)>;

  SelectPatternSheet(std::vector<std::string> names, std::string pattern, Mode mode, Apply apply,
                     std::function<void()> close, std::function<void()> rebuild,
                     std::function<void(class Input&)> focus);

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void setPattern(std::string pattern, bool showInField = false);
  void setMode(Mode mode);
  void apply(bool select);
  [[nodiscard]] const std::string& pattern() const noexcept { return m_pattern; }
  [[nodiscard]] Mode mode() const noexcept { return m_mode; }

  // Matches of `pattern` in `names`; nullopt-like: `error` set when a regex does not compile.
  [[nodiscard]] static std::vector<std::string> match(const std::vector<std::string>& names, const std::string& pattern,
                                                      Mode mode, std::string* error = nullptr);

private:
  void update();

  std::vector<std::string> m_names;
  std::string m_pattern;
  Mode m_mode;
  Apply m_apply;
  std::function<void()> m_close;
  std::function<void()> m_rebuild;
  std::function<void(class Input&)> m_focus;
  std::vector<std::string> m_matches;
  std::string m_error;
  bool m_focused = false;

  Label* m_note = nullptr;
  Button* m_select = nullptr;
  Button* m_deselect = nullptr;
};
