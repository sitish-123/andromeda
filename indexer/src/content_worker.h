#pragma once

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace andromeda::index {

  // Reads the text of new and changed files into the content index, on its own thread and database connection, at
  // SCHED_IDLE with idle I/O priority. Pauses while a battery runs below kLowBattery percent and discharges.
  class ContentWorker {
  public:
    static constexpr int kLowBattery = 20;

    explicit ContentWorker(std::filesystem::path dbFile);
    ~ContentWorker();
    ContentWorker(const ContentWorker&) = delete;
    ContentWorker& operator=(const ContentWorker&) = delete;

    // Files changed: look for work again.
    void wake();

  private:
    void run();

    std::filesystem::path m_dbFile;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_woken = true;
    bool m_stop = false;
    std::thread m_thread;
  };

  // True while a battery discharges below percent.
  [[nodiscard]] bool batteryLow(int percent, const std::filesystem::path& powerSupplies = "/sys/class/power_supply");

} // namespace andromeda::index
