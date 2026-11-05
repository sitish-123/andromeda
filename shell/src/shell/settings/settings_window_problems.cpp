#include "core/deferred_call.h"
#include "core/process/process.h"
#include "shell/settings/settings_content_problems.h"
#include "shell/settings/settings_window.h"

#include <chrono>
#include <string>
#include <vector>

void SettingsWindow::addProblemsContent(float scale) {
  if (m_selectedSection != "problems") {
    m_crashRecords.reset();
    return;
  }
  if (!m_crashRecords.has_value() && !m_crashRecordsLoading) {
    m_crashRecordsLoading = true;
    const bool started = process::runAsync(
        std::vector<std::string>{
            "journalctl", "--no-pager", "-o", "json",
            "--output-fields=COREDUMP_EXE,COREDUMP_PID,COREDUMP_TIMESTAMP,COREDUMP_USER_UNIT",
            // systemd-coredump's "process dumped core" message.
            "MESSAGE_ID=fc2e22bc6ee647b6b90729ab34a250b1", "--since=-30d"
        },
        process::RunCallbacks{
            .onExit =
                [this, alive = std::weak_ptr<void>(m_aliveGuard)](process::RunResult result) {
                  // Runs on the process worker thread: parse here, apply on the main loop.
                  const bool available = result.exitCode == 0;
                  auto records = settings::parseCrashRecords(result.out);
                  DeferredCall::callLater([this, alive, available, records = std::move(records)]() mutable {
                    if (alive.expired()) {
                      return;
                    }
                    m_crashRecordsLoading = false;
                    m_crashRecordsAvailable = available;
                    m_crashRecords = std::move(records);
                    if (isOpen()) {
                      requestContentRebuild();
                    }
                  });
                },
        },
        process::RunOptions{.timeout = std::chrono::seconds(10)}
    );
    if (!started) {
      m_crashRecordsLoading = false;
      m_crashRecordsAvailable = false;
      m_crashRecords.emplace();
    }
  }
  settings::addSettingsProblems(*m_contentContainer, scale, m_crashRecords, m_crashRecordsAvailable);
}
