#include "server/desktop_input_manager.h"

#include "config/config.h"
#include "config/input_settings.h"
#include "desktop-unstable-v1-protocol.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace umbriel {

  namespace {

    constexpr uint32_t kVersion = 1;

    DesktopInputManager* managerFrom(wl_resource* resource) {
      return static_cast<DesktopInputManager*>(wl_resource_get_user_data(resource));
    }

    std::filesystem::path generatedFile() { return configRootPath().parent_path() / "input.toml"; }

    std::string readFile(const std::filesystem::path& path) {
      std::ifstream in(path);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

  } // namespace

  DesktopInputManager::DesktopInputManager(Server& server) : m_server(server) {
    m_global = wl_global_create(server.display(), &dsk_input_manager_v1_interface, kVersion, this, bind);
    m_devices = server.inputDevices();
  }

  DesktopInputManager::~DesktopInputManager() {
    for (wl_resource* resource : m_resources) {
      wl_resource_set_user_data(resource, nullptr);
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void DesktopInputManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<DesktopInputManager*>(data);
    wl_resource* resource = wl_resource_create(client, &dsk_input_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kImplementation, self, handleResourceDestroyed);
    self->m_resources.push_back(resource);
    for (const InputDeviceInfo& device : self->m_devices) {
      dsk_input_manager_v1_send_device_added(resource, device.name.c_str(), device.kind);
    }
    self->sendSettings(resource);
    dsk_input_manager_v1_send_done(resource);
  }

  void DesktopInputManager::handleSet(wl_client*, wl_resource* resource, const char* key, const char* value) {
    if (DesktopInputManager* self = managerFrom(resource)) {
      self->set(resource, key, value);
    }
  }

  void DesktopInputManager::handleDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

  void DesktopInputManager::handleResourceDestroyed(wl_resource* resource) {
    if (DesktopInputManager* self = managerFrom(resource)) {
      std::erase(self->m_resources, resource);
    }
  }

  void DesktopInputManager::set(wl_resource* resource, std::string_view key, std::string_view value) {
    const std::string keyText(key);
    const auto reject = [&](const std::string& reason) {
      dsk_input_manager_v1_send_failed(resource, keyText.c_str(), reason.c_str());
    };
    const InputSettingParse parsed = parseInputSetting(key, value);
    if (!parsed.value) {
      reject(parsed.error);
      return;
    }
    if (documentSetsKey(readFile(configRootPath()), key)) {
      reject("set in " + configRootPath().filename().string() + ", which overrides changes made here");
      return;
    }
    const std::filesystem::path file = generatedFile();
    const std::vector<std::filesystem::path> watched = configWatchPaths();
    if (std::ranges::find(watched, file) == watched.end()) {
      reject(configRootPath().filename().string() + " does not include input.toml");
      return;
    }
    if (!saveInputSetting(file, key, *parsed.value)) {
      reject("cannot write " + file.string());
      return;
    }
    // Apply now rather than when the watcher notices the write; its later reload finds nothing changed.
    m_server.handleConfigReload();
  }

  void DesktopInputManager::sendSettings(wl_resource* resource) const {
    const std::string root = readFile(configRootPath());
    for (const std::string_view key : inputSettingKeys()) {
      const std::string keyText(key);
      const std::string value = inputSettingValue(config(), key);
      dsk_input_manager_v1_send_setting(resource, keyText.c_str(), value.c_str(), documentSetsKey(root, key) ? 1U : 0U);
    }
  }

  void DesktopInputManager::settingsChanged() {
    for (wl_resource* resource : m_resources) {
      sendSettings(resource);
      dsk_input_manager_v1_send_done(resource);
    }
  }

  void DesktopInputManager::devicesChanged() {
    std::vector<InputDeviceInfo> devices = m_server.inputDevices();
    if (devices == m_devices) {
      return;
    }
    for (wl_resource* resource : m_resources) {
      for (const InputDeviceInfo& old : m_devices) {
        if (std::ranges::find(devices, old) == devices.end()) {
          dsk_input_manager_v1_send_device_removed(resource, old.name.c_str());
        }
      }
      for (const InputDeviceInfo& device : devices) {
        if (std::ranges::find(m_devices, device) == m_devices.end()) {
          dsk_input_manager_v1_send_device_added(resource, device.name.c_str(), device.kind);
        }
      }
      dsk_input_manager_v1_send_done(resource);
    }
    m_devices = std::move(devices);
  }

  const struct dsk_input_manager_v1_interface DesktopInputManager::kImplementation = {
      .destroy = DesktopInputManager::handleDestroy,
      .set = DesktopInputManager::handleSet,
  };

} // namespace umbriel
