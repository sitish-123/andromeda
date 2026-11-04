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
        std::vector<std::string>{"coredumpctl", "list", "--json=short", "--since=-30d", "--no-pager"},
        process::RunCallbacks{
            .onExit =
                [this, alive = std::weak_ptr<void>(m_aliveGuard)](process::RunResult result) {
                  // Runs on the process worker thread: parse here, apply on the main loop.
                  // coredumpctl exits 1 with no output when there are no dumps.
                  const bool available = result.exitCode == 0 || (result.exitCode == 1 && result.out.empty());
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
