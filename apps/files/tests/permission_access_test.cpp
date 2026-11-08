#include "permission_access.h"

#include <array>
#include <print>
#include <string>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "permission_access_test: {}", what);
      ++g_failures;
    }
  }
} // namespace

int main() {
  expect(accessOf(0640, PermClass::Owner) == Access::ReadWrite, "rw- reads as Read and write");
  expect(accessOf(0640, PermClass::Group) == Access::Read, "r-- reads as Read only");
  expect(accessOf(0640, PermClass::Everyone) == Access::None, "--- reads as No access");
  expect(accessOf(0020, PermClass::Group) == Access::WriteOnly, "-w- is shown as it is, not as No access");

  // A choice changes only its own class: the mask covers that class's read and write bits and nothing else.
  expect(accessBits(PermClass::Everyone, Access::Read) == std::pair<std::uint32_t, std::uint32_t>{0004, 0006},
         "Everyone: Read only");
  expect(accessBits(PermClass::Group, Access::ReadWrite) == std::pair<std::uint32_t, std::uint32_t>{0060, 0060},
         "Group: Read and write");
  expect(accessBits(PermClass::Owner, Access::None) == std::pair<std::uint32_t, std::uint32_t>{0000, 0600},
         "Owner: No access");

  expect(runBits(0644, true) == std::pair<std::uint32_t, std::uint32_t>{0111, 0111}, "run follows who can read");
  expect(runBits(0640, true) == std::pair<std::uint32_t, std::uint32_t>{0110, 0111}, "no run for a class that can't read");
  expect(runBits(0755, false) == std::pair<std::uint32_t, std::uint32_t>{0000, 0111}, "off clears every x");

  const std::array<std::uint32_t, 2> same{0644, 0604};
  const std::array<std::uint32_t, 2> mixed{0644, 0600};
  expect(commonAccess(same, PermClass::Everyone) == Access::Read, "a shared value shows for many items");
  expect(!commonAccess(mixed, PermClass::Everyone).has_value(), "different values read as Mixed");
  return g_failures == 0 ? 0 : 1;
}
