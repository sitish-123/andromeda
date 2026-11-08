#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Cloud accounts (FILES-UI-SPEC §21) through rclone: remotes from rclone.conf, mounted at ~/Cloud/<name> with the VFS
// full cache, per-file state from that cache and the upload queue, Keep on This Device / Free Up Space, share links,
// and Add Account with rclone's browser sign-in.
class Cloud {
public:
  struct Remote {
    std::string name;
    std::string type; // rclone backend: drive, onedrive, dropbox, webdav, ...
  };
  enum class State : std::uint8_t { None, OnDevice, CloudOnly, Syncing };
  struct Provider {
    const char* type;
    const char* label;
    bool oauth; // false: URL, user and password fields
  };
  using Done = std::function<void(const std::string& error)>;

  Cloud();
  ~Cloud();
  Cloud(const Cloud&) = delete;
  Cloud& operator=(const Cloud&) = delete;

  [[nodiscard]] static bool available(); // rclone in PATH
  [[nodiscard]] static const std::vector<Provider>& providers();
  // Reads rclone.conf again (cheap INI parse).
  void reload();
  [[nodiscard]] const std::vector<Remote>& remotes() const noexcept { return m_remotes; }
  [[nodiscard]] static std::filesystem::path mountPoint(const std::string& remote);
  [[nodiscard]] static bool mounted(const std::string& remote);
  // The remote whose mount holds path, or empty.
  [[nodiscard]] std::string remoteOf(const std::filesystem::path& path) const;

  void mount(const std::string& remote, Done done);
  void unmount(const std::string& remote, Done done);

  // State of a file in a mounted remote (None for folders and paths outside a mount).
  [[nodiscard]] State state(const std::filesystem::path& path) const;
  // Asks rclone for the upload queue of the remote; changed() runs when the answer changes the states.
  void refreshQueue(const std::string& remote, std::function<void()> changed);
  // Reads the files fully through the mount on a worker (folders: every file under them) and pins them.
  void keep(const std::vector<std::filesystem::path>& paths, std::function<void(std::size_t files, const std::string& error)> done);
  // Drops cached data of files neither open nor queued, unpins, and makes rclone forget the folders.
  void freeUp(const std::vector<std::filesystem::path>& paths, std::function<void(std::size_t files)> done);
  // A public link through `rclone link`.
  void shareLink(const std::filesystem::path& path, std::function<void(const std::string& url, const std::string& error)> done);

  // Creates a remote; OAuth providers open the browser and finish when the user signs in. fields: url, user, pass.
  // ask: rclone needs one more answer (iCloud's two-factor code); call answer(text), or answer({}) to stop.
  using Ask = std::function<void(const std::string& question, std::function<void(const std::string* text)> answer)>;
  void addAccount(const std::string& name, const std::string& type, const std::map<std::string, std::string>& fields,
                  Ask ask, Done done);
  void cancelAdd(); // stops a pending sign-in and deletes the half-made remote
  void removeAccount(const std::string& name, Done done);
  [[nodiscard]] std::string nameError(const std::string& name) const;

private:
  [[nodiscard]] std::filesystem::path cacheFile(const std::string& remote, const std::filesystem::path& rel) const;
  void loadPins();
  void savePins() const;

  std::vector<Remote> m_remotes;
  std::map<std::string, std::set<std::string>> m_queued; // remote -> paths inside it waiting to upload
  std::set<std::string> m_pins;                            // absolute paths kept on this device
  std::string m_adding;                                    // remote name of a sign-in in progress
  int m_addPid = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
