#pragma once

#include "app/deferred_call_poll_source.h"
#include "app/timer_poll_source.h"
#include "calendar/calendar_poll_source.h"
#include "calendar/calendar_reminder_poll_source.h"
#include "clipboard/clipboard_poll_source.h"
#include "compositors/keyboard_layout_poll_source.h"
#include "compositors/workspace_poll_source.h"
#include "config/config_poll_source.h"
#include "core/files/file_watcher.h"
#include "core/process/process_poll_source.h"
#include "dbus/notification/notification_poll_source.h"
#include "ipc/ipc_poll_source.h"
#include "net/http_client_poll_source.h"
#include "system/desktop_entry_poll_source.h"
#include "system/icon_theme_poll_source.h"
#include "system/location_poll_source.h"
#include "system/lock_keys_poll_source.h"
#include "system/weather_poll_source.h"
#include "time/time_poll_source.h"
#include "wayland/key_repeat_poll_source.h"

#include <memory>

class BrightnessPollSource;
class MemoryPressurePollSource;
class PipeWirePollSource;
class PipeWireSpectrumPollSource;
class PolkitPollSource;
class ScreenSaverPollSource;
class SessionBusPollSource;
class SystemBusPollSource;

// The shell's poll sources, in Application's member order: each one is destroyed before the service it reads.
struct AppPollSources {
  std::unique_ptr<ScreenSaverPollSource> screenSaver;
  std::unique_ptr<SessionBusPollSource> bus;
  std::unique_ptr<SystemBusPollSource> systemBus;
  NotificationPollSource notification;
  DeferredCallPollSource deferredCall;
  ProcessPollSource process;
  TimePollSource time;
  ConfigPollSource config;
  DesktopEntryPollSource desktopEntry;
  IconThemePollSource iconTheme;
  ClipboardPollSource clipboard;
  TimerPollSource timer;
  KeyRepeatPollSource keyRepeat;
  WorkspacePollSource workspace;
  KeyboardLayoutPollSource keyboardLayout;
  LockKeysPollSource lockKeys;
  std::unique_ptr<BrightnessPollSource> brightness;
  std::unique_ptr<MemoryPressurePollSource> memoryPressure;
  std::unique_ptr<PipeWirePollSource> pipewire;
  std::unique_ptr<PipeWireSpectrumPollSource> pipewireSpectrum;
  std::unique_ptr<PolkitPollSource> polkit;
  IpcPollSource ipc;
  HttpClientPollSource httpClient;
  FileWatchPollSource fileWatch;
  LocationPollSource location;
  WeatherPollSource weather;
  CalendarPollSource calendar;
  CalendarReminderPollSource calendarReminder;
};
