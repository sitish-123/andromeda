#include "server/desktop_settings_manager.h"

#include "config/config.h"
#include "config/keybind_parse.h"
#include "config/managed_settings.h"
#include "desktop-unstable-v1-protocol.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

#include <xkbcommon/xkbcommon.h>

namespace umbriel {

  namespace {

    constexpr uint32_t kVersion = 1;

    DesktopSettingsManager* managerFrom(wl_resource* resource) {
      return static_cast<DesktopSettingsManager*>(wl_resource_get_user_data(resource));
    }

    std::string readFile(const std::filesystem::path& path) {
      std::ifstream in(path);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

  } // namespace

  DesktopSettingsManager::DesktopSettingsManager(Server& server) : m_server(server) {
    m_global = wl_global_create(server.display(), &dsk_settings_manager_v1_interface, kVersion, this, bind);
    m_devices = server.inputDevices();
  }

  DesktopSettingsManager::~DesktopSettingsManager() {
    for (wl_resource* resource : m_resources) {
      wl_resource_set_user_data(resource, nullptr);
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void DesktopSettingsManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<DesktopSettingsManager*>(data);
    wl_resource* resource =
        wl_resource_create(client, &dsk_settings_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kImplementation, self, handleResourceDestroyed);
    self->m_resources.push_back(resource);
    for (const InputDeviceInfo& device : self->m_devices) {
      dsk_settings_manager_v1_send_device_added(resource, device.name.c_str(), device.kind);
    }
    for (const ActionSpec& spec : actionSpecs()) {
      const std::string name(spec.name);
      const std::string param(spec.param);
      const std::string summary(spec.summary);
      dsk_settings_manager_v1_send_action_spec(resource, name.c_str(), param.c_str(), summary.c_str());
    }
    self->sendSettings(resource);
    self->sendKeybinds(resource);
    dsk_settings_manager_v1_send_done(resource);
  }

  void DesktopSettingsManager::handleSet(wl_client*, wl_resource* resource, const char* key, const char* value) {
    if (DesktopSettingsManager* self = managerFrom(resource)) {
      self->set(resource, key, value);
    }
  }

  void DesktopSettingsManager::handleBind(wl_client*, wl_resource* resource, const char* chord, const char* action) {
    if (DesktopSettingsManager* self = managerFrom(resource)) {
      self->bindChord(resource, chord, action);
    }
  }

  void DesktopSettingsManager::handleCaptureChord(wl_client*, wl_resource* resource) {
    if (DesktopSettingsManager* self = managerFrom(resource)) {
      if (self->m_captureResource != nullptr && self->m_captureResource != resource) {
        dsk_settings_manager_v1_send_chord_captured(self->m_captureResource, "");
      }
      self->m_captureResource = resource;
    }
  }

  void DesktopSettingsManager::handleCancelCapture(wl_client*, wl_resource* resource) {
    if (DesktopSettingsManager* self = managerFrom(resource); self != nullptr && self->m_captureResource == resource) {
      self->finishCapture({});
    }
  }

  void DesktopSettingsManager::handleDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

  void DesktopSettingsManager::handleResourceDestroyed(wl_resource* resource) {
    if (DesktopSettingsManager* self = managerFrom(resource)) {
      std::erase(self->m_resources, resource);
      if (self->m_captureResource == resource) {
        self->m_captureResource = nullptr;
      }
    }
  }

  void DesktopSettingsManager::set(wl_resource* resource, std::string_view key, std::string_view value) {
    const std::string keyText(key);
    const auto reject = [&](const std::string& reason) {
      dsk_settings_manager_v1_send_failed(resource, keyText.c_str(), reason.c_str());
    };
    const ManagedSettingParse parsed = parseManagedSetting(key, value);
    if (!parsed.value) {
      reject(parsed.error);
      return;
    }
    const std::filesystem::path file = managedSettingsFile(configRootPath());
    if (!saveManagedSetting(file, key, *parsed.value)) {
      reject("cannot write " + file.string());
      return;
    }
    // Apply now rather than when the watcher notices the write; its later reload finds nothing changed.
    m_server.handleConfigReload();
  }

  void DesktopSettingsManager::bindChord(wl_resource* resource, std::string_view chord, std::string_view action) {
    const std::string chordText(chord);
    const auto reject = [&](const std::string& reason) {
      dsk_settings_manager_v1_send_failed(resource, chordText.c_str(), reason.c_str());
    };
    Keybind parsed;
    if (!parseChord(chord, parsed)) {
      reject(std::format("'{}' is not a chord", chord));
      return;
    }
    if (!action.empty() && action != kUnboundAction && !parseAction(action, parsed)) {
      reject(std::format("'{}' is not an action", action));
      return;
    }
    // One entry per trigger: drop any spelling of the same chord already saved ("super+q" and "Super+Q").
    const std::filesystem::path file = managedSettingsFile(configRootPath());
    for (const auto& [savedChord, savedAction] : documentKeybinds(readFile(file))) {
      Keybind saved;
      if (savedChord != chord && parseChord(savedChord, saved) && sameChord(saved, parsed)
          && !saveManagedKeybind(file, savedChord, {})) {
        reject("cannot write " + file.string());
        return;
      }
    }
    if (!saveManagedKeybind(file, chord, action)) {
      reject("cannot write " + file.string());
      return;
    }
    m_server.handleConfigReload();
  }

  void DesktopSettingsManager::sendKeybinds(wl_resource* resource) const {
    std::vector<Keybind> saved;
    std::vector<Keybind> unbound;
    for (const auto& [chord, action] : documentKeybinds(readFile(managedSettingsFile(configRootPath())))) {
      Keybind bind;
      if (parseChord(chord, bind)) {
        (action == kUnboundAction ? unbound : saved).push_back(std::move(bind));
      }
    }
    const auto in = [](const std::vector<Keybind>& binds, const Keybind& bind) {
      return std::ranges::any_of(binds, [&](const Keybind& other) { return sameChord(other, bind); });
    };
    for (const Keybind& bind : config().keybinds) {
      const std::string chord = formatChord(bind);
      const std::string action = formatAction(bind);
      if (chord.empty() || action.empty()) {
        continue;
      }
      dsk_settings_manager_v1_send_keybind(resource, chord.c_str(), action.c_str(), in(saved, bind) ? 1U : 0U);
    }
    for (const Keybind& bind : unbound) {
      const std::string chord = formatChord(bind);
      const std::string action(kUnboundAction);
      dsk_settings_manager_v1_send_keybind(resource, chord.c_str(), action.c_str(), 1U);
    }
  }

  void DesktopSettingsManager::captureChord(uint32_t keysym, uint32_t modifiers) {
    const uint32_t held = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    if (held == 0 && keysym == XKB_KEY_Escape) {
      finishCapture({});
      return;
    }
    Keybind bind;
    bind.keysym = xkb_keysym_to_lower(keysym);
    bind.modifiers = held;
    const uint32_t mod = m_server.modKey();
    if (mod != 0 && (held & mod) == mod) {
      bind.useMod = true;
      bind.modifiers &= ~mod;
    }
    finishCapture(formatChord(bind));
  }

  void DesktopSettingsManager::finishCapture(std::string_view chord) {
    wl_resource* resource = std::exchange(m_captureResource, nullptr);
    if (resource != nullptr) {
      const std::string text(chord);
      dsk_settings_manager_v1_send_chord_captured(resource, text.c_str());
    }
  }

  void DesktopSettingsManager::sendSettings(wl_resource* resource) const {
    const std::string saved = readFile(managedSettingsFile(configRootPath()));
    for (const std::string_view key : managedSettingKeys()) {
      const std::string keyText(key);
      const std::string value = managedSettingValue(config(), key);
      const uint32_t customized = documentSetsKey(saved, key) ? 1U : 0U;
      dsk_settings_manager_v1_send_setting(resource, keyText.c_str(), value.c_str(), customized);
    }
  }

  void DesktopSettingsManager::settingsChanged() {
    for (wl_resource* resource : m_resources) {
      sendSettings(resource);
      sendKeybinds(resource);
      dsk_settings_manager_v1_send_done(resource);
    }
  }

  void DesktopSettingsManager::devicesChanged() {
    std::vector<InputDeviceInfo> devices = m_server.inputDevices();
    if (devices == m_devices) {
      return;
    }
    for (wl_resource* resource : m_resources) {
      for (const InputDeviceInfo& old : m_devices) {
        if (std::ranges::find(devices, old) == devices.end()) {
          dsk_settings_manager_v1_send_device_removed(resource, old.name.c_str());
        }
      }
      for (const InputDeviceInfo& device : devices) {
        if (std::ranges::find(m_devices, device) == m_devices.end()) {
          dsk_settings_manager_v1_send_device_added(resource, device.name.c_str(), device.kind);
        }
      }
      dsk_settings_manager_v1_send_done(resource);
    }
    m_devices = std::move(devices);
  }

  const struct dsk_settings_manager_v1_interface DesktopSettingsManager::kImplementation = {
      .destroy = DesktopSettingsManager::handleDestroy,
      .set = DesktopSettingsManager::handleSet,
      .bind = DesktopSettingsManager::handleBind,
      .capture_chord = DesktopSettingsManager::handleCaptureChord,
      .cancel_capture = DesktopSettingsManager::handleCancelCapture,
  };

} // namespace umbriel
