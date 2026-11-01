#pragma once

#include "render/core/color.h"

#include <string_view>

// Points GTK 3, GTK 4, qt6ct and fcitx5 at the theme files generated from the design tokens (installed under
// $XDG_DATA_DIRS: andromeda/gtk, qt6ct/colors, fcitx5/themes) and writes the live accent on top. The files it owns:
// $XDG_CONFIG_HOME/gtk-{3,4}.0/andromeda.css (imported once from the user's gtk.css) and
// $XDG_CONFIG_HOME/qt6ct/colors/andromeda.conf. It sets qt6ct's and fcitx5's theme keys and merges the KDE scheme's
// [Colors:*] groups plus ColorScheme/AccentColor into kdeglobals, nothing else.
namespace toolkit_theme {

  // scheme: "light" or "dark". Returns false when the generated files are not installed.
  bool write(std::string_view scheme, const Color& accent, const Color& onAccent);
  // Removes the gtk.css imports write() added; the user's own lines and the other toolkits' settings stay.
  void remove();

} // namespace toolkit_theme
