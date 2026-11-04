#pragma once

// Startup breakdown for the launch-time budgets (docs/performance.md). Main thread only.
namespace startup_metrics {

  // Milliseconds since the process started (kernel start time, 10 ms resolution).
  [[nodiscard]] double sinceProcessStart();
  void recordEglInit(double ms);
  void recordProgramBuild(double ms);
  // Logs the breakdown once, on the first presented frame.
  void recordFirstFrame();

} // namespace startup_metrics
