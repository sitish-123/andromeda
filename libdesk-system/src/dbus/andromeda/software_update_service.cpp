#include "dbus/andromeda/software_update_service.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "dbus/system_bus.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <string_view>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>

namespace {

  constexpr Logger kLog("software-update");
  const sdbus::ServiceName kUpdateService{"org.andromeda.Update1"};
  const sdbus::ObjectPath kUpdatePath{"/org/andromeda/Update1"};
  constexpr auto kUpdateName = "org.andromeda.Update1";
  const sdbus::ServiceName kHelperService{"org.andromeda.UpdateHelper1"};
  const sdbus::ObjectPath kHelperPath{"/org/andromeda/UpdateHelper1"};
  constexpr auto kHelperName = "org.andromeda.UpdateHelper1";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  using VariantMap = std::map<std::string, sdbus::Variant>;
  using UpdateRow = sdbus::Struct<std::string, std::string, std::string, std::string, std::uint64_t>;
  using NewsRow = sdbus::Struct<std::string, std::string, std::int64_t>;

  // Packages whose new version runs only after a new session or boot.
  bool needsRestart(const std::string& target) {
    static constexpr std::array kPrefixes{"linux", "mesa", "systemd", "glibc", "nvidia", "intel-ucode", "amd-ucode"};
    const std::string name = target.substr(0, target.find(' '));
    return std::ranges::any_of(kPrefixes, [&name](std::string_view prefix) {
      return name == prefix || (name.starts_with(prefix) && name[prefix.size()] == '-');
    });
  }

} // namespace

SoftwareUpdateService::SoftwareUpdateService(SessionBus& session, SystemBus* system)
    : m_session(session), m_system(system) {
  m_update = sdbus::createProxy(m_session.connection(), kUpdateService, kUpdatePath);
  m_update->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](const std::string& iface, const VariantMap& changed, const std::vector<std::string>& invalidated) {
        if (iface != kUpdateName) {
          return;
        }
        if (!changed.empty() || !invalidated.empty()) {
          load();
        }
      });
  if (m_system != nullptr) {
    m_helper = sdbus::createProxy(m_system->connection(), kHelperService, kHelperPath);
    m_busDaemon = sdbus::createProxy(
        m_system->connection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
    );
    m_busDaemon->uponSignal("NameOwnerChanged")
        .onInterface("org.freedesktop.DBus")
        .call([this](const std::string& name, const std::string& /*old*/, const std::string& owner) {
          // The helper exited (crash, kill) while our upgrade ran: no Finished will come.
          if (name == kHelperName && owner.empty() && m_runState == RunState::Running) {
            m_runState = RunState::Failed;
            m_resultMessage = "the update service stopped";
            notify();
          }
        });
    m_helper->uponSignal("Progress")
        .onInterface(kHelperName)
        .call([this](const std::string& stage, int percent, const std::string& package) {
          m_runState = RunState::Running;
          m_stage = stage;
          m_percent = std::clamp(percent, 0, 100);
          m_stagePackage = package;
          notify();
        });
    m_helper->uponSignal("Finished")
        .onInterface(kHelperName)
        .call([this](bool ok, const std::string& message, const std::vector<std::string>& targets,
                     const std::vector<std::string>& questions) {
          // Tonight's run may be refused before any Progress: its Finished still carries the reason.
          if (m_runState != RunState::Running && !m_tonight) {
            return;
          }
          m_tonight = false;
          m_resultMessage = message;
          m_questions = questions;
          m_runState = ok ? RunState::Done : (questions.empty() ? RunState::Failed : RunState::NeedsChoice);
          if (ok && std::ranges::any_of(targets, needsRestart)) {
            m_restartNeeded = true;
          }
          notify();
          if (ok) {
            check();
          }
        });
  }
}

SoftwareUpdateService::~SoftwareUpdateService() = default;

void SoftwareUpdateService::refresh() {
  load();
  if (m_helper == nullptr) {
    return;
  }
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    // An upgrade another client (or tonight's timer) started is shown as running; its signals follow.
    m_helper->callMethodAsync("GetAll").onInterface(kPropertiesInterface).withArguments(std::string{kHelperName}).uponReplyInvoke(
        [this, alive](const std::optional<sdbus::Error>& error, const VariantMap& props) {
          if (alive.expired() || error.has_value()) {
            return;
          }
          const auto prop = [&props](const char* name) {
            const auto it = props.find(name);
            return it != props.end() && it->second.get<bool>();
          };
          const bool running = prop("Running");
          if (prop("Tonight") != m_tonight) {
            m_tonight = !m_tonight;
            notify();
          }
          if (running && m_runState != RunState::Running) {
            m_runState = RunState::Running;
            m_stage.clear();
            notify();
          }
        }
    );
  } catch (const sdbus::Error& e) {
    kLog.debug("helper Running: {}", e.what());
  }
}

