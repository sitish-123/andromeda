#include "input/keyboard_layout.h"

#include "check.h"

using andromeda::matchingLayoutGroup;

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

ANDROMEDA_TEST(matchesLayoutByNameAcrossDifferentOrders) {
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

ANDROMEDA_TEST(leavesADeviceWithoutTheNamedLayoutUnchanged) {
  Keymap source("us,de");
  Keymap target("us,fr");
  CHECK(source.get() != nullptr);
  CHECK(target.get() != nullptr);
  CHECK(!matchingLayoutGroup(target.get(), source.get(), 1).has_value());
}

ANDROMEDA_TEST(rejectsAnInvalidSourceGroup) {
  Keymap source("us,de");
  Keymap target("us,de");
  CHECK(source.get() != nullptr);
  CHECK(target.get() != nullptr);
  CHECK(!matchingLayoutGroup(target.get(), source.get(), 2).has_value());
}

namespace {
  const andromeda::XkbNames kSystem{
      .layout = "us", .variant = "intl", .options = "terminate:ctrl_alt_bksp", .model = "pc105"
  };
} // namespace

ANDROMEDA_TEST(systemKeymapAppliesWhenConfigSetsNoXkbNames) {
  const andromeda::Config::Input input;
  CHECK(andromeda::usesSystemXkbNames(input));
  CHECK(andromeda::resolveXkbNames(input.keyboard, nullptr, kSystem) == kSystem);
}

ANDROMEDA_TEST(anyConfiguredXkbNameReplacesTheWholeSystemKeymap) {
  andromeda::Config::Input input;
  input.keyboard.options = "caps:escape";
  CHECK(!andromeda::usesSystemXkbNames(input));
  const andromeda::XkbNames expected{.layout = "", .variant = "", .options = "caps:escape", .model = ""};
  CHECK(andromeda::resolveXkbNames(input.keyboard, nullptr, kSystem) == expected);
}

ANDROMEDA_TEST(deviceXkbNamesReplaceTheSystemKeymap) {
  const andromeda::Config::Input input;
  andromeda::Config::Input::Device device;
  device.name = "kbd";
  device.layout = "de";
  const andromeda::XkbNames expected{.layout = "de", .variant = "", .options = "", .model = ""};
  CHECK(andromeda::resolveXkbNames(input.keyboard, &device, kSystem) == expected);
}

ANDROMEDA_TEST(deviceClearingEveryXkbNameFallsBackToTheSystemKeymap) {
  andromeda::Config::Input input;
  input.keyboard.layout = "fr";
  andromeda::Config::Input::Device device;
  device.name = "kbd";
  device.layout = "";
  input.devices.push_back(device);
  CHECK(andromeda::usesSystemXkbNames(input));
  CHECK(andromeda::resolveXkbNames(input.keyboard, input.devices.data(), kSystem) == kSystem);
}

int main() { return RUN_TESTS(); }
