#pragma once

#include <filesystem>
#include <string>

// Opens the user's terminal in `dir` (Open Terminal Here): $TERMINAL when set, else xdg-terminal-exec, else
// the first known terminal on PATH. Returns the command used, or "" when none was found or it failed to start.
std::string openTerminalIn(const std::filesystem::path& dir);