void SoftwareUpdateService::load() {
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    m_update->callMethodAsync("GetAll")
        .onInterface(kPropertiesInterface)
        .withArguments(std::string{kUpdateName})
        .uponReplyInvoke([this, alive](const std::optional<sdbus::Error>& error, const VariantMap& props) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.debug("Update1 not available: {}", error->what());
            m_available = false;
            notify();
            return;
          }
          m_available = true;
          try {
            m_updates.clear();
            for (const auto& row : props.at("Updates").get<std::vector<UpdateRow>>()) {
              m_updates.push_back({row.get<0>(), row.get<1>(), row.get<2>(), row.get<3>(), row.get<4>()});
            }
            m_news.clear();
            for (const auto& row : props.at("News").get<std::vector<NewsRow>>()) {
              m_news.push_back({row.get<0>(), row.get<1>(), row.get<2>()});
            }
            std::ranges::sort(m_news, std::ranges::greater{}, &SoftwareUpdateNews::published);
            m_newsBlocksSystem = props.at("NewsBlocksSystem").get<bool>();
            m_lastCheck = props.at("LastCheck").get<std::int64_t>();
            m_checking = props.at("Checking").get<bool>();
            m_errors = props.at("Errors").get<std::vector<std::string>>();
          } catch (const std::exception& e) {
            kLog.warn("Update1 properties: {}", e.what());
          }
          notify();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("Update1: {}", e.what());
  }
}

void SoftwareUpdateService::check() {
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    m_update->callMethodAsync("Check").onInterface(kUpdateName).uponReplyInvoke(
        [this, alive](const std::optional<sdbus::Error>& error) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.warn("check: {}", error->what());
          }
          load();
        }
    );
  } catch (const sdbus::Error& e) {
    kLog.warn("check: {}", e.what());
  }
}

void SoftwareUpdateService::markNewsRead() {
  if (m_news.empty()) {
    return;
  }
  const std::int64_t until = m_news.front().published;
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    // The helper's token gates Upgrade (root); the session token gates what the page shows. Helper first: if the
    // user cancels the polkit prompt, the page keeps showing the news.
    if (m_helper == nullptr) {
      return;
    }
    m_helper->callMethodAsync("MarkNewsRead")
        .onInterface(kHelperName)
        .withArguments(until)
        .withTimeout(std::chrono::minutes(5))
        .uponReplyInvoke([this, alive, until](const std::optional<sdbus::Error>& error) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.warn("mark news read (helper): {}", error->what());
            return;
          }
          try {
            m_update->callMethodAsync("MarkNewsRead")
                .onInterface(kUpdateName)
                .withArguments(until)
                .uponReplyInvoke([this, alive](const std::optional<sdbus::Error>& sessionError) {
                  if (alive.expired()) {
                    return;
                  }
                  if (sessionError.has_value()) {
                    kLog.warn("mark news read: {}", sessionError->what());
                  }
                  load();
                });
          } catch (const sdbus::Error& e) {
            kLog.warn("mark news read: {}", e.what());
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("mark news read: {}", e.what());
  }
}

void SoftwareUpdateService::upgrade() {
  if (m_helper == nullptr || m_runState == RunState::Running) {
    return;
  }
  m_runState = RunState::Running;
  m_stage.clear();
  m_stagePackage.clear();
  m_percent = 0;
  m_resultMessage.clear();
  m_questions.clear();
  notify();
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    // The reply comes after the polkit prompt; progress and the result follow as signals.
    m_helper->callMethodAsync("Upgrade")
        .onInterface(kHelperName)
        .withArguments(false)
        .withTimeout(std::chrono::minutes(5))
        .uponReplyInvoke([this, alive](const std::optional<sdbus::Error>& error) {
          if (alive.expired() || !error.has_value()) {
            return;
          }
          const bool cancelled = error->getName() == "org.freedesktop.DBus.Error.AccessDenied";
          m_runState = cancelled ? RunState::Idle : RunState::Failed;
          m_resultMessage = cancelled ? std::string() : error->getMessage();
          notify();
        });
  } catch (const sdbus::Error& e) {
    m_runState = RunState::Failed;
    m_resultMessage = e.getMessage();
    notify();
  }
}

void SoftwareUpdateService::scheduleTonight(bool enable) {
  if (m_helper == nullptr) {
    return;
  }
  const std::weak_ptr<int> alive = m_lifetimeToken;
  try {
    m_helper->callMethodAsync("ScheduleTonight")
        .onInterface(kHelperName)
        .withArguments(enable)
        .withTimeout(std::chrono::minutes(5))
        .uponReplyInvoke([this, alive, enable](const std::optional<sdbus::Error>& error) {
          if (alive.expired()) {
            return;
          }
          if (error.has_value()) {
            kLog.warn("schedule tonight: {}", error->what());
            return;
          }
          m_tonight = enable;
          notify();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("schedule tonight: {}", e.what());
  }
}

void SoftwareUpdateService::notify() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}
