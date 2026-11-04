#pragma once

// `noctalia lock-helper --start-fd N`: renders plugin lock widgets out of the shell process
// (.agent/DESIGN.md "Lock helper (L5)"). The start fd holds a TOML document:
//   dark, time_format, date_format, plugin_sources = [dirs], [plugin_settings."<plugin id>"],
//   [[slot]] fd, type, width, height (logical px), scale, content_scale, [slot.settings].
// Each slot fd is one end of a SOCK_SEQPACKET socketpair that serves the pixel stream protocol.
// The helper exits when stdin reaches EOF or a slot closes.
int runLockHelper(int argc, char* argv[]);
