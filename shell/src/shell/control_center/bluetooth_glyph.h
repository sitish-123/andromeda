#pragma once

#include "dbus/bluetooth/bluetooth_service.h"

[[nodiscard]] inline const char* bluetoothDeviceGlyph(BluetoothDeviceKind kind) {
  switch (kind) {
  case BluetoothDeviceKind::Headset:
    return "bluetooth-device-headset";
  case BluetoothDeviceKind::Headphones:
    return "bluetooth-device-headphones";
  case BluetoothDeviceKind::Earbuds:
    return "bluetooth-device-earbuds";
  case BluetoothDeviceKind::Speaker:
    return "bluetooth-device-speaker";
  case BluetoothDeviceKind::Microphone:
    return "bluetooth-device-microphone";
  case BluetoothDeviceKind::Mouse:
    return "bluetooth-device-mouse";
  case BluetoothDeviceKind::Keyboard:
    return "bluetooth-device-keyboard";
  case BluetoothDeviceKind::Phone:
    return "bluetooth-device-phone";
  case BluetoothDeviceKind::Computer:
    return "device-laptop";
  case BluetoothDeviceKind::Gamepad:
    return "bluetooth-device-gamepad";
  case BluetoothDeviceKind::Watch:
    return "bluetooth-device-watch";
  case BluetoothDeviceKind::Tv:
    return "bluetooth-device-tv";
  case BluetoothDeviceKind::Unknown:
  default:
    return "bluetooth-device-generic";
  }
}
