#pragma once

class AsyncTextureCache;
class BluetoothService;
class LinkService;
class BrightnessService;
class ClipboardService;
class CompositorPlatform;
class ConfigService;
class EasyEffectsService;
class FileWatcher;
class HttpClient;
class IdleInhibitor;
class INetworkService;
class LockKeysService;
class ModemManagerService;
class MprisService;
class NotificationManager;
class PipeWireService;
class PipeWireSpectrum;
class PowerProfilesService;
class RenderContext;
class ScreenshotService;
class SystemMonitorService;
class TrayService;
class UPowerService;
class WeatherService;
class GammaService;

namespace noctalia::theme {
  class ThemeService;
}
namespace scripting {
  class ScriptApiContext;
}

class SessionBus;

struct BarServices {
  CompositorPlatform& platform;
  ConfigService& config;
  NotificationManager* notifications = nullptr;
  TrayService* tray = nullptr;
  PipeWireService* audio = nullptr;
  EasyEffectsService* easyEffects = nullptr;
  UPowerService* upower = nullptr;
  SystemMonitorService* sysmon = nullptr;
  PowerProfilesService* powerProfiles = nullptr;
  INetworkService* network = nullptr;
  ModemManagerService* modem = nullptr;
  IdleInhibitor* idleInhibitor = nullptr;
  MprisService* mpris = nullptr;
  PipeWireSpectrum* audioSpectrum = nullptr;
  HttpClient* httpClient = nullptr;
  WeatherService* weather = nullptr;
  RenderContext* renderContext = nullptr;
  // Shared off-thread icon decode; null in tests (widgets then decode in place).
  AsyncTextureCache* asyncTextures = nullptr;
  GammaService* nightLight = nullptr;
  noctalia::theme::ThemeService* theme = nullptr;
  BluetoothService* bluetooth = nullptr;
  LinkService* link = nullptr;
  BrightnessService* brightness = nullptr;
  LockKeysService* lockKeys = nullptr;
  ClipboardService* clipboard = nullptr;
  FileWatcher* fileWatcher = nullptr;
  ScreenshotService* screenshots = nullptr;
  scripting::ScriptApiContext* scriptApi = nullptr;
  // Session bus for widgets that read other apps' D-Bus objects (the macOS-layout app menus).
  SessionBus* bus = nullptr;
};
