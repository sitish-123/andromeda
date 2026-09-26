#include "server/ipc_commands.h"

#include "check.h"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>

namespace {

  std::string captureHumanOutput(const umbriel::IpcCommandSpec& spec, const nlohmann::json& ok) {
    FILE* output = std::tmpfile();
    CHECK(output != nullptr);
    if (output == nullptr) {
      return {};
    }

    std::fflush(stdout);
    const int savedStdout = dup(STDOUT_FILENO);
    CHECK(savedStdout >= 0);
    CHECK(dup2(fileno(output), STDOUT_FILENO) >= 0);

    spec.printHuman(ok);
    std::fflush(stdout);

    CHECK(dup2(savedStdout, STDOUT_FILENO) >= 0);
    close(savedStdout);

    CHECK(std::fseek(output, 0, SEEK_END) == 0);
    const long size = std::ftell(output);
    CHECK(size >= 0);
    CHECK(std::fseek(output, 0, SEEK_SET) == 0);

    std::string text(size > 0 ? static_cast<size_t>(size) : 0, '\0');
    CHECK(std::fread(text.data(), 1, text.size(), output) == text.size());
    std::fclose(output);
    return text;
  }

} // namespace

UMBRIEL_TEST(keyboardLayoutsHumanOutputListsAndMarksCurrentLayout) {
  const umbriel::IpcCommandSpec* spec = umbriel::findIpcCommand("keyboard-layouts");
  CHECK(spec != nullptr);
  if (spec == nullptr) {
    return;
  }

  CHECK(spec->printHuman != nullptr);
  if (spec->printHuman == nullptr) {
    return;
  }

  const nlohmann::json layouts = {
      {"names", {"English (US)", "German"}},
      {"current_index", 1},
  };
  CHECK_EQ(captureHumanOutput(*spec, layouts), "  English (US)\n* German\n");
}

int main() { return RUN_TESTS(); }
