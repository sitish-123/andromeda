#include "input/keyboard_layout.h"

#include "check.h"

using umbriel::matchingLayoutGroup;

namespace {

  class Keymap {
  public:
    explicit Keymap(const char* layouts) : m_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {
      const xkb_rule_names names{
          .rules = nullptr,
          .model = nullptr,
          .layout = layouts,
          .variant = nullptr,
          .options = nullptr,
      };
      if (m_context != nullptr) {
        m_keymap = xkb_keymap_new_from_names(m_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
      }
    }

    ~Keymap() {
      if (m_keymap != nullptr) {
        xkb_keymap_unref(m_keymap);
      }
      if (m_context != nullptr) {
        xkb_context_unref(m_context);
      }
    }

    Keymap(const Keymap&) = delete;
    Keymap& operator=(const Keymap&) = delete;

    [[nodiscard]] xkb_keymap* get() const { return m_keymap; }

  private:
    xkb_context* m_context = nullptr;
    xkb_keymap* m_keymap = nullptr;
  };

} // namespace

UMBRIEL_TEST(matchesLayoutByNameAcrossDifferentOrders) {
  Keymap source("us,de");
  Keymap target("de,us");
  CHECK(source.get() != nullptr);
  CHECK(target.get() != nullptr);

  const auto group = matchingLayoutGroup(target.get(), source.get(), 1);
  CHECK(group.has_value());
  if (group.has_value()) {
    CHECK_EQ(*group, xkb_layout_index_t{0});
  }
}

UMBRIEL_TEST(leavesADeviceWithoutTheNamedLayoutUnchanged) {
  Keymap source("us,de");
  Keymap target("us,fr");
  CHECK(source.get() != nullptr);
  CHECK(target.get() != nullptr);
  CHECK(!matchingLayoutGroup(target.get(), source.get(), 1).has_value());
}

UMBRIEL_TEST(rejectsAnInvalidSourceGroup) {
  Keymap source("us,de");
  Keymap target("us,de");
  CHECK(source.get() != nullptr);
  CHECK(target.get() != nullptr);
  CHECK(!matchingLayoutGroup(target.get(), source.get(), 2).has_value());
}

int main() { return RUN_TESTS(); }
