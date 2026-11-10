#pragma once

#include "compositors/workspace_backend.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

struct wl_output;
struct ext_workspace_manager_v1;

class WaylandWorkspaces {
public:
  using ChangeCallback = std::function<void()>;

  WaylandWorkspaces();
  ~WaylandWorkspaces();

  void bindExtWorkspace(ext_workspace_manager_v1* manager);
  void setOutputNameResolver(const std::function<std::string(wl_output*)>& resolver);
  void initialize();
  void onOutputAdded(wl_output* output);
  void onOutputRemoved(wl_output* output);
  void setChangeCallback(ChangeCallback callback);
  void activate(const std::string& id);
  void activateForOutput(wl_output* output, const std::string& id);
  void activateForOutput(wl_output* output, const Workspace& workspace);
  void cleanup();
  [[nodiscard]] int pollFd() const noexcept;
  [[nodiscard]] short pollEvents() const noexcept;
  [[nodiscard]] int pollTimeoutMs() const noexcept;
  void dispatchPoll(short revents);
  [[nodiscard]] const char* backendName() const noexcept;
  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>> appIdsByWorkspace(wl_output* output) const;
  [[nodiscard]] TaskbarAssignmentMode taskbarAssignmentMode() const noexcept;
  [[nodiscard]] std::unordered_map<std::uintptr_t, WorkspaceWindow>
  assignTaskbarWindows(const std::vector<TaskbarWindowCandidate>& windows, wl_output* output) const;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(wl_output* output) const;
  [[nodiscard]] std::vector<std::string> openOverlayWorkspaceKeys(wl_output* output) const;
  void focusWindow(const std::string& windowId) const;

  [[nodiscard]] std::vector<Workspace> all() const;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const;

private:
  void setActiveBackend(WorkspaceBackend* backend);
  void notifyChanged() const;

  std::vector<std::unique_ptr<WorkspaceBackend>> m_backends;
  std::vector<class OutputLifecycleObserver*> m_outputObservers;
  std::vector<WorkspaceOutputNameResolver*> m_outputNameResolvers;
  ExtWorkspaceProtocolBinder* m_extWorkspaceBinder = nullptr;
  WorkspaceBackend* m_extBackend = nullptr;
  WorkspaceBackend* m_activeBackend = nullptr;
  ChangeCallback m_changeCallback;
};
