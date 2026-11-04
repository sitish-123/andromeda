#pragma once

#include "config/widget_setting_value.h"

#include <string>
#include <unordered_map>

class ClipboardService;
class CalendarService;
class ConfigService;
class FileWatcher;
class HttpClient;
class MprisService;
class PipeWireService;
class PipeWireSpectrum;
class RenderContext;
class SharedTextureCache;
class SystemMonitorService;
class WaylandConnection;
class WeatherService;

namespace scripting {
  class ScriptApiContext;
}

// Dependencies for plugin-backed (`[[desktop_widget]]`) widgets. All-null means
// plugin desktop widgets are unavailable in this factory (e.g. tests).
struct DesktopWidgetScriptDeps {
  scripting::ScriptApiContext* scriptApi = nullptr;
  FileWatcher* fileWatcher = nullptr;
  ClipboardService* clipboard = nullptr;
  ConfigService* configService = nullptr;
  // Plugin-level settings by plugin id, for hosts without a ConfigService (the lock helper).
  const std::unordered_map<std::string, std::unordered_map<std::string, WidgetSettingValue>>* pluginSettings = nullptr;
};

struct DesktopWidgetRuntimeServices {
  CalendarService* calendar = nullptr;
  PipeWireService* pipewire = nullptr;
  PipeWireSpectrum* pipewireSpectrum = nullptr;
  const WeatherService* weather = nullptr;
  MprisService* mpris = nullptr;
  HttpClient* httpClient = nullptr;
  SystemMonitorService* sysmon = nullptr;
  DesktopWidgetScriptDeps scriptDeps;
};

struct DesktopWidgetServices {
  WaylandConnection& wayland;
  ConfigService* config = nullptr;
  RenderContext* renderContext = nullptr;
  DesktopWidgetRuntimeServices runtime;
  SharedTextureCache* textureCache = nullptr;
};
