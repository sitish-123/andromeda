#pragma once

// Greeter log output: syslog under greetd, plus the file or console NOCTALIA_GREETER_LOG names.
// Installs itself as the toolkit log sink (core/log.h).
void emergencyLogBootstrap(int argc, char* argv[]);
void initLogging();
[[nodiscard]] const char* loggingPaths();
