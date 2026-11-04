#include "core/process/process.h"
#include "launcher/dmenu_provider.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

int main() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "dmenu_exec_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string pwned = (dir / "pwned").string();
  const std::string out = (dir / "out").string();

  const std::string selection = "a'b\"c $(touch " + pwned + ") `touch " + pwned + "`; touch " + pwned;
  const std::string query = "q $(touch " + pwned + ")";
  const auto result =
      process::runSync(dmenuExecArgv("printf '%s|%s' {selection} {query} > " + out, selection, query));

  TEST_CHECK(result.exitCode == 0);
  TEST_CHECK(!std::filesystem::exists(pwned));
  std::ifstream in(out);
  std::stringstream text;
  text << in.rdbuf();
  TEST_CHECK(text.str() == selection + "|" + query);

  std::filesystem::remove_all(dir);
  return EXIT_SUCCESS;
}
