#pragma once

#include "dbus/systemd/systemd_units_service.h"
#include "system/container_service.h"

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "system/activity_details.h"
#include "system/background_activity_monitor.h"
#include "system/battery_impact.h"
#include "system/icon_resolver.h"
#include "ui/controls/scroll_view.h"
#include "wayland/toplevel_surface.h"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <unordered_map>
#include <set>
#include <string>
#include <vector>

class ActivityActions;
class Button;
class ActivityMonitor;
class ConfigService;
class Flex;
class Graph;
class Input;
class Label;
class RenderContext;
class Renderer;
class ShellWaylandConnection;
class SystemMonitorService;
class UPowerService;
struct ActivityRow;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;

// The Activity app (DESIGN.md "Activity v3"): an xdg-toplevel with a toolbar pane switch, a grouped process table,
// a problem card while a pressure is high, a graph strip and an inspector for the selected row.
// Holds an ActivityMonitor reference while open, so sampling runs only while the window shows.
class ActivityWindow {
public:
  enum class Pane : std::uint8_t { Cpu, Memory, Energy, Disk, Network, Containers };

  ActivityWindow();
  ~ActivityWindow();

  void initialize(
      ShellWaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, ActivityMonitor* monitor,
      ActivityActions* actions
  );

  // Verdicts for the background rows and the problem card; may stay null (no warnings).
  void setBackgroundMonitor(BackgroundActivityMonitor* monitor) { m_background = monitor; }
  // GPU shares, history and process groups; owned by the caller so 24 h history outlives the window.
  void setDetails(ActivityDetails* details) { m_details = details; }
  // Services that start after the window exists; asked for on open.
  void setSystemMonitorProvider(std::function<SystemMonitorService*()> provider) {
    m_sysmonProvider = std::move(provider);
  }
  void setUPowerProvider(std::function<UPowerService*()> provider) { m_upowerProvider = std::move(provider); }
  // Docker / Podman engines for the Containers pane; the pane shows only when an engine socket exists.
  void setContainers(ContainerService* containers) { m_containers = containers; }

  // rowKey selects that row when it exists; "containers" or "ctr:<id>" opens the Containers pane.
  void open(const std::string& rowKey = {});
  void close();

  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr && m_surface->isRunning(); }
  [[nodiscard]] bool ownsKeyboardSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  void onThemeChanged() { requestRebuild(); }
  // Visible buttons as a JSON array of {text, x, y} centres in surface coordinates, for E2E clicks.
  [[nodiscard]] std::string buttonsJson() const;

