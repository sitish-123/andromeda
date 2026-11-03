#include "system/background_warning_state.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

int main() {
  char tmpl[] = "/tmp/background-warning-state-test-XXXXXX";
  const fs::path root = ::mkdtemp(tmpl);
  const std::string path = (root / "state/activity.json").string();
  const std::string unitFile = "/home/u/.config/systemd/user/opencode.service";

  {
    BackgroundWarningState state(path);
    TEST_CHECK(!state.alreadyNotified(unitFile, 100));
    state.markNotified(unitFile, 100);
    state.setIgnored("docker.service", true);
  }
  // A new boot (a new instance) remembers both.
  {
    BackgroundWarningState state(path);
    TEST_CHECK(state.alreadyNotified(unitFile, 100));
    TEST_CHECK(!state.alreadyNotified(unitFile, 200)); // the file changed: notify again
    TEST_CHECK(state.ignored("docker.service"));
    state.setIgnored("docker.service", false);
  }
  TEST_CHECK(!BackgroundWarningState(path).ignored("docker.service"));

  // A corrupt file starts empty instead of failing.
  std::ofstream(path) << "{not json";
  TEST_CHECK(!BackgroundWarningState(path).alreadyNotified(unitFile, 100));

  std::ofstream(root / "f") << "x";
  TEST_CHECK(fileMtime((root / "f").string()) > 0);
  TEST_CHECK(fileMtime((root / "missing").string()) == 0);

  fs::remove_all(root);
  return 0;
}
