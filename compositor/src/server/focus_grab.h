#pragma once

#include "wlr.h"

#include <memory>
#include <vector>

namespace andromeda {

  // Serves hyprland_focus_grab_manager_v1: a client (the shell) names the surfaces of an open panel; the first pointer
  // or touch press outside them clears the grab and sends `cleared`, so the panel closes. The press itself still
  // reaches what it hit (a window under the click gets it and focus), and the shell needs no full-screen shield.
  // One grab is active at a time: a new one clears the others.
  class FocusGrabManager {
  public:
    explicit FocusGrabManager(wl_display* display);
    ~FocusGrabManager();
    FocusGrabManager(const FocusGrabManager&) = delete;
    FocusGrabManager& operator=(const FocusGrabManager&) = delete;

    // A pointer button or touch went down on `surface` (null: on nothing a client drew).
    void press(wlr_surface* surface);
    // A grab with committed surfaces exists.
    [[nodiscard]] bool active() const;

    struct Grab;
    struct Member;

  private:
    friend struct FocusGrabCallbacks;
    void activated(Grab* grab);
    void clear(Grab& grab);

    wl_global* m_global = nullptr;
    std::vector<Grab*> m_grabs;
  };

} // namespace andromeda
