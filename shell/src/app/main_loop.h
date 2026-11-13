#pragma once

#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

class PollSource;
class WaylandConnection;

class MainLoop {
public:
  using PollSourcesProvider = std::function<std::vector<PollSource*>()>;

  // closeSurfaces runs once at shutdown, before the last Wayland flush.
  MainLoop(WaylandConnection& wayland, PollSourcesProvider sourcesProvider, std::function<void()> closeSurfaces = {});

  void run();

  // Developer diagnostics: a main-loop pass of 250 ms or more writes a stall report (protocols/stall_watch.h). Off,
  // the loop arms nothing. Call on the main thread.
  static void setDeveloperDiagnostics(bool on);

private:
  WaylandConnection& m_wayland;
  PollSourcesProvider m_sourcesProvider;
  std::function<void()> m_closeSurfaces;

  // Absolute deadline by which each source must next be dispatched, armed from
  // the timeout it advertised and retained across iterations until it fires.
  std::unordered_map<PollSource*, std::chrono::steady_clock::time_point> m_sourceDeadlines;
};
