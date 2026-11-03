#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "system/activity_details.h"
#include "system/background_activity_monitor.h"
#include "system/icon_resolver.h"
#include "ui/controls/scroll_view.h"
#include "wayland/toplevel_surface.h"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>

class ActivityActions;
class Button;
class ActivityMonitor;
class ConfigService;
class Flex;
class Input;
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

// The Activity app: an xdg-toplevel with a sidebar (Apps, Background), a row list and an inspector.
// Holds an ActivityMonitor reference while open, so sampling runs only while the window shows.
class ActivityWindow {
public:
  enum class Tab : std::uint8_t { Apps, Background, Battery, History };

  ActivityWindow();
  ~ActivityWindow();

  void initialize(
      ShellWaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, ActivityMonitor* monitor,
      ActivityActions* actions
  );

  // Verdicts for the Background cards and the "Needs attention" card; may stay null (no warnings).
  void setBackgroundMonitor(BackgroundActivityMonitor* monitor) { m_background = monitor; }
  // GPU shares, history and process groups; owned by the caller so 24 h history outlives the window.
  void setDetails(ActivityDetails* details) { m_details = details; }
  // Services that start after the window exists; asked for on open.
  void setSystemMonitorProvider(std::function<SystemMonitorService*()> provider) {
    m_sysmonProvider = std::move(provider);
  }
  void setUPowerProvider(std::function<UPowerService*()> provider) { m_upowerProvider = std::move(provider); }

  // rowKey selects that row (and its tab) when it exists.
  void open(const std::string& rowKey = {});
  void close();

  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr && m_surface->isRunning(); }
  [[nodiscard]] bool ownsKeyboardSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  void onThemeChanged() { requestRebuild(); }

private:
  void destroyWindow();
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void buildScene(Renderer& renderer, float width, float height);
  [[nodiscard]] std::unique_ptr<Flex> buildSidebar(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildList(Renderer& renderer, float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildInspector(Renderer& renderer, float scale);
  [[nodiscard]] std::unique_ptr<Button> buildRow(Renderer& renderer, const ActivityRow& row, float scale);
  void addRow(Renderer& renderer, Flex& card, const ActivityRow& row, float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildSystemGraphs(float scale);
  void addBatteryCards(Renderer& renderer, Flex& list, float scale);
  void addHistoryCards(Renderer& renderer, Flex& list, float scale);
  void onSample();
  void forceQuitSelected();
  [[nodiscard]] const ActivityRow* selectedRow() const;
  [[nodiscard]] const BackgroundActivityMonitor::RowState* rowState(const std::string& key) const;
  [[nodiscard]] bool needsAttention(const ActivityRow& row) const;
  void refreshRowStates(bool force);
  [[nodiscard]] bool inTab(const ActivityRow& row, Tab tab) const;
  void stopSelected();
  void requestRebuild();
  void invalidate();
  [[nodiscard]] float uiScale() const;
  // Theme icon for name, else the generic executable icon; empty when the theme has neither.
  [[nodiscard]] std::string iconPath(const std::string& name, int sizePx);

  ShellWaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  ActivityMonitor* m_monitor = nullptr;
  ActivityActions* m_actions = nullptr;
  BackgroundActivityMonitor* m_background = nullptr;
  ActivityDetails* m_details = nullptr;
  std::function<SystemMonitorService*()> m_sysmonProvider;
  std::function<UPowerService*()> m_upowerProvider;
  SystemMonitorService* m_sysmon = nullptr; // retained while open
  std::deque<float> m_diskHistory;          // MB/s summed over rows, one per sample
  std::map<std::string, BackgroundActivityMonitor::RowState> m_rowStates;
  std::chrono::steady_clock::time_point m_rowStatesAt;

  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  Flex* m_body = nullptr;
  AnimationManager m_animations;
  InputDispatcher m_inputDispatcher;
  IconResolver m_icons;
  ScrollViewState m_listScroll;
  wl_output* m_output = nullptr;

  Tab m_tab = Tab::Apps;
  std::string m_selectedKey;
  std::string m_query;
  Input* m_searchInput = nullptr;
  Node m_placeholderRoot;             // input root while the scene rebuilds
  std::unique_ptr<Node> m_keptSearch; // the search field between removal from the old tree and the new sidebar
  float m_searchScale = 0.0F;
  std::string m_actionError;
  bool m_pointerInside = false;
  bool m_rebuildRequested = false;
  bool m_buttonDown = false;
};
