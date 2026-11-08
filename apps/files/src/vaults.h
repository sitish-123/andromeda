#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Encrypted folders (mockup files-v2-live.html §17, Plasma Vaults idea): a gocryptfs cipher folder under the data
// dir that mounts at ~/Vaults/<name> with its password. gocryptfs locks a vault after 10 min idle; a guard process
// (andromeda-files --vault-guard) locks it before the system sleeps.
class Vaults {
public:
  struct Vault {
    std::string name;
    std::filesystem::path cipher;
    std::filesystem::path mount;
  };
  // error is empty on success.
  using Done = std::function<void(const std::string& error)>;

  explicit Vaults(std::string file = "vaults.json");

  [[nodiscard]] const std::vector<Vault>& list() const noexcept { return m_vaults; }
  [[nodiscard]] const Vault* find(const std::string& name) const;
  [[nodiscard]] static bool unlocked(const Vault& vault);
  [[nodiscard]] static bool available();
  // A name the user may give: not empty, no '/', not taken.
  [[nodiscard]] std::string nameError(const std::string& name) const;

  // Makes the cipher folder, saves the vault and unlocks it.
  void create(const std::string& name, const std::string& password, Done done);
  void unlock(const Vault& vault, const std::string& password, Done done);
  void lock(const Vault& vault, Done done);

  // The guard: locks the vault at mount when logind announces sleep, and ends when the vault is locked. Returns the
  // exit code.
  static int guard(const std::filesystem::path& mount);

private:
  void save() const;
  [[nodiscard]] std::filesystem::path storePath() const;

  std::string m_file;
  std::vector<Vault> m_vaults;
};
