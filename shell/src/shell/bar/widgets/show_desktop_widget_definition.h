#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/show_desktop_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<ShowDesktopWidget::Options>& showDesktopWidgetDefinition();
