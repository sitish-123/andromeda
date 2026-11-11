#include "link_share.h"

#include "core/log.h"
#include "core/process/process.h"
#include "dbus/session_bus.h"

#include <fcntl.h>
#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("files");
  const sdbus::ServiceName kBusName{"org.andromeda.Link1"};
  const sdbus::ObjectPath kObjectPath{"/org/andromeda/Link1"};
  constexpr auto kInterface = "org.andromeda.Link1";

} // namespace

LinkShare::LinkShare(SessionBus& bus) : m_bus(bus) {
  m_proxy = sdbus::createProxy(m_bus.connection(), kBusName, kObjectPath);
  m_proxy->uponSignal("PropertiesChanged")
      .onInterface("org.freedesktop.DBus.Properties")
      .call([this](const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changed,
                   const std::vector<std::string>&) {
        if (interfaceName == kInterface && changed.contains("Devices")) {
          refresh();
        }
        if (interfaceName == kInterface && changed.contains("DeviceInfo")) {
          refreshInfo();
        }
      });
  m_proxy->uponSignal("TransferProgress").onInterface(kInterface).call(
      [this](const std::string& id, std::uint64_t bytes, std::uint64_t total) {
        if (auto it = m_transfers.find(id); it != m_transfers.end() && it->second.progress) {
          it->second.progress(bytes, total);
        }
      });
  m_proxy->uponSignal("TransferFinished").onInterface(kInterface).call(
      [this](const std::string& id, const std::string& status, const std::vector<std::string>&) {
        auto it = m_transfers.find(id);
        if (it == m_transfers.end()) {
          return;
        }
        Transfer transfer = std::move(it->second);
        m_transfers.erase(it);
        kLog.debug("link transfer {} {}", id, status);
        if (transfer.finished) {
          transfer.finished(status, outcome(status, {}));
        }
      });
  refreshInfo();
  refresh();
}

LinkShare::~LinkShare() = default;

const LinkShare::Device* LinkShare::device(const std::string& id) const {
  for (const Device& d : m_devices) {
    if (d.id == id) {
      return &d;
    }
  }
  return nullptr;
}

std::string LinkShare::outcome(const std::string& status, const std::string& phone) {
  const std::string who = phone.empty() ? std::string("The phone") : phone;
  if (status == "done") {
    return {};
  }
  if (status == "declined") {
    return who + " declined the files.";
  }
  if (status == "no-space") {
    return who + " has no space for the files.";
  }
  if (status == "too-large") {
    return "The files are too large for " + (phone.empty() ? std::string("the phone") : phone) + ".";
  }
  if (status == "busy") {
    return who + " is busy with another transfer.";
  }
  if (status == "cancelled") {
    return "The transfer was stopped.";
  }
  return "The transfer failed.";
}

void LinkShare::watch(const std::string& transferId, Transfer transfer) { m_transfers[transferId] = std::move(transfer); }

void LinkShare::cancel(const std::string& transferId) {
  m_proxy->callMethodAsync("CancelTransfer").onInterface(kInterface).withArguments(transferId).uponReplyInvoke(
      [](std::optional<sdbus::Error>) {});
}

void LinkShare::recentPhotos(const std::string& deviceId, std::uint16_t count,
                             std::function<void(std::vector<Photo>, const std::string&)> done) {
  m_proxy->callMethodAsync("RecentPhotos")
      .onInterface(kInterface)
      .withArguments(deviceId, count)
      .uponReplyInvoke([done = std::move(done), alive = std::weak_ptr<bool>(m_alive)](
                           std::optional<sdbus::Error> error,
                           std::vector<sdbus::Struct<std::string, std::string, std::uint64_t>> list) {
        if (alive.expired()) {
          return;
        }
        if (error.has_value()) {
          const std::string message = error->getMessage();
          done({}, message.find("denied") != std::string::npos ? std::string("denied") : message);
          return;
        }
        std::vector<Photo> photos;
        for (const auto& p : list) {
          photos.push_back({.path = p.get<0>(), .name = p.get<1>(), .time = p.get<2>()});
        }
        done(std::move(photos), {});
      });
}

void LinkShare::thumbnail(const std::string& deviceId, const std::string& path, std::uint16_t size,
                          std::function<void(std::vector<std::uint8_t>)> done) {
  m_proxy->callMethodAsync("Thumbnail")
      .onInterface(kInterface)
      .withArguments(deviceId, path, size)
      .uponReplyInvoke([done = std::move(done), alive = std::weak_ptr<bool>(m_alive)](
                           std::optional<sdbus::Error> error, std::vector<std::uint8_t> png) {
        if (!alive.expired()) {
          done(error.has_value() ? std::vector<std::uint8_t>{} : std::move(png));
        }
      });
}

void LinkShare::savePhoto(const std::string& deviceId, const std::string& path, const std::filesystem::path& folder,
                          std::function<void(const std::filesystem::path&, const std::string&)> done) {
  savePhotoOnce(deviceId, path, folder, std::move(done), false);
}

