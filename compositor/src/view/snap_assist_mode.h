#pragma once

#include <utility>
#include <variant>

namespace andromeda {

  // What snap assist shows: nothing, the open picker, or the picker's last picture fading out. The mode is the one
  // owner of the picture (Picture is move-only), so a picture goes exactly when the mode leaves it.
  template <class Picture> class SnapAssistMode {
  public:
    [[nodiscard]] bool open() const { return std::holds_alternative<Open>(m_state); }
    [[nodiscard]] bool fading() const { return std::holds_alternative<FadingOut>(m_state); }
    // The picture that shows, open or fading; null when closed.
    [[nodiscard]] Picture* picture() {
      if (auto* state = std::get_if<Open>(&m_state)) {
        return &state->picture;
      }
      if (auto* state = std::get_if<FadingOut>(&m_state)) {
        return &state->picture;
      }
      return nullptr;
    }

    // Any mode -> Open on `picture`; the picture that showed before goes.
    void show(Picture picture) { m_state.template emplace<Open>(std::move(picture)); }

    // Open -> FadingOut with the open picture. Rejected in the other modes.
    bool fadeOut() {
      auto* state = std::get_if<Open>(&m_state);
      if (state == nullptr) {
        return false;
      }
      Picture picture = std::move(state->picture);
      m_state.template emplace<FadingOut>(std::move(picture));
      return true;
    }

    // Any mode -> Closed; the picture goes.
    void close() { m_state.template emplace<Closed>(); }

  private:
    struct Closed {};
    struct Open {
      Picture picture;
    };
    struct FadingOut {
      Picture picture;
    };
    std::variant<Closed, Open, FadingOut> m_state;
  };

} // namespace andromeda
