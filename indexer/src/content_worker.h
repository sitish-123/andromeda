#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace andromeda::index {

  // Reads the text of new and changed files into the content index. Two threads, each with its own database
  // connection: the live lane reads up to kLiveBatch files changed in the last kLiveWindow at SCHED_BATCH, so a busy
  // CPU cannot starve a fresh file; the bulk lane reads the rest at SCHED_IDLE with idle I/O priority. Both pause
  // while a battery runs below kLowBattery percent and discharges.
  class ContentWorker {
  public:
    static constexpr int kLowBattery = 20;
    static constexpr int kLiveBatch = 32;
    static constexpr auto kLiveWindow = std::chrono::minutes(10);

    explicit ContentWorker(std::filesystem::path dbFile);
    ~ContentWorker();
    ContentWorker(const ContentWorker&) = delete;
    ContentWorker& operator=(const ContentWorker&) = delete;

    // Files changed: look for work again. Live: the live lane first, then the bulk lane.
    void wake();
    void wakeLive();

  private:
    struct Lane {
      std::condition_variable cv;
      bool woken = false;
    };

    void run(bool live);
    void wakeLane(Lane& lane);
    bool claim(std::int64_t id);
    void release(std::int64_t id);

    std::filesystem::path m_dbFile;
    std::mutex m_mutex;
    Lane m_bulk;
    Lane m_live;
    std::unordered_set<std::int64_t> m_claimed;
    bool m_stop = false;
    std::thread m_bulkThread;
    std::thread m_liveThread;
  };

  // True while a battery discharges below percent.
  [[nodiscard]] bool batteryLow(int percent, const std::filesystem::path& powerSupplies = "/sys/class/power_supply");

} // namespace andromeda::index
