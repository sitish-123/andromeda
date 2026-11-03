#pragma once

#include <filesystem>
#include <fstream>
#include <sched.h>
#include <string>
#include <system_error>

namespace andromeda {

  // Mesa submits GL work on its own "gdrv" thread. The main thread waits for it each frame, so it must share the main
  // thread's real-time policy: at SCHED_OTHER a build at nice -4 delayed it 0.77 ms per wake-up and the cursor lagged.
  // Call on the main thread after each GL context is created. Returns the number of threads raised.
  inline int shareSchedulingWithDriverThreads() {
    const int policy = sched_getscheduler(0) & ~SCHED_RESET_ON_FORK;
    sched_param param{};
    if ((policy != SCHED_RR && policy != SCHED_FIFO) || sched_getparam(0, &param) != 0) {
      return 0;
    }
    int raised = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
      std::ifstream commFile(entry.path() / "comm");
      std::string comm;
      std::getline(commFile, comm);
      if (!comm.contains("gdrv")) {
        continue;
      }
      const pid_t tid = static_cast<pid_t>(std::stol(entry.path().filename().string()));
      if (sched_setscheduler(tid, policy | SCHED_RESET_ON_FORK, &param) == 0) {
        ++raised;
      }
    }
    return raised;
  }

} // namespace andromeda
