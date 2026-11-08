#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

typedef struct _GVolumeMonitor GVolumeMonitor;

// Drives the system knows but has not mounted (USB sticks, encrypted partitions, phones over MTP, cameras over
// PTP), through the GIO volume monitor (gvfs-udisks2, gvfs-mtp, gvfs-gphoto2). Mounted block drives come from
// listVolumes(); mounted phones and cameras (no /dev device) come from here.
class Drives {
public:
  enum class Kind : std::uint8_t { Drive, Usb, Phone, Camera };
  struct Drive {
    std::string id;     // stable while plugged in: the volume's unix device or uuid, else its name
    std::string name;
    Kind kind = Kind::Drive;
    bool mounted = false;
    std::filesystem::path root; // set when mounted
    bool encrypted = false;
    std::string device; // /dev node, for Format; empty for MTP/PTP
  };
  // error empty = mounted at root.
  using Mounted = std::function<void(const std::filesystem::path& root, const std::string& error)>;
  // The drive asks for a password: call answer(password), or answer({}) to cancel.
  using AskPassword = std::function<void(const std::string& name, const std::string& message,
                                         std::function<void(const std::string* password)> answer)>;

  explicit Drives(std::function<void()> changed);
  ~Drives();
  Drives(const Drives&) = delete;
  Drives& operator=(const Drives&) = delete;

  [[nodiscard]] std::vector<Drive> list() const;
  void mount(const std::string& id, AskPassword ask, Mounted done);
  // An encrypted drive: unlock with the passphrase through udisks2, then mount the unlocked file system. Asking
  // first keeps out gvfs's Secret Service lookup, which waits 25 s for the D-Bus timeout without a keyring.
  static void unlock(const std::string& device, const std::string& passphrase, Mounted done);

  // Format through udisks2 (polkit asks when needed); type = "exfat", "vfat", "ext4" or "ntfs".
  static void format(const std::string& device, const std::string& type, const std::string& label,
                     std::function<void(const std::string& error)> done);
  // The file systems udisks2 can make here, from Manager.CanFormat.
  [[nodiscard]] static std::vector<std::string> formats();
  // The block device behind a mounted folder, from /proc/self/mounts; empty when none.
  [[nodiscard]] static std::string deviceOf(const std::filesystem::path& mount);

private:
  GVolumeMonitor* m_monitor = nullptr;
  std::function<void()> m_changed;
};

// Import Photos (Finder/Photos-style, GNOME Shotwell idea) from any folder with a DCIM tree: phones, cameras, SD
// cards. Each imported file is logged by drive + path + size + mtime, so a second import copies only new files.
class PhotoImport {
public:
  explicit PhotoImport(std::string file = "imports.json");
  // The DCIM folder at or under root (one level), or empty.
  [[nodiscard]] static std::filesystem::path dcim(const std::filesystem::path& root);
  // Photos and videos under dcim that the log does not hold yet.
  [[nodiscard]] std::vector<std::filesystem::path> pending(const std::filesystem::path& dcim) const;
  // Marks files as imported after the copy job finished.
  void remember(const std::vector<std::filesystem::path>& files);
  // ~/Pictures/<YYYY-MM-DD>: today's import folder.
  [[nodiscard]] static std::filesystem::path destination();

private:
  [[nodiscard]] static std::string key(const std::filesystem::path& file);
  void save() const;

  std::string m_file;
  std::vector<std::string> m_done;
};
