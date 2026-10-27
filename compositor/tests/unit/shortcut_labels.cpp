#include "overview/shortcut_labels.h"

#include "check.h"

#include <string>
#include <vector>

using andromeda::shortcutLabels;

namespace {

  bool isPrefixFree(const std::vector<std::string>& labels) {
    for (size_t i = 0; i < labels.size(); ++i) {
      for (size_t j = 0; j < labels.size(); ++j) {
        if (i != j && labels[j].starts_with(labels[i])) {
          return false;
        }
      }
    }
    return true;
  }

} // namespace

ANDROMEDA_TEST(zeroLabelsIsEmpty) { CHECK(shortcutLabels(0, "12").empty()); }

ANDROMEDA_TEST(fewerThanTwoKeysIsEmpty) {
  CHECK(shortcutLabels(4, "").empty());
  CHECK(shortcutLabels(4, "1").empty());
}

ANDROMEDA_TEST(singleKeyLabelsKeepFavoriteOrder) {
  CHECK_EQ(shortcutLabels(4, "asdf"), (std::vector<std::string>{"a", "s", "d", "f"}));
}

ANDROMEDA_TEST(twoKeysExpandLeastFavoriteTerminal) {
  CHECK_EQ(shortcutLabels(3, "12"), (std::vector<std::string>{"1", "21", "22"}));
}

ANDROMEDA_TEST(digitsExpandZeroFirst) {
  CHECK_EQ(
      shortcutLabels(12, "1234567890"),
      (std::vector<std::string>{"1", "2", "3", "4", "5", "6", "7", "8", "9", "01", "02", "03"})
  );
}

ANDROMEDA_TEST(generatedLabelsArePrefixFree) {
  CHECK(isPrefixFree(shortcutLabels(100, "asdf")));
  CHECK(isPrefixFree(shortcutLabels(100, "1234567890")));
}

int main() { return RUN_TESTS(); }
