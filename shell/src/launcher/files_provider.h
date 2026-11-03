#pragma once

#include "core/poll_source.h"
#include "launcher/launcher_provider.h"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ClipboardService;
class SessionBus;
namespace sdbus {
  class IProxy;
}

// Files from the andromeda-index daemon ($XDG_RUNTIME_DIR/andromeda-index.sock). Queries go out on one socket that
// the main loop polls; answers arrive in request order, and only the answer to the newest query is shown.
class FilesProvider final : public LauncherProvider, public PollSource {
public:
  static constexpr std::size_t kMinQuery = 3;
  static constexpr int kLimit = 8;

  FilesProvider(ClipboardService* clipboard, SessionBus* bus);
  ~FilesProvider() override;
  FilesProvider(const FilesProvider&) = delete;
  FilesProvider& operator=(const FilesProvider&) = delete;

  [[nodiscard]] std::string_view defaultPrefix() const override { return "file"; }
  [[nodiscard]] std::string_view id() const override { return "Files"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "folder"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  // Files the user opens, reveals or copies rise in the section by frecency.
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] bool isLoading() const override { return m_loading; }
  [[nodiscard]] std::optional<std::string> answeredQuery() const override { return m_answered; }
  void setResultsChangedCallback(std::function<void()> callback) override { m_onResultsChanged = std::move(callback); }

  void reset() override;
  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

  // Opens the folder of a result with the file selected.
  void reveal(const LauncherResult& result) const;
  void copyPath(const LauncherResult& result) const;

  [[nodiscard]] int pollTimeoutMs() const override { return -1; }
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  [[nodiscard]] bool connect() const;
  void disconnect() const;
  void onAnswer(const std::string& text, const std::string& body) const;

  ClipboardService* m_clipboard = nullptr;
  SessionBus* m_bus = nullptr;
  std::function<void()> m_onResultsChanged;
  mutable int m_fd = -1;
  mutable std::string m_received;
  mutable std::deque<std::string> m_pending; // queries sent, oldest first
  mutable std::optional<std::string> m_requested;
  mutable std::optional<std::string> m_answered;
  mutable std::vector<LauncherResult> m_results;
  mutable bool m_loading = false;
  mutable std::unique_ptr<sdbus::IProxy> m_fileManager; // kept alive for pending ShowItems replies
};
