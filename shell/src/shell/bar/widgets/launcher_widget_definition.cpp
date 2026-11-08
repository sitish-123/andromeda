#include "shell/bar/widgets/launcher_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const noctalia::bar::WidgetDefinition<LauncherWidget::Options>& launcherWidgetDefinition() {
  using Options = LauncherWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "launcher",
      .fields = noctalia::bar::glyphButtonFields<Options>(noctalia::bar::field<&Options::iconSize>({
          .key = "icon_size",
          .minValue = 0.0,
          .maxValue = 64.0,
          .step = 4.0,
      }), noctalia::bar::field<&Options::cellSize>({
          .key = "cell_size",
          .minValue = 0.0,
          .maxValue = 96.0,
          .step = 4.0,
      })),
      .glyph = [](const Options& options) { return options.glyph; },
  };
  return definition;
}
