// The login keyring that pam_gnome_keyring unlocks at login serves the shell's secret store without a prompt.
// meson runs it under dbus-run-session with a private HOME and a gnome-keyring-daemon started like the PAM module.
#include "security/secret_store.h"
#include "tests/test_check.h"

#include <cstdint>
#include <poll.h>
#include <span>
#include <string>
#include <vector>

namespace {
  bool dispatchCompletion(security::SecretStore& store) {
    std::vector<pollfd> fds;
    const std::size_t start = store.addPollFds(fds);
    if (fds.size() != 1 || ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 10000) != 1) {
      return false;
    }
    store.dispatch(fds, start);
    return true;
  }
} // namespace

int main() {
  security::SecretStore store;
  const security::SecretId id{.scope = "storage", .owner = "encrypted-state", .name = "master-key"};
  const std::string text = "s3cret";

  bool stored = false;
  auto status = security::SecretStoreStatus::BackendError;
  (void)store.store(
      id, security::SecureBuffer(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())),
      "keyring login unlock test", [&](security::SecretStoreStatus s) {
        status = s;
        stored = true;
      }
  );
  TEST_CHECK(dispatchCompletion(store) && stored);
  TEST_CHECK(status == security::SecretStoreStatus::Success);

  bool looked = false;
  std::string value;
  (void)store.lookup(id, [&](security::SecretStoreStatus s, security::SecureBuffer buffer) {
    status = s;
    const auto bytes = buffer.bytes();
    value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    looked = true;
  });
  TEST_CHECK(dispatchCompletion(store) && looked);
  TEST_CHECK(status == security::SecretStoreStatus::Success);
  TEST_CHECK(value == text);
  return 0;
}
