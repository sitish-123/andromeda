#include "shortcut_labels.h"

#include <deque>

namespace umbriel {

  std::vector<std::string> shortcutLabels(size_t count, std::string_view keys) {
    if (keys.size() < 2) {
      return {};
    }

    std::deque<std::string> terminals;
    for (const char key : keys) {
      terminals.emplace_back(1, key);
    }

    while (terminals.size() < count) {
      std::string prefix = std::move(terminals.back());
      terminals.pop_back();
      for (const char key : keys) {
        terminals.push_back(prefix + key);
      }
    }

    std::vector<std::string> labels;
    labels.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      labels.push_back(std::move(terminals.front()));
      terminals.pop_front();
    }
    return labels;
  }

} // namespace umbriel
