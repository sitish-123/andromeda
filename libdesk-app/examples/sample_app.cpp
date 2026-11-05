// desk-app-sample: the smallest libdesk-app app. One window with a counter kept in the app state, so a kill and
// a relaunch show the restored window size, maximized state and count (tests/e2e/app_state_restore.sh).
#include "app/desk_app.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"

#include <format>

namespace {

  class SampleApp final : public DeskApp {
  public:
    SampleApp() : DeskApp({.id = "dev.andromeda.AppSample", .name = "App Sample"}) {}

  protected:
    AppWindow& createWindow() override {
      AppWindow& window = addWindow({.width = 720, .height = 480});
      window.setContentBuilder([this, &window](Renderer& /*renderer*/, float /*width*/, float /*height*/) {
        const int clicks = state().get<int>("/sample/clicks", 0);
        auto content = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = 12.0F, .flexGrow = 1.0F});
        content->addChild(ui::label({
            .text = std::format("Clicked {} times", clicks),
            .color = colorSpecFromRole(ColorRole::Label1),
        }));
        content->addChild(ui::button({
            .text = std::string("Click"),
            .variant = ButtonVariant::Default,
            .onClick = [this, &window] {
              state().set("/sample/clicks", state().get<int>("/sample/clicks", 0) + 1);
              window.requestRebuild();
            },
        }));
        return content;
      });
      return window;
    }
  };

} // namespace

int main(int argc, char* argv[]) {
  SampleApp app;
  return app.run(argc, argv);
}
