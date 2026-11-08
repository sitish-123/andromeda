#pragma once

#include <filesystem>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
}

// Share › <phone> (mockup §4): sends files to a paired phone through the Link daemon (org.andromeda.Link1 SendFiles).
// The device list is cached from the Devices property and its changes, so building a menu makes no D-Bus call.
class LinkShare {
public:
  struct Device {
    std::string id;
    std::string name;
    bool connected = false;
    // From DeviceInfo; -1 or 0 until the phone reports them.
    int battery = -1;
    std::string transport; // wifi or bluetooth while connected
    std::int64_t lastSeen = 0; // Unix seconds
    std::uint64_t storageFree = 0;
    std::uint64_t storageTotal = 0;
  };

  explicit LinkShare(SessionBus& bus);
  ~LinkShare();
  LinkShare(const LinkShare&) = delete;
  LinkShare& operator=(const LinkShare&) = delete;

  struct Photo {
    std::string path; // on the phone: the key for Thumbnail and SavePhoto
    std::string name;
    std::uint64_t time = 0; // Unix seconds
  };
  // A transfer this window started: progress and the end, with the daemon's status mapped to a sentence.
  struct Transfer {
    std::function<void(std::uint64_t bytes, std::uint64_t total)> progress;
    std::function<void(const std::string& status, const std::string& message)> finished;
  };

  [[nodiscard]] const std::vector<Device>& devices() const noexcept { return m_devices; }
  [[nodiscard]] const Device* device(const std::string& id) const;
  // Called after the device list changes.
  void setChanged(std::function<void()> changed) { m_changed = std::move(changed); }
  // error "denied": the phone holds back photo access; other text: no answer.
  void recentPhotos(const std::string& deviceId, std::uint16_t count,
                    std::function<void(std::vector<Photo> photos, const std::string& error)> done);
  void thumbnail(const std::string& deviceId, const std::string& path, std::uint16_t size,
                 std::function<void(std::vector<std::uint8_t> png)> done);
  // Saves a photo of the last recentPhotos answer into folder (empty = Downloads).
  void savePhoto(const std::string& deviceId, const std::string& path, const std::filesystem::path& folder,
                 std::function<void(const std::filesystem::path& saved, const std::string& error)> done);
  void cancel(const std::string& transferId);
  // Starts the phone's screen mirror window (andromeda-link-mirror).
  void mirror(const std::string& deviceId) const;
  // Watches a transfer id returned by send.
  void watch(const std::string& transferId, Transfer transfer);
  // The sentence for a TransferFinished status; empty for "done".
  [[nodiscard]] static std::string outcome(const std::string& status, const std::string& phone);
  // done(transfer id, error): error empty on success (the phone then asks to accept the transfer).
  void send(const std::string& deviceId, const std::vector<std::filesystem::path>& files,
            std::function<void(const std::string& transferId, const std::string& error)> done);

private:
  void refresh();
  void refreshInfo();
  void savePhotoOnce(const std::string& deviceId, const std::string& path, const std::filesystem::path& folder,
                     std::function<void(const std::filesystem::path&, const std::string&)> done, bool retried);

  SessionBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_proxy;
  std::vector<Device> m_devices;
  std::map<std::string, Device> m_info; // DeviceInfo by id, merged into m_devices
  std::function<void()> m_changed;
  std::map<std::string, Transfer> m_transfers;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
