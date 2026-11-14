#include "application.h"
#include "application_internal.h"
#include "a11y/atspi_poll_source.h"
#include "capture/screen_recorder.h"
#include "compositors/andromeda/andromeda_audio_provider.h"
#include "dbus/idle/screensaver_poll_source.h"
#include "launcher/files_provider.h"
#include "dbus/idle/screensaver_service.h"
#include "dbus/polkit/polkit_poll_source.h"
#include "dbus/session_bus_poll_source.h"
#include "dbus/system_bus_poll_source.h"
#include "pipewire/pipewire_poll_source.h"
#include "pipewire/pipewire_spectrum_poll_source.h"
#include "pipewire/wireplumber_mixer.h"
#include "system/brightness_poll_source.h"
#include "system/memory_pressure_poll_source.h"
#include "ui/dialogs/file_dialog_sheet.h"
#include "core/files/directory_watcher.h"

std::vector<PollSource*> Application::currentPollSources() {
  std::vector<PollSource*> sources;
  if (m_poll.bus != nullptr) {
    sources.push_back(m_poll.bus.get());
  }
  if (screenSaver() != nullptr
      && screenSaver()->hasScreenSaverBus()
      && m_poll.screenSaver != nullptr) {
    sources.push_back(m_poll.screenSaver.get());
  }
  if (m_poll.systemBus != nullptr) {
    sources.push_back(m_poll.systemBus.get());
  }
  sources.push_back(&m_poll.notification);
  sources.push_back(&m_secretStore);
  sources.push_back(&m_poll.deferredCall);
  sources.push_back(&m_desktopIcons.watcher());
  sources.push_back(&FileDialogSheet::watcher());
  sources.push_back(&m_poll.process);
  if (m_screenRecorder != nullptr) {
    sources.push_back(m_screenRecorder.get());
  }
  sources.push_back(&m_poll.time);
  if (m_filesProvider != nullptr) {
    sources.push_back(m_filesProvider);
  }
  sources.push_back(&m_poll.config);
  sources.push_back(&m_poll.desktopEntry);
  sources.push_back(&m_poll.iconTheme);
  sources.push_back(&m_poll.clipboard);
  sources.push_back(&m_poll.timer);
  sources.push_back(&m_poll.keyRepeat);
  sources.push_back(&m_poll.workspace);
  sources.push_back(&m_poll.keyboardLayout);
  if constexpr (kLockKeysEnabled) {
    if (lockKeysConsumersEnabled(m_configService.config())) {
      sources.push_back(&m_poll.lockKeys);
    }
  }
  if (m_poll.pipewire != nullptr) {
    sources.push_back(m_poll.pipewire.get());
  }
  if (m_poll.pipewireSpectrum != nullptr) {
    sources.push_back(m_poll.pipewireSpectrum.get());
  }
  if (m_andromedaAudioProvider != nullptr) {
    sources.push_back(m_andromedaAudioProvider.get());
  }
  if (m_wirePlumberMixer != nullptr) {
    sources.push_back(m_wirePlumberMixer.get());
  }
  if (m_poll.polkit != nullptr) {
    sources.push_back(m_poll.polkit.get());
  }
  if (m_poll.brightness != nullptr) {
    sources.push_back(m_poll.brightness.get());
  }
  if (m_poll.memoryPressure != nullptr) {
    sources.push_back(m_poll.memoryPressure.get());
  }
  sources.push_back(&m_poll.fileWatch);
  sources.push_back(&m_poll.ipc);
  sources.push_back(&m_askpassBroker);
  sources.push_back(&m_dmenuIpc);
  sources.push_back(&m_poll.httpClient);
  sources.push_back(&m_poll.location);
  sources.push_back(&m_poll.weather);
  sources.push_back(&m_poll.calendar);
  sources.push_back(&m_poll.calendarReminder);
  sources.push_back(&m_thumbnailService);
  sources.push_back(&m_wallpaperScanner);
  sources.push_back(&m_asyncTextureCache);
  sources.push_back(&m_sharedTextureCache);
  sources.push_back(&m_pixelStreamService);
  static AtspiPollSource atspiPollSource;
  sources.push_back(&atspiPollSource);
  return sources;
}

std::vector<PollSource*> Application::buildPollSources() {
  if (m_bus != nullptr) {
    if (m_poll.bus == nullptr) {
      m_poll.bus = std::make_unique<SessionBusPollSource>(*m_bus);
    }
  } else {
    m_poll.bus.reset();
  }
  if (screenSaver() != nullptr && screenSaver()->hasScreenSaverBus()) {
    if (m_poll.screenSaver == nullptr) {
      m_poll.screenSaver = std::make_unique<ScreenSaverPollSource>(*screenSaver());
    }
  } else {
    m_poll.screenSaver.reset();
  }
  if (m_systemBus != nullptr) {
    if (m_poll.systemBus == nullptr) {
      m_poll.systemBus = std::make_unique<SystemBusPollSource>(*m_systemBus);
    }
  } else {
    m_poll.systemBus.reset();
  }
  if (pipewire() != nullptr) {
    if (m_poll.pipewire == nullptr) {
      m_poll.pipewire = std::make_unique<PipeWirePollSource>(*pipewire());
    }
  } else {
    m_poll.pipewire.reset();
  }
  if (m_pipewireSpectrum != nullptr) {
    if (m_poll.pipewireSpectrum == nullptr) {
      m_poll.pipewireSpectrum = std::make_unique<PipeWireSpectrumPollSource>(*m_pipewireSpectrum);
    }
  } else {
    m_poll.pipewireSpectrum.reset();
  }
  if (m_brightnessService != nullptr) {
    if (m_poll.brightness == nullptr) {
      m_poll.brightness = std::make_unique<BrightnessPollSource>(*m_brightnessService);
    }
  } else {
    m_poll.brightness.reset();
  }
  if (m_poll.memoryPressure == nullptr) {
    m_poll.memoryPressure = std::make_unique<MemoryPressurePollSource>([this] {
      m_renderContext.releaseUnusedCaches();
      trimAllocator();
    });
  }
  return currentPollSources();
}
