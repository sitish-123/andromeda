#include "shell/launcher/launcher_view.h"
#include "tests/test_check.h"

#include <string>

namespace {

  struct Return {
    std::string query;
  };

  using State = launcher_view::State<Return>;
  using Browser = launcher_view::Browser<Return>;

  void testFilesRejectsProviderOverview() {
    State state;
    TEST_CHECK(state.set(launcher_view::Files{}));
    TEST_CHECK(!state.set(launcher_view::ProviderOverview{}));
    TEST_CHECK(state.is<launcher_view::Files>());
  }

  void testFilesRejectsProviderView() {
    State state;
    TEST_CHECK(state.set(launcher_view::Files{}));
    TEST_CHECK(!state.set(Browser{.providerViewId = "Session"}));
    TEST_CHECK(state.is<launcher_view::Files>());
    TEST_CHECK(state.providerViewId().empty());
  }

  void testProviderViewKeepsReturnUntilTaken() {
    State state;
    TEST_CHECK(state.set(launcher_view::ProviderOverview{}));
    TEST_CHECK(state.set(Browser{.providerViewId = "Session", .returnView = Return{.query = "/"}}));
    TEST_CHECK(state.providerViewId() == "Session");
    const auto taken = state.takeReturnView();
    TEST_CHECK(taken.has_value() && taken->query == "/");
    TEST_CHECK(!state.takeReturnView().has_value());
    TEST_CHECK(state.providerViewId() == "Session");
  }

  void testDefaultLeavesEveryView() {
    State state;
    TEST_CHECK(state.set(Browser{.providerViewId = "Session"}));
    TEST_CHECK(!state.set(launcher_view::ProviderOverview{}));
    TEST_CHECK(state.set(launcher_view::Files{}));
    TEST_CHECK(state.set(launcher_view::Default{}));
    TEST_CHECK(state.is<launcher_view::Default>());
    TEST_CHECK(state.providerViewId().empty());
  }

} // namespace

int main() {
  testFilesRejectsProviderOverview();
  testFilesRejectsProviderView();
  testProviderViewKeepsReturnUntilTaken();
  testDefaultLeavesEveryView();
  return 0;
}
