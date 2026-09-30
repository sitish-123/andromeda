#include "config/output_identity.h"

#include <cstdio>
#include <cstring>

namespace {
  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "FAIL: %s\n", message);
      ++failures;
    }
  }
} // namespace

int main() {
  char identifier[512];
  expect(
      greeter_output_stable_identifier(
          "DP-3", "Dell Inc.", "DELL U2723QE", "ABC123", "Dell Inc. DELL U2723QE ABC123 (DP-3)", identifier,
          sizeof(identifier)
      ),
      "description-based stable identifier is available"
  );
  expect(
      std::strcmp(identifier, "Dell Inc. DELL U2723QE ABC123") == 0,
      "connector suffix is removed from stable identifier"
  );
  expect(
      greeter_output_identifier_matches(
          "DP-3", "Dell Inc.", "DELL U2723QE", "ABC123", "Dell Inc. DELL U2723QE ABC123 (DP-3)", "DP-3"
      ),
      "connector identifier matches"
  );
  expect(
      greeter_output_identifier_matches(
          "DP-3", "Dell Inc.", "DELL U2723QE", "ABC123", "Dell Inc. DELL U2723QE ABC123 (DP-3)",
          "Dell Inc. DELL U2723QE ABC123"
      ),
      "stable identifier matches"
  );
  expect(
      greeter_output_identifier_matches(
          "DP-3", "Dell Inc.", "DELL U2723QE", "ABC123", nullptr, "Dell Inc. DELL U2723QE"
      ),
      "make and model match when the client cannot see the serial"
  );
  expect(
      !greeter_output_identifier_matches("DP-3", "Dell Inc.", "DELL U2723QE", "ABC123", nullptr, "DP-1"),
      "different connector does not match"
  );
  expect(greeter_output_name_is_internal("eDP-1"), "eDP output is internal");
  expect(greeter_output_name_is_internal("LVDS-1"), "LVDS output is internal");
  expect(!greeter_output_name_is_internal("DP-1"), "DisplayPort output is external");

  return failures == 0 ? 0 : 1;
}
