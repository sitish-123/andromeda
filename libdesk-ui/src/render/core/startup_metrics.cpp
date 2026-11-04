#include "render/core/startup_metrics.h"

#include "core/log.h"

#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace startup_metrics {
  namespace {

    constexpr Logger kLog("startup");

    double bootMs() {
      timespec ts{};
      clock_gettime(CLOCK_BOOTTIME, &ts);
      return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
    }

    // Field 22 of /proc/self/stat: start time in clock ticks after boot.
    double processStartBootMs() {
      std::ifstream stat("/proc/self/stat");
      std::string line;
      std::getline(stat, line);
      const auto close = line.rfind(')');
      if (close == std::string::npos) {
        return bootMs();
      }
      std::istringstream fields(line.substr(close + 2));
      std::string field;
      for (int i = 3; i <= 22 && fields >> field; ++i) {
      }
      const long ticksPerSecond = sysconf(_SC_CLK_TCK);
      return ticksPerSecond > 0 ? std::stod(field) * 1000.0 / static_cast<double>(ticksPerSecond) : bootMs();
    }

    double g_eglInitMs = 0.0;
    double g_programMs = 0.0;
    int g_programCount = 0;
    bool g_firstFrameLogged = false;

  } // namespace

  double sinceProcessStart() {
    static const double start = processStartBootMs();
    return bootMs() - start;
  }

  void recordEglInit(double ms) { g_eglInitMs += ms; }

  void recordProgramBuild(double ms) {
    g_programMs += ms;
    ++g_programCount;
  }

  void recordFirstFrame() {
    if (g_firstFrameLogged) {
      return;
    }
    g_firstFrameLogged = true;
    kLog.info(
        "first frame at {:.0f} ms after process start; EGL init {:.1f} ms; {} GL programs built in {:.1f} ms",
        sinceProcessStart(), g_eglInitMs, g_programCount, g_programMs
    );
  }

} // namespace startup_metrics
