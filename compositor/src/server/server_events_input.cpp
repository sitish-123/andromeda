#include "qos/controller.h"
#include "config/change.h"
#include "config/config.h"
#include "config/config_watcher.h"
#include "config/store.h"
#include "core/log.h"
#include "desktop-unstable-v1-protocol.h"
#include "input/cursor.h"
#include "input/eis.h"
#include "input/gestures.h"
#include "input/input_capture.h"
#include "input/keyboard.h"
#include "input/keyboard_leds.h"
#include "input/seat.h"
#include "input/system_keymap.h"
#include "input/text_input.h"
#include "layer/layer_surface.h"
#include "layout/scrolling.h"
#include "lock/session_lock.h"
#include "output/display_store.h"
#include "output/identity.h"
#include "output/mirror.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/effect_registry.h"
#include "scene/hint_rect.h"
#include "scene/quit_confirm.h"
#include "server/allocator.h"
#include "server/backend_manager.h"
#include "server/desktop_output_manager.h"
#include "server/desktop_settings_manager.h"
#include "server/desktop_shell.h"
#include "server/ipc.h"
#include "server/server.h"
#include "server/server_events_internal.h"
#include "view/popup.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace andromeda {

  namespace {
    constexpr Logger kLog("server");
    using server_events::applyPointerConfig;

    const char* deviceName(const wlr_input_device* device) {
      return device->name != nullptr ? device->name : "unknown";
    }

    void applyNaturalScroll(
        libinput_device* libinputDevice, const wlr_input_device* device, const std::optional<bool>& configured,
        std::string_view setting
    ) {
      if (libinput_device_config_scroll_has_natural_scroll(libinputDevice) == 0) {
        if (configured) {
          kLog.warn("input: '{}' does not support {}", deviceName(device), setting);
        }
        return;
      }
      const bool enabled =
          configured.value_or(libinput_device_config_scroll_get_default_natural_scroll_enabled(libinputDevice) != 0);
      if (libinput_device_config_scroll_set_natural_scroll_enabled(libinputDevice, enabled)
          != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        kLog.warn("input: failed to apply {} to '{}'", setting, deviceName(device));
      }
    }

    void applyLeftHanded(
        libinput_device* libinputDevice, const wlr_input_device* device, const std::optional<bool>& configured,
        std::string_view setting
    ) {
      if (libinput_device_config_left_handed_is_available(libinputDevice) == 0) {
        if (configured) {
          kLog.warn("input: '{}' does not support {}", deviceName(device), setting);
        }
        return;
      }
      const bool enabled = configured.value_or(libinput_device_config_left_handed_get_default(libinputDevice) != 0);
      if (libinput_device_config_left_handed_set(libinputDevice, enabled) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        if (configured) {
          kLog.warn("input: failed to apply {} to '{}'", setting, deviceName(device));
        } else {
          kLog.warn("input: failed to restore the default left-handed state for '{}'", deviceName(device));
        }
      }
    }

    void applyScrollMethod(
        libinput_device* libinputDevice, const wlr_input_device* device, std::optional<ScrollMethod> configured,
        std::string_view setting
    ) {
      const uint32_t methods = libinput_device_config_scroll_get_methods(libinputDevice);
      if (methods == 0) {
        if (configured) {
          kLog.warn("input: '{}' does not support {}", deviceName(device), setting);
        }
        return;
      }
      // applyScrollButton() runs first and owns on-button-down scrolling; restoring the default here would undo it.
      if (!configured
          && libinput_device_config_scroll_get_method(libinputDevice) == LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN) {
        return;
      }
      enum libinput_config_scroll_method method = libinput_device_config_scroll_get_default_method(libinputDevice);
      if (configured) {
        enum libinput_config_scroll_method requested = LIBINPUT_CONFIG_SCROLL_2FG;
        const char* requestedName = "two_finger";
        switch (*configured) {
        case ScrollMethod::TwoFinger:
          break;
        case ScrollMethod::Edge:
          requested = LIBINPUT_CONFIG_SCROLL_EDGE;
          requestedName = "edge";
          break;
        case ScrollMethod::None:
          requested = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
          requestedName = "none";
          break;
        }
        if (requested != LIBINPUT_CONFIG_SCROLL_NO_SCROLL && (methods & requested) == 0) {
          kLog.warn("input: '{}' does not support the {} scroll method", deviceName(device), requestedName);
          return;
        }
        method = requested;
      }
      if (libinput_device_config_scroll_set_method(libinputDevice, method) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        if (configured) {
          kLog.warn("input: failed to apply {} to '{}'", setting, deviceName(device));
        } else {
          kLog.warn("input: failed to restore the default scroll method for '{}'", deviceName(device));
        }
      }
    }

    void applyClickMethod(
        libinput_device* libinputDevice, const wlr_input_device* device, std::optional<ClickMethod> configured,
        std::string_view setting
    ) {
      const uint32_t methods = libinput_device_config_click_get_methods(libinputDevice);
      if (methods == 0) {
        if (configured) {
          kLog.warn("input: '{}' does not support {}", deviceName(device), setting);
        }
        return;
      }
      enum libinput_config_click_method method = libinput_device_config_click_get_default_method(libinputDevice);
      if (configured) {
        enum libinput_config_click_method requested = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
        const char* requestedName = "button_areas";
        switch (*configured) {
        case ClickMethod::ButtonAreas:
          break;
        case ClickMethod::ClickFinger:
          requested = LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
          requestedName = "clickfinger";
          break;
        }
        if ((methods & requested) == 0) {
          kLog.warn("input: '{}' does not support the {} click method", deviceName(device), requestedName);
          return;
        }
        method = requested;
      }
      if (libinput_device_config_click_set_method(libinputDevice, method) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        if (configured) {
          kLog.warn("input: failed to apply {} to '{}'", setting, deviceName(device));
        } else {
          kLog.warn("input: failed to restore the default click method for '{}'", deviceName(device));
        }
      }
    }

    void applyTapButtonMap(
        libinput_device* libinputDevice, const wlr_input_device* device, std::optional<TapButtonMap> configured,
        std::string_view setting
    ) {
      if (!configured) {
        if (libinput_device_config_tap_set_button_map(
                libinputDevice, libinput_device_config_tap_get_default_button_map(libinputDevice)
            )
            != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn("input: failed to restore the default tap button map for '{}'", deviceName(device));
        }
        return;
      }
      enum libinput_config_tap_button_map requested = LIBINPUT_CONFIG_TAP_MAP_LRM;
      switch (*configured) {
      case TapButtonMap::LeftRightMiddle:
        break;
      case TapButtonMap::LeftMiddleRight:
        requested = LIBINPUT_CONFIG_TAP_MAP_LMR;
        break;
      }
      if (libinput_device_config_tap_set_button_map(libinputDevice, requested) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        kLog.warn("input: failed to apply {} to '{}'", setting, deviceName(device));
      }
    }

    // libinput's on-button-down scrolling: while the configured button is held (or latched, with the lock), motion
    // turns into scroll events and the button itself stops clicking.
    void applyScrollButton(
        libinput_device* libinputDevice, const wlr_input_device* device, std::optional<uint32_t> configuredButton,
        std::optional<bool> configuredLock, std::string_view buttonSetting, std::string_view lockSetting
    ) {
      const bool supported =
          (libinput_device_config_scroll_get_methods(libinputDevice) & LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN) != 0;
      if (!supported) {
        if (configuredButton) {
          kLog.warn("input: '{}' does not support {}", deviceName(device), buttonSetting);
        } else if (configuredLock) {
          kLog.warn("input: '{}' does not support {}", deviceName(device), lockSetting);
        }
        return;
      }

      if (!configuredButton) {
        if (configuredLock) {
          kLog.warn("input: '{}' ignores {} because {} is not set", deviceName(device), lockSetting, buttonSetting);
        }
        // The device may still carry a button-down method from an earlier config, so every default has to go back.
        if (libinput_device_config_scroll_set_method(
                libinputDevice, libinput_device_config_scroll_get_default_method(libinputDevice)
            ) != LIBINPUT_CONFIG_STATUS_SUCCESS
            || libinput_device_config_scroll_set_button(
                   libinputDevice, libinput_device_config_scroll_get_default_button(libinputDevice)
               ) != LIBINPUT_CONFIG_STATUS_SUCCESS
            || libinput_device_config_scroll_set_button_lock(
                   libinputDevice, libinput_device_config_scroll_get_default_button_lock(libinputDevice)
               ) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn("input: failed to restore the default scroll button state for '{}'", deviceName(device));
        }
        return;
      }

      if (libinput_device_config_scroll_set_button(libinputDevice, *configuredButton)
          != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        const char* name = mouseButtonName(*configuredButton);
        kLog.warn(
            "input: could not apply {} to '{}': the device has no {} button", buttonSetting, deviceName(device),
            name != nullptr ? name : "such"
        );
        return;
      }
      if (libinput_device_config_scroll_set_method(libinputDevice, LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN)
          != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        kLog.warn("input: failed to apply {} to '{}'", buttonSetting, deviceName(device));
        return;
      }
      const auto lockState = configuredLock.value_or(
                                 libinput_device_config_scroll_get_default_button_lock(libinputDevice)
                                 == LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_ENABLED
                             )
          ? LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_ENABLED
          : LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_DISABLED;
      if (libinput_device_config_scroll_set_button_lock(libinputDevice, lockState) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        kLog.warn("input: failed to apply {} to '{}'", lockSetting, deviceName(device));
      }
    }

    void applyMouseAcceleration(
        libinput_device* libinputDevice, const wlr_input_device* device,
        const std::optional<AccelProfile>& configuredProfile, const std::optional<double>& configuredSensitivity,
        std::string_view accelSetting, std::string_view sensitivitySetting
    ) {
      if (libinput_device_config_accel_is_available(libinputDevice) == 0) {
        return;
      }

      if (!configuredProfile) {
        const auto profile = libinput_device_config_accel_get_default_profile(libinputDevice);
        if (libinput_device_config_accel_set_profile(libinputDevice, profile) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn("input: failed to restore the default acceleration profile for '{}'", deviceName(device));
          return;
        }
      } else {
        enum libinput_config_accel_profile profile = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
        const char* profileName = "flat";
        switch (configuredProfile->kind) {
        case AccelProfile::Kind::Flat:
          break;
        case AccelProfile::Kind::Adaptive:
          profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
          profileName = "adaptive";
          break;
        case AccelProfile::Kind::Custom:
          profile = LIBINPUT_CONFIG_ACCEL_PROFILE_CUSTOM;
          profileName = "custom";
          break;
        }
        if ((libinput_device_config_accel_get_profiles(libinputDevice) & profile) == 0) {
          kLog.warn("input: '{}' does not support the {} acceleration profile", deviceName(device), profileName);
          return;
        }

        if (configuredProfile->kind == AccelProfile::Kind::Custom) {
          libinput_config_accel* acceleration = libinput_config_accel_create(profile);
          if (acceleration == nullptr) {
            kLog.warn("input: failed to create custom acceleration profile for '{}'", deviceName(device));
            return;
          }
          const auto pointsStatus = libinput_config_accel_set_points(
              acceleration, LIBINPUT_ACCEL_TYPE_MOTION, configuredProfile->step, configuredProfile->points.size(),
              configuredProfile->points.data()
          );
          const auto applyStatus = pointsStatus == LIBINPUT_CONFIG_STATUS_SUCCESS
              ? libinput_device_config_accel_apply(libinputDevice, acceleration)
              : pointsStatus;
          libinput_config_accel_destroy(acceleration);
          if (applyStatus != LIBINPUT_CONFIG_STATUS_SUCCESS) {
            kLog.warn("input: failed to apply {} to '{}'", accelSetting, deviceName(device));
          }
          return;
        }

        if (libinput_device_config_accel_set_profile(libinputDevice, profile) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn("input: failed to apply {} to '{}'", accelSetting, deviceName(device));
          return;
        }
      }

      const double sensitivity =
          configuredSensitivity.value_or(libinput_device_config_accel_get_default_speed(libinputDevice));
      if (libinput_device_config_accel_set_speed(libinputDevice, sensitivity) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        if (configuredSensitivity) {
          kLog.warn("input: failed to apply {} to '{}'", sensitivitySetting, deviceName(device));
        } else {
          kLog.warn("input: failed to restore the default acceleration speed for '{}'", deviceName(device));
        }
      }
    }

    // A touchscreen is a physical panel, so with no map_to_output it follows the output the device reports, or else
    // the only enabled built-in panel. Two built-in panels are ambiguous and leave the device on the full layout.
    Output* defaultTouchOutput(std::span<const std::unique_ptr<Output>> outputs, const char* reported) {
      Output* builtin = nullptr;
      bool ambiguous = false;
      for (const auto& output : outputs) {
        if (!output->wlr()->enabled) {
          continue;
        }
        const std::string_view name = output->wlr()->name;
        if (reported != nullptr && name == reported) {
          return output.get();
        }
        if (name.starts_with("eDP-") || name.starts_with("LVDS-") || name.starts_with("DSI-")) {
          ambiguous = builtin != nullptr;
          builtin = output.get();
        }
      }
      return ambiguous ? nullptr : builtin;
    }
  } // namespace

  namespace server_events {
    void applyPointerConfig(wlr_input_device* device) {
      if (!wlr_input_device_is_libinput(device)) {
        return;
      }
      libinput_device* libinputDevice = wlr_libinput_get_device_handle(device);
      if (libinputDevice == nullptr) {
        return;
      }

      const Config::Input& input = config().input;
      const Config::Input::Device* override = input.findDevice(device->name != nullptr ? device->name : "");

      const bool isTouchpad = libinput_device_config_tap_get_finger_count(libinputDevice) > 0;
      const bool enabled =
          override != nullptr && override->enabled ? *override->enabled : !isTouchpad || input.touchpad.enabled;
      if ((libinput_device_config_send_events_get_modes(libinputDevice) & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED) != 0) {
        libinput_device_config_send_events_set_mode(
            libinputDevice, enabled ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
        );
      } else if (!enabled) {
        kLog.warn("input: '{}' cannot be disabled", deviceName(device));
      }
      if (!enabled) {
        return;
      }

      if (isTouchpad) {
        const std::optional<bool>& tap = override != nullptr && override->tap ? override->tap : input.touchpad.tap;
        const auto tapState =
            tap.value_or(libinput_device_config_tap_get_default_enabled(libinputDevice) == LIBINPUT_CONFIG_TAP_ENABLED)
            ? LIBINPUT_CONFIG_TAP_ENABLED
            : LIBINPUT_CONFIG_TAP_DISABLED;
        if (libinput_device_config_tap_set_enabled(libinputDevice, tapState) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn(
              "input: failed to apply {} to '{}'",
              override != nullptr && override->tap ? "input.device.tap" : "input.touchpad.tap", deviceName(device)
          );
        }
        const bool hasTapDragOverride = override != nullptr && override->tapAndDrag.has_value();
        const std::optional<bool>& tapAndDrag = hasTapDragOverride ? override->tapAndDrag : input.touchpad.tapAndDrag;
        const auto tapDragState =
            tapAndDrag.value_or(
                libinput_device_config_tap_get_default_drag_enabled(libinputDevice) == LIBINPUT_CONFIG_DRAG_ENABLED
            )
            ? LIBINPUT_CONFIG_DRAG_ENABLED
            : LIBINPUT_CONFIG_DRAG_DISABLED;
        if (libinput_device_config_tap_set_drag_enabled(libinputDevice, tapDragState)
            != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn(
              "input: failed to apply {} to '{}'",
              hasTapDragOverride ? "input.device.tap_and_drag" : "input.touchpad.tap_and_drag", deviceName(device)
          );
        }
        const bool hasDragLockOverride = override != nullptr && override->dragLock.has_value();
        const std::optional<DragLock>& dragLock = hasDragLockOverride ? override->dragLock : input.touchpad.dragLock;
        enum libinput_config_drag_lock_state dragLockState =
            libinput_device_config_tap_get_default_drag_lock_enabled(libinputDevice);
        if (dragLock) {
          switch (*dragLock) {
          case DragLock::Off:
            dragLockState = LIBINPUT_CONFIG_DRAG_LOCK_DISABLED;
            break;
          case DragLock::Timeout:
            dragLockState = LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_TIMEOUT;
            break;
          case DragLock::Sticky:
            dragLockState = LIBINPUT_CONFIG_DRAG_LOCK_ENABLED_STICKY;
            break;
          }
        }
        if (libinput_device_config_tap_set_drag_lock_enabled(libinputDevice, dragLockState)
            != LIBINPUT_CONFIG_STATUS_SUCCESS) {
          kLog.warn(
              "input: failed to apply {} to '{}'",
              hasDragLockOverride ? "input.device.drag_lock" : "input.touchpad.drag_lock", deviceName(device)
          );
        }
        if ((libinput_device_config_send_events_get_modes(libinputDevice)
             & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE)
            != 0) {
          const auto sendEventsMode = input.touchpad.disableOnExternalMouse.value_or(
                                          libinput_device_config_send_events_get_default_mode(libinputDevice)
                                          == LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE
                                      )
              ? LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE
              : LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
          if (libinput_device_config_send_events_set_mode(libinputDevice, sendEventsMode)
              != LIBINPUT_CONFIG_STATUS_SUCCESS) {
            kLog.warn("input: failed to apply input.touchpad.disable_on_external_mouse to '{}'", deviceName(device));
          }
        } else if (input.touchpad.disableOnExternalMouse) {
          kLog.warn(
              "input: could not apply input.touchpad.disable_on_external_mouse to '{}': device does not support "
              "disabling on external mouse",
              deviceName(device)
          );
        }
        const bool hasDwtOverride = override != nullptr && override->disableWhileTyping.has_value();
        const std::optional<bool>& dwt =
            hasDwtOverride ? override->disableWhileTyping : input.touchpad.disableWhileTyping;
        const std::string_view dwtSetting =
            hasDwtOverride ? "input.device.disable_while_typing" : "input.touchpad.disable_while_typing";
        if (libinput_device_config_dwt_is_available(libinputDevice)) {
          const auto dwtState =
              dwt.value_or(
                  libinput_device_config_dwt_get_default_enabled(libinputDevice) == LIBINPUT_CONFIG_DWT_ENABLED
              )
              ? LIBINPUT_CONFIG_DWT_ENABLED
              : LIBINPUT_CONFIG_DWT_DISABLED;
          if (libinput_device_config_dwt_set_enabled(libinputDevice, dwtState) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
            if (dwt) {
              kLog.warn("input: failed to apply {} to '{}'", dwtSetting, deviceName(device));
            } else {
              kLog.warn("input: failed to restore default disable-while-typing for '{}'", deviceName(device));
            }
          }
        } else if (dwt) {
          kLog.warn(
              "input: could not apply {} to '{}': device does not support disable-while-typing", dwtSetting,
              deviceName(device)
          );
        }
        const bool hasTapMapOverride = override != nullptr && override->tapButtonMap.has_value();
        const std::optional<TapButtonMap>& tapButtonMap =
            hasTapMapOverride ? override->tapButtonMap : input.touchpad.tapButtonMap;
        applyTapButtonMap(
            libinputDevice, device, tapButtonMap,
            hasTapMapOverride ? "input.device.tap_button_map" : "input.touchpad.tap_button_map"
        );
      }

      const bool hasClickOverride = override != nullptr && override->clickMethod.has_value();
      const std::optional<ClickMethod> clickMethod = hasClickOverride ? override->clickMethod
          : isTouchpad                                                ? input.touchpad.clickMethod
                                                                      : std::nullopt;
      applyClickMethod(
          libinputDevice, device, clickMethod,
          hasClickOverride ? "input.device.click_method" : "input.touchpad.click_method"
      );

      const std::optional<bool>& naturalScroll = override != nullptr && override->naturalScroll
          ? override->naturalScroll
          : isTouchpad ? input.touchpad.naturalScroll
                       : input.mouse.naturalScroll;
      applyNaturalScroll(
          libinputDevice, device, naturalScroll,
          override != nullptr && override->naturalScroll ? "input.device.natural_scroll"
              : isTouchpad                               ? "input.touchpad.natural_scroll"
                                                         : "input.mouse.natural_scroll"
      );

      const std::optional<bool>& leftHanded = override != nullptr && override->leftHanded ? override->leftHanded
          : isTouchpad                                                                    ? input.touchpad.leftHanded
                                                                                          : input.mouse.leftHanded;
      applyLeftHanded(
          libinputDevice, device, leftHanded,
          override != nullptr && override->leftHanded ? "input.device.left_handed"
              : isTouchpad                            ? "input.touchpad.left_handed"
                                                      : "input.mouse.left_handed"
      );

      // Button scrolling has no `[input.touchpad]` counterpart: a touchpad only gets it from its own device rule,
      // never from `[input.mouse]`, since claiming a button there would cost the pad its two-finger scrolling.
      const std::optional<uint32_t> scrollButton = override != nullptr && override->scrollButton
          ? override->scrollButton
          : isTouchpad ? std::nullopt
                       : input.mouse.scrollButton;
      const std::optional<bool> scrollButtonLock = override != nullptr && override->scrollButtonLock
          ? override->scrollButtonLock
          : isTouchpad ? std::nullopt
                       : input.mouse.scrollButtonLock;
      applyScrollButton(
          libinputDevice, device, scrollButton, scrollButtonLock,
          override != nullptr && override->scrollButton ? "input.device.scroll_button" : "input.mouse.scroll_button",
          override != nullptr && override->scrollButtonLock ? "input.device.scroll_button_lock"
                                                            : "input.mouse.scroll_button_lock"
      );

      const bool hasMethodOverride = override != nullptr && override->scrollMethod.has_value();
      const bool ruleOwnsScrollButton = override != nullptr && override->scrollButton.has_value();
      const std::optional<ScrollMethod> scrollMethod = hasMethodOverride ? override->scrollMethod
          : ruleOwnsScrollButton                                         ? std::nullopt
          : isTouchpad                                                   ? input.touchpad.scrollMethod
                                                                         : std::nullopt;
      applyScrollMethod(
          libinputDevice, device, scrollMethod,
          hasMethodOverride ? "input.device.scroll_method" : "input.touchpad.scroll_method"
      );

      const std::optional<AccelProfile> accelProfile = override != nullptr && override->accelProfile
          ? override->accelProfile
          : isTouchpad ? input.touchpad.accelProfile
                       : std::optional{input.mouse.accelProfile};
      const std::optional<double> sensitivity = override != nullptr && override->sensitivity ? override->sensitivity
          : isTouchpad ? input.touchpad.sensitivity
                       : std::optional{input.mouse.sensitivity};
      applyMouseAcceleration(
          libinputDevice, device, accelProfile, sensitivity,
          override != nullptr && override->accelProfile ? "input.device.accel_profile"
              : isTouchpad                              ? "input.touchpad.accel_profile"
                                                        : "input.mouse.accel_profile",
          override != nullptr && override->sensitivity ? "input.device.sensitivity"
              : isTouchpad                             ? "input.touchpad.sensitivity"
                                                       : "input.mouse.sensitivity"
      );
    }
  } // namespace server_events

  std::vector<InputDeviceInfo> Server::inputDevices() const {
    std::vector<InputDeviceInfo> devices;
    const auto add = [&devices](wlr_input_device* device, uint32_t kind) {
      if (device != nullptr && wlr_input_device_is_libinput(device) && device->name != nullptr) {
        devices.push_back({.name = device->name, .kind = kind});
      }
    };
    for (const auto& keyboard : m_keyboards) {
      add(&keyboard->wlr()->base, DSK_SETTINGS_MANAGER_V1_KIND_KEYBOARD);
    }
    for (const auto& pointer : m_pointers) {
      libinput_device* handle =
          wlr_input_device_is_libinput(pointer->device) ? wlr_libinput_get_device_handle(pointer->device) : nullptr;
      const bool touchpad = handle != nullptr && libinput_device_config_tap_get_finger_count(handle) > 0;
      add(pointer->device, touchpad ? DSK_SETTINGS_MANAGER_V1_KIND_TOUCHPAD : DSK_SETTINGS_MANAGER_V1_KIND_MOUSE);
    }
    for (const auto& touch : m_touchDevices) {
      add(touch->device, DSK_SETTINGS_MANAGER_V1_KIND_TOUCH);
    }
    for (const auto& tablet : m_tabletDevices) {
      add(tablet->device, DSK_SETTINGS_MANAGER_V1_KIND_TABLET);
    }
    return devices;
  }

  void Server::inputDevicesChanged() {
    if (m_desktopSettingsManager != nullptr) {
      m_desktopSettingsManager->devicesChanged();
    }
  }

  void Server::onNewInput(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newInput);
    auto* device = static_cast<wlr_input_device*>(data);
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
      self->addKeyboard(device);
      break;
    case WLR_INPUT_DEVICE_POINTER:
      self->addPointer(device);
      break;
    case WLR_INPUT_DEVICE_TOUCH:
      self->addTouch(device);
      break;
    case WLR_INPUT_DEVICE_TABLET:
      self->addTablet(device);
      break;
    case WLR_INPUT_DEVICE_TABLET_PAD:
      self->addTabletPad(device);
      break;
    case WLR_INPUT_DEVICE_SWITCH:
      self->addSwitch(device);
      break;
    default:
      break;
    }
    self->updateSeatCapabilities();
  }

  void Server::onNewPointerConstraint(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newPointerConstraint);
    self->m_cursor->handleNewConstraint(static_cast<wlr_pointer_constraint_v1*>(data));
  }

  void Server::onNewVirtualKeyboard(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newVirtualKeyboard);
    auto* keyboard = static_cast<wlr_virtual_keyboard_v1*>(data);
    if (keyboard->keyboard.keymap != nullptr) {
      self->addKeyboard(&keyboard->keyboard.base);
      self->updateSeatCapabilities();
      return;
    }

    // Virtual-keyboard creation and keymap upload are separate protocol
    // requests. Do not expose the device until it has a usable map, otherwise
    // wlroots broadcasts a transient no-keymap event to every keyboard client.
    auto* pending = new PendingVirtualKeyboard{
        .server = self,
        .keyboard = &keyboard->keyboard,
    };
    pending->keymap.notify = onPendingVirtualKeyboardKeymap;
    wl_signal_add(&keyboard->keyboard.events.keymap, &pending->keymap);
    pending->destroy.notify = onPendingVirtualKeyboardDestroy;
    wl_signal_add(&keyboard->keyboard.base.events.destroy, &pending->destroy);
  }

  void Server::onPendingVirtualKeyboardKeymap(wl_listener* listener, void* /*data*/) {
    PendingVirtualKeyboard* pending;
    pending = wl_container_of(listener, pending, keymap);
    if (pending->keyboard->keymap == nullptr) {
      return;
    }

    Server* server = pending->server;
    wlr_input_device* device = &pending->keyboard->base;
    destroyPendingVirtualKeyboard(pending);
    server->addKeyboard(device);
    server->updateSeatCapabilities();
  }

  void Server::onPendingVirtualKeyboardDestroy(wl_listener* listener, void* /*data*/) {
    PendingVirtualKeyboard* pending;
    pending = wl_container_of(listener, pending, destroy);
    destroyPendingVirtualKeyboard(pending);
  }

  void Server::destroyPendingVirtualKeyboard(PendingVirtualKeyboard* pending) {
    wl_list_remove(&pending->keymap.link);
    wl_list_remove(&pending->destroy.link);
    delete pending;
  }

  void Server::onNewVirtualPointer(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newVirtualPointer);
    auto* event = static_cast<wlr_virtual_pointer_v1_new_pointer_event*>(data);
    auto* vpointer = event->new_pointer;

    auto device = std::make_unique<VirtualPointerDevice>();
    device->server = self;
    device->vpointer = vpointer;
    device->destroy.notify = onVirtualPointerDestroy;
    wl_resource_add_destroy_listener(vpointer->resource, &device->destroy);

    // Attach to the cursor exactly like a physical pointer (see addPointer), so a virtual pointer runs the same Cursor
    // pipeline: hover, click-to-focus, mouse binds, and interactive move and resize.
    self->m_cursor->attachInputDevice(&vpointer->pointer.base);
    // A pointer created for an output (as Link's remote control does for the monitor a phone shows) moves only on it.
    if (event->suggested_output != nullptr) {
      self->m_cursor->mapInputToOutput(&vpointer->pointer.base, event->suggested_output);
    }

    self->m_virtualPointers.push_back(std::move(device));
  }

  void Server::onVirtualPointerDestroy(wl_listener* listener, void* /*data*/) {
    VirtualPointerDevice* device;
    device = wl_container_of(listener, device, destroy);
    // wlr_cursor detaches the device itself when the pointer is destroyed.
    wl_list_remove(&device->destroy.link);
    std::erase_if(device->server->m_virtualPointers, [device](const std::unique_ptr<VirtualPointerDevice>& ptr) {
      return ptr.get() == device;
    });
  }

  void Server::addKeyboard(wlr_input_device* device) {
    wlr_seat* seat = m_seat->wlr();
    const bool seatHasKeyboard = wlr_seat_get_keyboard(seat) != nullptr;
    auto entry = std::make_unique<Keyboard>(*this, device);
    Keyboard* keyboard = entry.get();
    m_keyboards.push_back(std::move(entry));

    if (!keyboard->virtualDevice()) {
      if (m_keyboardLayoutSource == nullptr) {
        m_keyboardLayoutSource = keyboard;
      } else {
        keyboard->syncLayoutFrom(*m_keyboardLayoutSource);
      }
      notifyKeyboardLayoutIpc();
    }

    // Pending virtual keyboards are promoted only after their first keymap.
    // Keep the same guard for physical-keymap setup failures so wlroots never
    // broadcasts a transient no-keymap event from this path.
    if (!seatHasKeyboard && keyboard->wlr()->keymap != nullptr) {
      wlr_seat_set_keyboard(seat, keyboard->wlr());
    }
    m_keyboardLeds->add(keyboard->wlr(), keyboard->virtualDevice(), config().input.keyboard.numlockToggle);
    if (m_inputCapture != nullptr && !keyboard->virtualDevice()) {
      m_inputCapture->addKeyboard(keyboard->wlr());
    }
  }

  bool Server::cycleKeyboardLayout() {
    if (m_keyboardLayoutSource != nullptr && m_keyboardLayoutSource->cycleLayout()) {
      return true;
    }
    for (const auto& keyboard : m_keyboards) {
      if (keyboard.get() != m_keyboardLayoutSource && keyboard->cycleLayout()) {
        return true;
      }
    }
    return false;
  }

  void Server::syncKeyboardLayout(Keyboard* source) {
    for (const auto& keyboard : m_keyboards) {
      if (keyboard.get() != source) {
        keyboard->syncLayoutFrom(*source);
      }
    }
  }

  void Server::keyboardLayoutChanged(Keyboard* origin) {
    m_keyboardLayoutSource = origin;
    syncKeyboardLayout(origin);
    notifyKeyboardLayoutIpc();
  }

  std::optional<Server::KeyboardLayoutState> Server::keyboardLayoutState() const {
    const Keyboard* source = m_keyboardLayoutSource;
    auto usable = [](const Keyboard* keyboard) {
      return keyboard != nullptr
          && !keyboard->virtualDevice()
          && keyboard->wlr()->keymap != nullptr
          && keyboard->wlr()->xkb_state != nullptr;
    };
    if (!usable(source)) {
      source = nullptr;
      for (const auto& keyboard : m_keyboards) {
        if (usable(keyboard.get())) {
          source = keyboard.get();
          break;
        }
      }
    }
    if (source == nullptr) {
      return std::nullopt;
    }

    wlr_keyboard* wlrKeyboard = source->wlr();
    KeyboardLayoutState state;
    const xkb_layout_index_t count = xkb_keymap_num_layouts(wlrKeyboard->keymap);
    state.names.reserve(count);
    for (xkb_layout_index_t i = 0; i < count; ++i) {
      const char* name = xkb_keymap_layout_get_name(wlrKeyboard->keymap, i);
      state.names.emplace_back(name != nullptr ? name : "");
    }
    state.currentIndex = xkb_state_serialize_layout(wlrKeyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    return state;
  }

  void Server::notifyKeyboardLayoutIpc() {
    if (m_ipc != nullptr) {
      m_ipc->notifyKeyboardLayoutChanged();
    }
  }

  void Server::addPointer(wlr_input_device* device) {
    auto pointer = std::make_unique<PointerDevice>();
    pointer->server = this;
    pointer->device = device;
    pointer->destroy.notify = onPointerDestroy;
    wl_signal_add(&device->events.destroy, &pointer->destroy);
    applyPointerConfig(device);
    m_cursor->attachInputDevice(device);
    m_pointers.push_back(std::move(pointer));
  }

  void Server::addTouch(wlr_input_device* device) {
    auto touch = std::make_unique<TouchDevice>();
    touch->server = this;
    touch->device = device;
    touch->destroy.notify = onTouchDestroy;
    wl_signal_add(&device->events.destroy, &touch->destroy);
    m_cursor->attachInputDevice(device);
    applyTouchConfig(*touch);
    m_touchDevices.push_back(std::move(touch));
    remapTouches();
    kLog.info("input: added touch device '{}'", deviceName(device));
  }

  void Server::onTouchDestroy(wl_listener* listener, void* /*data*/) {
    TouchDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_touchDevices, [watch](const std::unique_ptr<TouchDevice>& entry) {
      return entry.get() == watch;
    });
    server->updateSeatCapabilities();
  }

  void Server::addSwitch(wlr_input_device* device) {
    auto entry = std::make_unique<SwitchDevice>();
    entry->server = this;
    entry->device = device;
    if (wlr_input_device_is_libinput(device) != 0) {
      if (libinput_device* handle = wlr_libinput_get_device_handle(device);
          handle != nullptr && libinput_device_switch_has_switch(handle, LIBINPUT_SWITCH_LID) == 1) {
        // libinput defines OFF as the initial logical state and emits no event
        // to confirm it. Its queued initial ON event, when present, is handled
        // before the event loop reaches the reconciliation idle callback.
        entry->lidSource = m_lidState.addSource(LidState::Open);
      }
    }
    entry->destroy.notify = onSwitchDestroy;
    wl_signal_add(&device->events.destroy, &entry->destroy);
    entry->toggle.notify = onSwitchToggle;
    wl_signal_add(&wlr_switch_from_input_device(device)->events.toggle, &entry->toggle);
    const bool tracksLid = entry->lidSource.has_value();
    m_switchDevices.push_back(std::move(entry));
    if (tracksLid) {
      scheduleLidStateReconcile();
    }
    kLog.info("input: added switch device '{}'", deviceName(device));
  }

  void Server::onSwitchDestroy(wl_listener* listener, void* /*data*/) {
    SwitchDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    wl_list_remove(&watch->toggle.link);
    if (watch->lidSource) {
      server->m_lidState.removeSource(*watch->lidSource);
      server->scheduleLidStateReconcile();
    }
    std::erase_if(server->m_switchDevices, [watch](const std::unique_ptr<SwitchDevice>& entry) {
      return entry.get() == watch;
    });
  }

  void Server::onSwitchToggle(wl_listener* listener, void* data) {
    SwitchDevice* watch;
    watch = wl_container_of(listener, watch, toggle);
    const auto* event = static_cast<wlr_switch_toggle_event*>(data);
    if (event->switch_type != WLR_SWITCH_TYPE_LID) {
      return;
    }
    Server* server = watch->server;
    const LidState state = event->switch_state == WLR_SWITCH_STATE_ON ? LidState::Closed : LidState::Open;
    if (watch->lidSource) {
      server->m_lidState.updateSource(*watch->lidSource, state);
    } else {
      // Non-libinput switch devices have no capability query. Their first LID
      // event proves that they are a logical lid source.
      watch->lidSource = server->m_lidState.addSource(state);
    }
    server->scheduleLidStateReconcile();
  }

  void Server::scheduleLidStateReconcile() {
    if (m_stopping || m_lidStateReconcileIdle != nullptr) {
      return;
    }
    m_lidStateReconcileIdle =
        wl_event_loop_add_idle(wl_display_get_event_loop(m_display), onLidStateReconcileIdle, this);
    if (m_lidStateReconcileIdle == nullptr) {
      kLog.error("failed to register lid-state reconciliation idle source");
    }
  }

  void Server::onLidStateReconcileIdle(void* data) {
    auto* server = static_cast<Server*>(data);
    server->m_lidStateReconcileIdle = nullptr;
    server->reconcileLidState();
  }

  void Server::reconcileLidState() {
    const std::optional<LidState> transition = m_lidState.takeTransition();
    if (!transition) {
      return;
    }
    if (*transition == LidState::Closed) {
      kLog.info("lid closed");
      if (!config().events.lidClose.empty()) {
        spawn(config().events.lidClose.c_str(), "events.lid_close");
      }
    } else {
      kLog.info("lid opened");
      if (!config().events.lidOpen.empty()) {
        spawn(config().events.lidOpen.c_str(), "events.lid_open");
      }
    }
  }

  void Server::addTablet(wlr_input_device* device) {
    auto tablet = std::make_unique<TabletDevice>();
    tablet->server = this;
    tablet->device = device;
    tablet->v2 = wlr_tablet_create(m_tabletManager, m_seat->wlr(), device);
    tablet->destroy.notify = onTabletDestroy;
    wl_signal_add(&device->events.destroy, &tablet->destroy);
    m_cursor->attachInputDevice(device);
    applyTabletConfig(*tablet);
    m_tabletDevices.push_back(std::move(tablet));
    pairTabletPads();
    kLog.info("input: added tablet '{}'", deviceName(device));
  }

  void Server::applyTabletConfig(TabletDevice& tablet) {
    if (wlr_input_device_is_libinput(tablet.device) == 0) {
      kLog.debug("input: tablet '{}' is not a libinput device; tablet settings skipped", deviceName(tablet.device));
      return;
    }
    libinput_device* libinputDevice = wlr_libinput_get_device_handle(tablet.device);
    if (libinputDevice == nullptr) {
      return;
    }
    const Config::Input::Tablet& cfg = config().input.tablet;
    if ((libinput_device_config_send_events_get_modes(libinputDevice) & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED) != 0) {
      libinput_device_config_send_events_set_mode(
          libinputDevice, cfg.enabled ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
      );
    } else if (!cfg.enabled) {
      kLog.warn("input: '{}' cannot be disabled", deviceName(tablet.device));
    }
    if (libinput_device_config_left_handed_is_available(libinputDevice) != 0
        && libinput_device_config_left_handed_set(libinputDevice, cfg.leftHanded) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
      kLog.warn("input: failed to apply input.tablet.left_handed to '{}'", deviceName(tablet.device));
    }
    if (libinput_device_config_calibration_has_matrix(libinputDevice) != 0) {
      if (cfg.calibrationMatrix.has_value()) {
        libinput_device_config_calibration_set_matrix(libinputDevice, cfg.calibrationMatrix->data());
      } else {
        // Removing the key on reload restores the device default.
        float matrix[6] = {};
        libinput_device_config_calibration_get_default_matrix(libinputDevice, matrix);
        libinput_device_config_calibration_set_matrix(libinputDevice, matrix);
      }
    }
    remapTablets();
  }

  void Server::applyTouchConfig(TouchDevice& touch) {
    if (wlr_input_device_is_libinput(touch.device) == 0) {
      kLog.debug("input: touch device '{}' is not a libinput device; touch settings skipped", deviceName(touch.device));
      return;
    }
    libinput_device* libinputDevice = wlr_libinput_get_device_handle(touch.device);
    if (libinputDevice == nullptr) {
      return;
    }
    const Config::Input::Touch& cfg = config().input.touch;
    if ((libinput_device_config_send_events_get_modes(libinputDevice) & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED) != 0) {
      libinput_device_config_send_events_set_mode(
          libinputDevice, cfg.enabled ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
      );
    } else if (!cfg.enabled) {
      kLog.warn("input: '{}' cannot be disabled", deviceName(touch.device));
    }
  }

  void Server::addTabletPad(wlr_input_device* device) {
    auto pad = std::make_unique<TabletPadDevice>();
    pad->server = this;
    pad->device = device;
    pad->v2 = wlr_tablet_pad_create(m_tabletManager, m_seat->wlr(), device);
    wlr_tablet_pad* wlrPad = wlr_tablet_pad_from_input_device(device);
    pad->destroy.notify = onTabletPadDestroy;
    wl_signal_add(&device->events.destroy, &pad->destroy);
    pad->button.notify = onTabletPadButton;
    wl_signal_add(&wlrPad->events.button, &pad->button);
    pad->ring.notify = onTabletPadRing;
    wl_signal_add(&wlrPad->events.ring, &pad->ring);
    pad->strip.notify = onTabletPadStrip;
    wl_signal_add(&wlrPad->events.strip, &pad->strip);
    applyTabletPadConfig(*pad);
    m_tabletPads.push_back(std::move(pad));
    pairTabletPads();
    // Deliver an initial enter to the keyboard-focused surface so a client
    // already holding focus gets the pad without a focus change.
    if (wlr_surface* focused = m_seat->wlr()->keyboard_state.focused_surface;
        focused != nullptr && m_tabletPads.back()->tablet != nullptr) {
      TabletPadDevice* entry = m_tabletPads.back().get();
      entry->enteredSurface = focused;
      wlr_tablet_v2_tablet_pad_notify_enter(entry->v2, entry->tablet->v2, focused);
    }
    kLog.info("input: added tablet pad '{}'", deviceName(device));
  }

  void Server::applyTabletPadConfig(TabletPadDevice& pad) {
    if (wlr_input_device_is_libinput(pad.device) == 0) {
      return;
    }
    libinput_device* libinputDevice = wlr_libinput_get_device_handle(pad.device);
    if (libinputDevice == nullptr) {
      return;
    }
    const bool enabled = config().input.tablet.enabled;
    if ((libinput_device_config_send_events_get_modes(libinputDevice) & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED) != 0) {
      libinput_device_config_send_events_set_mode(
          libinputDevice, enabled ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
      );
    } else if (!enabled) {
      kLog.warn("input: '{}' cannot be disabled", deviceName(pad.device));
    }
  }

  void Server::pairTabletPads() {
    for (const auto& pad : m_tabletPads) {
      TabletDevice* match = nullptr;
      for (const auto& candidate : m_tabletDevices) {
        if (wlr_input_device_is_libinput(pad->device) != 0 && wlr_input_device_is_libinput(candidate->device) != 0) {
          libinput_device* padHandle = wlr_libinput_get_device_handle(pad->device);
          libinput_device* tabletHandle = wlr_libinput_get_device_handle(candidate->device);
          if (padHandle != nullptr
              && tabletHandle != nullptr
              && libinput_device_get_device_group(padHandle) == libinput_device_get_device_group(tabletHandle)) {
            match = candidate.get();
            break;
          }
        }
      }
      if (match == nullptr && m_tabletDevices.size() == 1) {
        match = m_tabletDevices.front().get();
      }
      pad->tablet = match;
    }
  }

  void Server::remapTablets() {
    const Config::Input::Tablet& cfg = config().input.tablet;
    for (const auto& tablet : m_tabletDevices) {
      wlr_box region{};
      wlr_output* output = nullptr;
      if (cfg.mapToFocusedWindow) {
        if (View* view = View::fromSurface(m_seat->wlr()->keyboard_state.focused_surface)) {
          const wlr_box geo = view->geometryBox();
          if (geo.width > 0 && geo.height > 0) {
            region = {view->layoutTargetX(), view->layoutTargetY(), geo.width, geo.height};
          }
        }
      }
      if (wlr_box_empty(&region) && (cfg.mapToFocusedWindow || cfg.mapToFocusedOutput)) {
        if (Output* out = focusedOutput()) {
          output = out->wlr();
        }
      }
      if (wlr_box_empty(&region) && output == nullptr && !cfg.mapToOutput.empty()) {
        if (Output* out = outputFromName(cfg.mapToOutput)) {
          output = out->wlr();
        }
      }
      wlr_cursor_map_input_to_region(m_cursor->wlr(), tablet->device, &region);
      wlr_cursor_map_input_to_output(m_cursor->wlr(), tablet->device, output);
    }
  }

  void Server::remapTouches() {
    const Config::Input::Touch& cfg = config().input.touch;
    for (const auto& touch : m_touchDevices) {
      Output* panel = !cfg.mapToOutput.empty()
          ? outputFromName(cfg.mapToOutput)
          : defaultTouchOutput(outputs(), wlr_touch_from_input_device(touch->device)->output_name);
      wlr_box region{};
      wlr_output* output = nullptr;
      if (const Output* source = panel != nullptr ? panel->mirrorSource() : nullptr) {
        const wlr_output* target = panel->wlr();
        region = mirrorInputRegion(
            source->layoutBox(), source->wlr()->width, source->wlr()->height, source->wlr()->transform, target->width,
            target->height, target->transform
        );
      } else if (panel != nullptr && panel->onDesktop()) {
        output = panel->wlr();
      }
      wlr_cursor_map_input_to_region(m_cursor->wlr(), touch->device, &region);
      wlr_cursor_map_input_to_output(m_cursor->wlr(), touch->device, output);
    }
  }

  wlr_tablet_v2_tablet* Server::tabletV2FromWlr(const wlr_tablet* tablet) const {
    if (tablet == nullptr) {
      return nullptr;
    }
    for (const auto& entry : m_tabletDevices) {
      if (&tablet->base == entry->device) {
        return entry->v2;
      }
    }
    return nullptr;
  }

  void Server::onTabletDestroy(wl_listener* listener, void* /*data*/) {
    TabletDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    for (const auto& pad : server->m_tabletPads) {
      if (pad->tablet != watch) {
        continue;
      }
      if (pad->enteredSurface != nullptr) {
        wlr_tablet_v2_tablet_pad_notify_leave(pad->v2, pad->enteredSurface);
        pad->enteredSurface = nullptr;
      }
      pad->tablet = nullptr;
    }
    std::erase_if(server->m_tabletDevices, [watch](const std::unique_ptr<TabletDevice>& entry) {
      return entry.get() == watch;
    });
    server->pairTabletPads();
    server->inputDevicesChanged();
  }

  void Server::onTabletPadButton(wl_listener* listener, void* data) {
    TabletPadDevice* watch;
    watch = wl_container_of(listener, watch, button);
    auto* event = static_cast<wlr_tablet_pad_button_event*>(data);
    if (event->state == WLR_BUTTON_PRESSED) {
      watch->server->notifyInputActivity();
    } else {
      watch->server->notifyIdleActivity();
    }
    wlr_tablet_v2_tablet_pad_notify_button(
        watch->v2, event->button, event->time_msec,
        event->state == WLR_BUTTON_PRESSED ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                                           : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED
    );
  }

  void Server::onTabletPadRing(wl_listener* listener, void* data) {
    TabletPadDevice* watch;
    watch = wl_container_of(listener, watch, ring);
    auto* event = static_cast<wlr_tablet_pad_ring_event*>(data);
    if (event->position < 0) {
      watch->server->notifyIdleActivity();
    } else {
      watch->server->notifyInputActivity();
    }
    wlr_tablet_v2_tablet_pad_notify_ring(
        watch->v2, event->ring, event->position, event->source == WLR_TABLET_PAD_RING_SOURCE_FINGER, event->time_msec
    );
  }

  void Server::onTabletPadStrip(wl_listener* listener, void* data) {
    TabletPadDevice* watch;
    watch = wl_container_of(listener, watch, strip);
    auto* event = static_cast<wlr_tablet_pad_strip_event*>(data);
    if (event->position < 0) {
      watch->server->notifyIdleActivity();
    } else {
      watch->server->notifyInputActivity();
    }
    wlr_tablet_v2_tablet_pad_notify_strip(
        watch->v2, event->strip, event->position, event->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER, event->time_msec
    );
  }

  void Server::onPadKeyboardFocusChange(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_padKeyboardFocusChange);
    auto* event = static_cast<wlr_seat_keyboard_focus_change_event*>(data);
    for (const auto& pad : self->m_tabletPads) {
      if (pad->tablet == nullptr) {
        continue;
      }
      if (pad->enteredSurface != nullptr) {
        wlr_tablet_v2_tablet_pad_notify_leave(pad->v2, pad->enteredSurface);
        pad->enteredSurface = nullptr;
      }
      if (event->new_surface != nullptr) {
        pad->enteredSurface = event->new_surface;
        wlr_tablet_v2_tablet_pad_notify_enter(pad->v2, pad->tablet->v2, event->new_surface);
      }
    }
  }

  void Server::onTabletPadDestroy(wl_listener* listener, void* /*data*/) {
    TabletPadDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    wl_list_remove(&watch->button.link);
    wl_list_remove(&watch->ring.link);
    wl_list_remove(&watch->strip.link);
    if (watch->enteredSurface != nullptr) {
      wlr_tablet_v2_tablet_pad_notify_leave(watch->v2, watch->enteredSurface);
    }
    std::erase_if(server->m_tabletPads, [watch](const std::unique_ptr<TabletPadDevice>& entry) {
      return entry.get() == watch;
    });
    server->pairTabletPads();
  }

  bool Server::setKeyboardLayout(std::string_view layout) {
    if (layout.empty()) {
      return false;
    }
    Keyboard* source = nullptr;
    if (m_keyboardLayoutSource != nullptr && m_keyboardLayoutSource->setLayoutByName(layout)) {
      source = m_keyboardLayoutSource;
    } else {
      for (const auto& keyboard : m_keyboards) {
        if (keyboard.get() != m_keyboardLayoutSource && keyboard->setLayoutByName(layout)) {
          source = keyboard.get();
          break;
        }
      }
    }
    if (source == nullptr) {
      return false;
    }
    m_keyboardLayoutSource = source;
    syncKeyboardLayout(source);
    notifyKeyboardLayoutIpc();
    return true;
  }

  void Server::notifyKeyboardEnter(wlr_surface* surface, KeyboardGrab grab) {
    if (m_inputCapture != nullptr && (m_inputCapture->capabilities() & 1U) != 0) {
      return;
    }
    wlr_seat* seat = m_seat->wlr();
    wlr_surface* previous = seat->keyboard_state.focused_surface;
    const bool leavesXwayland = m_xwayland != nullptr
        && previous != nullptr
        && wlr_xwayland_surface_try_from_wlr_surface(previous) != nullptr
        && surface != nullptr
        && wlr_xwayland_surface_try_from_wlr_surface(surface) == nullptr;

    if (config().input.keyboard.trackLayout == TrackLayout::Window) {
      if (const auto state = keyboardLayoutState();
          state.has_value() && state->currentIndex < state->names.size() && !state->names.empty()) {
        const std::string_view current = state->names[state->currentIndex];
        if (!current.empty()) {
          m_surfaceLayouts.remember(seat->keyboard_state.focused_surface, current);
          if (surface != nullptr) {
            const std::optional<std::string_view> recalled = m_surfaceLayouts.recall(surface);
            const std::string_view target = recalled.value_or(state->names.front());
            if (!target.empty() && target != current) {
              // Drop focus before changing the group so the outgoing client
              // cannot receive the incoming surface's modifier state.
              wlr_seat_keyboard_notify_clear_focus(seat);
              if (!setKeyboardLayout(target) && recalled.has_value()) {
                m_surfaceLayouts.forget(surface);
                setKeyboardLayout(state->names.front());
              }
            }
          }
        }
      }
    }

    if (m_qos != nullptr && surface != nullptr) {
      View* focusedView = View::fromSurface(wlr_surface_get_root_surface(surface));
      m_qos->focus(focusedView != nullptr ? focusedView->pid() : surfaceClientPid(surface));
    }

    // An active grab's enter handler decides where focus goes; an xdg_popup grab ignores it entirely.
    const auto enter =
        [seat,
         grab](wlr_surface* target, const uint32_t* keycodes, size_t count, const wlr_keyboard_modifiers* modifiers) {
          if (grab == KeyboardGrab::Bypass) {
            wlr_seat_keyboard_enter(seat, target, keycodes, count, modifiers);
          } else {
            wlr_seat_keyboard_notify_enter(seat, target, keycodes, count, modifiers);
          }
        };

    wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard == nullptr) {
      enter(surface, nullptr, 0, nullptr);
      if (leavesXwayland) {
        m_xwayland->scheduleFocusClear();
      }
      return;
    }

    // The device's key array is physical state. Keys a bind consumed never
    // reached a client and their release never will, so handing them over as
    // held keys strands them: XWayland turns that into an endless key repeat.
    const std::unordered_set<uint32_t>* consumed = nullptr;
    for (const std::unique_ptr<Keyboard>& entry : m_keyboards) {
      if (entry->wlr() == keyboard) {
        consumed = &entry->consumedKeycodes();
        break;
      }
    }
    if (consumed == nullptr || consumed->empty()) {
      enter(surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
      if (leavesXwayland) {
        m_xwayland->scheduleFocusClear();
      }
      return;
    }
    std::array<uint32_t, WLR_KEYBOARD_KEYS_CAP> forwarded{};
    size_t count = 0;
    for (size_t i = 0; i < keyboard->num_keycodes; ++i) {
      if (!consumed->contains(keyboard->keycodes[i])) {
        forwarded[count++] = keyboard->keycodes[i];
      }
    }
    enter(surface, forwarded.data(), count, &keyboard->modifiers);
    if (leavesXwayland) {
      m_xwayland->scheduleFocusClear();
    }
  }

  void Server::forgetConsumedKeycodes() {
    for (const std::unique_ptr<Keyboard>& entry : m_keyboards) {
      entry->forgetConsumedKeycodes();
    }
  }

  void Server::notifyKeyboardClearFocus() {
    wlr_seat* seat = m_seat->wlr();
    // Remember what the outgoing surface was using, but leave the active layout
    // alone because no incoming surface has a layout to restore.
    if (config().input.keyboard.trackLayout == TrackLayout::Window) {
      if (const auto state = keyboardLayoutState(); state.has_value() && state->currentIndex < state->names.size()) {
        m_surfaceLayouts.remember(seat->keyboard_state.focused_surface, state->names[state->currentIndex]);
      }
    }
    wlr_seat_keyboard_notify_clear_focus(seat);
  }

  void Server::rememberKeyboardInputSource(Keyboard& keyboard) {
    if (m_inputMethodRelay == nullptr || !m_inputMethodRelay->ownsKeyboard(keyboard.wlr())) {
      m_keyboardInputSource = &keyboard;
    }
    if (!m_stopping && m_keyboardLeds != nullptr) {
      m_keyboardLeds->update(keyboard.wlr());
    }
  }

  Keyboard* Server::syntheticKeyboard() const {
    if (m_keyboardInputSource != nullptr) {
      return m_keyboardInputSource;
    }
    return m_keyboards.empty() ? nullptr : m_keyboards.front().get();
  }

  bool Server::syncingKeyboardLocks() const { return m_keyboardLeds != nullptr && m_keyboardLeds->syncing(); }

  void Server::refreshKeyboardLeds() {
    if (!m_stopping && m_keyboardLeds != nullptr) {
      m_keyboardLeds->update();
    }
  }

  void Server::removeKeyboard(Keyboard* keyboard) {
    if (m_inputCapture != nullptr) {
      m_inputCapture->removeKeyboard(keyboard->wlr());
    }
    wlr_seat* seat = m_seat->wlr();
    const bool seatKeyboardRemoved = wlr_seat_get_keyboard(seat) == keyboard->wlr();
    const bool sourceRemoved = m_keyboardLayoutSource == keyboard;
    if (m_keyboardInputSource == keyboard) {
      m_keyboardInputSource = nullptr;
    }
    if (m_keyboardLeds != nullptr) {
      m_keyboardLeds->remove(keyboard->wlr());
    }
    std::erase_if(m_keyboards, [keyboard](const std::unique_ptr<Keyboard>& entry) { return entry.get() == keyboard; });
    if (seatKeyboardRemoved) {
      wlr_keyboard* replacement = m_keyboardInputSource != nullptr ? m_keyboardInputSource->wlr() : nullptr;
      if (replacement == nullptr) {
        for (const auto& entry : m_keyboards) {
          if (entry->wlr()->keymap != nullptr) {
            replacement = entry->wlr();
            break;
          }
        }
      }
      // Detach wlroots' later destroy listener so it cannot clear the replacement.
      wlr_seat_set_keyboard(seat, replacement);
      // Publish the replacement keymap before its masks, or neutralize the
      // pointer-only client before removing the final keyboard capability.
      m_seat->notifyPointerModifiers(replacement == nullptr);
    }
    if (sourceRemoved) {
      m_keyboardLayoutSource = nullptr;
      for (const auto& entry : m_keyboards) {
        if (!entry->virtualDevice()) {
          m_keyboardLayoutSource = entry.get();
          break;
        }
      }
      notifyKeyboardLayoutIpc();
    }
    updateSeatCapabilities();
  }

} // namespace andromeda
