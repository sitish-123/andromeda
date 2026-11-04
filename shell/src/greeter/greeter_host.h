#pragma once

// `noctalia greeter`: the login screen under `andromeda --greeter` (.agent/DESIGN.md "Shell greeter mode"). It never
// builds Application: no user config, plugins, IPC socket or session bus.
int runGreeterHost(int argc, char* argv[]);
