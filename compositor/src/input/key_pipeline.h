#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace andromeda {

  // Mirrors wlr_keyboard_key_event so a stage reads the same fields as the raw event. `state` is a
  // wl_keyboard_key_state value.
  struct KeyEvent {
    uint32_t time_msec = 0;
    uint32_t keycode = 0;
    uint32_t state = 0;
  };

  enum class KeyResult {
    Pass,    // the next stage sees the event
    Consume, // no later stage sees the event
    Replace, // the stage rewrote the event in place; the next stage sees the new one
  };

  // Stage order is the enum order. A slot with no stage passes.
  enum class KeyStage : size_t {
    AccessibilityFilter, // 1: empty until M5
    KeyboardMonitor,     // 2: empty until M5
    Bindings,            // 3: compositor bindings
    SnippetMatcher,      // 4: observes only; empty until M6
    InputMethodGrab,     // 5
    FocusedClient,       // 6
    Count,
  };

  class KeyPipeline {
  public:
    using Stage = std::function<KeyResult(KeyEvent&)>;

    void set(KeyStage slot, Stage stage) { m_stages[static_cast<size_t>(slot)] = std::move(stage); }

    // Runs every stage in order. Returns Consume when a stage consumed the event, else Pass.
    KeyResult run(KeyEvent& event) const { return runFrom(KeyStage::AccessibilityFilter, event); }

    // Entry point for synthetic keys (dsk_shell_v1 insert_text/key): they skip stages 1 to 4, so no binding fires on
    // them, and start at the input-method grab.
    KeyResult runAfterBindings(KeyEvent& event) const { return runFrom(KeyStage::InputMethodGrab, event); }

  private:
    KeyResult runFrom(KeyStage first, KeyEvent& event) const;

    std::array<Stage, static_cast<size_t>(KeyStage::Count)> m_stages;
  };

} // namespace andromeda