void LinkShare::savePhotoOnce(const std::string& deviceId, const std::string& path, const std::filesystem::path& folder,
                              std::function<void(const std::filesystem::path&, const std::string&)> done, bool retried) {
  // The daemon saves only paths of its last RecentPhotos answer for the device, and other clients (the Control
  // Center) ask too: on .Rejected, ask for the newest 64 again and retry once.
  auto reply = [this, deviceId, path, folder, retried, done = std::move(done),
                alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error, std::string saved) {
    if (alive.expired()) {
      return;
    }
    if (error.has_value() && !retried && std::string(error->getName()).ends_with(".Rejected")) {
      recentPhotos(deviceId, 64, [this, deviceId, path, folder, done](std::vector<Photo>, const std::string&) {
        savePhotoOnce(deviceId, path, folder, done, true);
      });
      return;
    }
    done(saved, error.has_value() ? error->getMessage() : std::string());
  };
  if (folder.empty()) {
    m_proxy->callMethodAsync("SavePhoto").onInterface(kInterface).withArguments(deviceId, path).uponReplyInvoke(
        std::move(reply));
  } else {
    m_proxy->callMethodAsync("SavePhotoTo")
        .onInterface(kInterface)
        .withArguments(deviceId, path, folder.string())
        .uponReplyInvoke(std::move(reply));
  }
}

void LinkShare::refresh() {
  m_proxy->callMethodAsync("Get")
      .onInterface("org.freedesktop.DBus.Properties")
      .withArguments(std::string{kInterface}, std::string{"Devices"})
      .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error, sdbus::Variant value) {
        if (alive.expired()) {
          return;
        }
        if (error.has_value()) {
          m_devices.clear(); // no Link daemon: no phone rows
          return;
        }
        m_devices.clear();
        for (const auto& d : value.get<std::vector<sdbus::Struct<std::string, std::string, bool>>>()) {
          Device device = m_info.contains(d.get<0>()) ? m_info.at(d.get<0>()) : Device{};
          device.id = d.get<0>();
          device.name = d.get<1>();
          device.connected = d.get<2>();
          m_devices.push_back(std::move(device));
        }
        kLog.debug("link devices {}", m_devices.size());
        if (m_changed) {
          m_changed();
        }
      });
}

void LinkShare::refreshInfo() {
  m_proxy->callMethodAsync("Get")
      .onInterface("org.freedesktop.DBus.Properties")
      .withArguments(std::string{kInterface}, std::string{"DeviceInfo"})
      .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error, sdbus::Variant value) {
        if (alive.expired() || error.has_value()) {
          return;
        }
        m_info.clear();
        for (const auto& [id, fields] : value.get<std::map<std::string, std::map<std::string, sdbus::Variant>>>()) {
          Device info;
          const auto get = [&fields]<typename T>(const char* key, T& out) {
            if (auto it = fields.find(key); it != fields.end() && it->second.containsValueOfType<T>()) {
              out = it->second.get<T>();
            }
          };
          get("battery", info.battery);
          get("transport", info.transport);
          get("last_seen", info.lastSeen);
          get("storage_free", info.storageFree);
          get("storage_total", info.storageTotal);
          m_info.emplace(id, std::move(info));
        }
        for (Device& d : m_devices) {
          if (auto it = m_info.find(d.id); it != m_info.end()) {
            d.battery = it->second.battery;
            d.transport = it->second.transport;
            d.lastSeen = it->second.lastSeen;
            d.storageFree = it->second.storageFree;
            d.storageTotal = it->second.storageTotal;
          }
        }
        if (m_changed) {
          m_changed();
        }
      });
}

void LinkShare::mirror(const std::string& deviceId) const {
  const Device* d = device(deviceId);
  if (!process::runAsync(std::vector<std::string>{"andromeda-link-mirror", deviceId, "--name", d ? d->name : deviceId})) {
    kLog.warn("could not start andromeda-link-mirror");
  }
}

void LinkShare::send(const std::string& deviceId, const std::vector<std::filesystem::path>& files,
                     std::function<void(const std::string& transferId, const std::string& error)> done) {
  std::vector<sdbus::Struct<sdbus::UnixFd, std::string>> sources;
  for (const auto& path : files) {
    // O_NONBLOCK keeps open(2) from waiting on a FIFO or device on the UI thread.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
      done({}, path.filename().string() + ": cannot be read");
      return;
    }
    sdbus::UnixFd owned{fd, sdbus::adopt_fd};
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      done({}, path.filename().string() + ": not a regular file");
      return;
    }
    sources.emplace_back(std::move(owned), path.filename().string());
  }
  kLog.debug("share {} files to {}", files.size(), deviceId);
  m_proxy->callMethodAsync("SendFiles")
      .onInterface(kInterface)
      .withArguments(deviceId, sources)
      .uponReplyInvoke([done = std::move(done), alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error,
                                                                                       std::string transferId) {
        if (!alive.expired()) {
          done(transferId, error.has_value() ? error->getMessage() : std::string());
        }
      });
}