private:
  enum class Confirm : std::uint8_t { None, Quit, ForceQuit };
  enum class Problem : std::uint8_t { None, Memory, Cpu, Background };
  // One table column of a pane: header key, cell text and the value it sorts and shades by.
  struct Column {
    std::string header;
    std::function<std::string(const ActivityRow&)> text;
    std::function<double(const ActivityRow&)> value;
    bool muted = false;
    char group = 0; // 'c' CPU or 'm' memory: the value a process-group child row shows in this column
  };

  void destroyWindow();
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void buildScene(Renderer& renderer, float width, float height);
  // activity_table.cpp
  [[nodiscard]] std::unique_ptr<Flex> buildToolbar(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildProblemCard(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildTable(Renderer& renderer, float width, float scale);
  [[nodiscard]] std::unique_ptr<Button> buildRow(
      Renderer& renderer, const ActivityRow& row, const std::vector<Column>& columns, float nameWidth, float scale
  );
  void addChildRows(Flex& list, const ActivityRow& row, const std::vector<Column>& columns, float nameWidth, float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildStrip(float width, float scale);
  [[nodiscard]] std::vector<Column> columnsFor(Pane pane) const;
  [[nodiscard]] std::size_t sortColumn() const;
  void computeOrder();
  [[nodiscard]] Problem problem(std::string* rowKey) const;
  // activity_inspector.cpp
  [[nodiscard]] std::unique_ptr<Flex> buildInspector(Renderer& renderer, float scale);
  void addOverview(Flex& details, const ActivityRow& row, float scale);
  void addProcesses(Flex& details, const ActivityRow& row, float scale);
  void addActions(Flex& inspector, const ActivityRow& row, float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildContainers(float scale);
  void addContainerCards(Flex& list, float scale);
  void addContainerInspector(Flex& inspector, const ContainerInfo& container, float scale);

  void onSample();
  // After a sample: updates the bound labels, fills and graphs in place when structureKey() still matches the
  // scene, else rebuilds it. A full rebuild costs several ms on the main thread; an in-place update well under one.
  void refreshAfterSample();
  [[nodiscard]] std::string structureKey() const;
  [[nodiscard]] std::string captionFor(const ActivityRow& row) const;
  // Desktop-entry name and icon; else the unit Description for a service; else the raw ID.
  [[nodiscard]] RowIdentity identity(const ActivityRow& row) const;
  [[nodiscard]] const ActivityRow* findRow(const std::string& key) const;
  // True when the row's first unit cgroup reports "frozen 1" in cgroup.events (Pause).
  [[nodiscard]] bool isFrozen(const ActivityRow& row) const;
  // Limit picker indexes (0 = Off) read from the first unit cgroup's cpu.max and memory.high.
  struct Limits {
    std::size_t cpu = 0;
    std::size_t memory = 0;
  };
  [[nodiscard]] Limits limitsOf(const ActivityRow& row) const;
  // cgroup files of the selected row, the limited rows and the engine services, read on the SharedWorker.
  struct CgroupControls {
    bool frozen = false;
    double cpuShare = 0.0;   // cpu.max quota / period; 0 = max
    double memoryHigh = 0.0; // bytes; 0 = max
    std::uint64_t memoryCurrent = 0;
    bool operator==(const CgroupControls&) const = default;
  };
  [[nodiscard]] const CgroupControls* controlsOf(const ActivityRow& row) const;
  void readControlsAsync();
  // One Limit picker change; the other field keeps its pending pick, or its exact cgroup value.
  void setSelectedLimit(bool cpuField, std::size_t index);
  [[nodiscard]] Limits shownLimits(const ActivityRow& row) const;
  void indexRows();
  void setSelectedFrozen(bool frozen);
  void updateLive();
  void forceQuitSelected();
  void stopSelected();
  void select(const std::string& key);
  void moveSelection(int delta);
  void setPane(Pane pane);
  [[nodiscard]] const ActivityRow* selectedRow() const;
  [[nodiscard]] const BackgroundActivityMonitor::RowState* rowState(const std::string& key) const;
  [[nodiscard]] bool needsAttention(const ActivityRow& row) const;
  void refreshRowStates(bool force);
  [[nodiscard]] bool matchesQuery(const ActivityRow& row) const;
  [[nodiscard]] bool containersAvailable() const;
  [[nodiscard]] double gpuOf(const ActivityRow& row) const;
  // Lists containers every sample on the Containers pane, else every 5th; engine units every 5th, before the list.
  void refreshContainers();
  // An engine whose socket unit listens while its service is down is not asked: the request would start it.
  void listContainers();
  void runContainerAction(const ContainerInfo& container, int action);
  [[nodiscard]] std::vector<SystemdUnit> engineUnits(const ContainerEngine& engine) const;
  void requestRebuild();
  void invalidate();
  void syncBlur();
  [[nodiscard]] float uiScale() const;
  // Theme icon for name, else the generic executable icon; empty when the theme has neither.
  [[nodiscard]] std::string iconPath(const std::string& name, int sizePx);

  ShellWaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  ActivityMonitor* m_monitor = nullptr;
  ActivityActions* m_actions = nullptr;
  ContainerService* m_containers = nullptr;
  std::map<std::string, SystemdUnit> m_engineUnits; // by engine socket + "|" + unit name
  unsigned m_containerTicks = 0;
  bool m_engineUnitsRead = false; // no list before the first unit read, so an idle engine is never woken
  BackgroundActivityMonitor* m_background = nullptr;
  ActivityDetails* m_details = nullptr;
  std::function<SystemMonitorService*()> m_sysmonProvider;
  std::function<UPowerService*()> m_upowerProvider;
  SystemMonitorService* m_sysmon = nullptr; // retained while open
  // One point per sample, 2 min: disk MB/s over rows, highest memory pressure avg10 %, battery watts.
  std::deque<float> m_diskHistory;
  std::deque<float> m_pressureHistory;
  std::deque<float> m_powerHistory;
  std::map<std::string, BackgroundActivityMonitor::RowState> m_rowStates;
  std::chrono::steady_clock::time_point m_rowStatesAt;
  bool m_rowStatesReading = false;
  bool m_rowStatesAgain = false;
  std::unordered_map<std::string, std::size_t> m_rowIndex; // row key -> index in rows(), rebuilt per sample
  struct PendingLimits {
    std::string key;
    Limits picks;
    std::uint64_t cpuUsec = 0;     // CPUQuotaPerSecUSec to send
    std::uint64_t memoryBytes = 0; // MemoryHigh to send
  };
  std::optional<PendingLimits> m_pendingLimits; // picks sent from this window, until the selection changes

  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  Node* m_body = nullptr;
  AnimationManager m_animations;
  InputDispatcher m_inputDispatcher;
  IconResolver m_icons;
  ScrollViewState m_listScroll;
  ScrollViewState m_inspectorScroll;
  wl_output* m_output = nullptr;
  bool m_glass = false;

  Pane m_pane = Pane::Memory;
  std::map<Pane, std::size_t> m_sortBy; // a header click; default: the pane's first value column
  bool m_processesTab = false;          // inspector segment: Overview / Processes
  bool m_inspectorHidden = false;       // Space toggles it while a row is selected
  Confirm m_confirm = Confirm::None;
  std::string m_selectedKey;
  std::string m_query;
  std::vector<std::string> m_order; // visible row keys, by section then sort column
  std::chrono::steady_clock::time_point m_orderAt;
  std::chrono::steady_clock::time_point m_openedAt;
  Timer m_skeletonTimer;
  Input* m_searchInput = nullptr;
  Node m_placeholderRoot;             // input root while the scene rebuilds
  std::unique_ptr<Node> m_keptSearch; // the search field between removal from the old tree and the new toolbar
  float m_searchScale = 0.0F;
  std::string m_actionError;
  std::set<std::string> m_limitedKeys; // rows limited from this window; only these get the status mark
  bool m_pointerInside = false;
  bool m_rebuildRequested = false;
  bool m_buttonDown = false;

  struct LiveText {
    Label* label = nullptr;
    std::function<std::string()> text;
  };
  struct LiveGraph {
    Graph* graph = nullptr;
    std::function<std::vector<float>()> values;
  };
  struct LiveHeat {
    Flex* cell = nullptr;
    // 0..1 of the column's largest visible value; a null cell runs first and only refreshes that maximum.
    std::function<float()> share;
  };
  std::vector<LiveText> m_liveText; // pointers into m_sceneRoot; cleared on each rebuild
  std::vector<LiveGraph> m_liveGraphs;
  std::vector<LiveHeat> m_liveHeat;
  std::vector<std::function<void()>> m_liveCalls; // other per-sample updates (accessible descriptions)
  double m_heatTop = 0.0; // largest sort-column value among the visible rows
  float m_columnWidth = 0.0F; // value column width of the current table, in surface px
  std::string m_structure; // structureKey() of the current scene
  std::unordered_map<std::string, CgroupControls> m_controls; // by unit cgroup path
  bool m_controlsReading = false;
  bool m_controlsAgain = false;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
