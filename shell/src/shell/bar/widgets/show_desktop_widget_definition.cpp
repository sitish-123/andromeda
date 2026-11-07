#include "shell/bar/widgets/show_desktop_widget_definition.h"

const noctalia::bar::WidgetDefinition<ShowDesktopWidget::Options>& showDesktopWidgetDefinition() {
  static const noctalia::bar::WidgetDefinition<ShowDesktopWidget::Options> definition{.type = "show_desktop"};
  return definition;
}
