#include "crawler.h"

#include "skip_rules.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <string>

namespace andromeda::index {

  namespace {
    constexpr std::size_t kBatchRows = 2000;
    constexpr unsigned kMinThreads = 8; // folder reads wait on I/O, so more threads than cores still pays

    struct Row {
      std::int64_t id;
      std::int64_t parent;
      std::string name;
      std::int64_t mtime;
      std::int64_t size;
      bool isDir;
    };

    struct Job {
      std::string path;
      std::int64_t id;
      bool root; // markers do not skip a root
    };

    // Folder reads run on a thread pool; one writer thread (the caller) owns the database. Ids are assigned when a
    // folder is found, so a child row never waits for its parent's insert.
    class ParallelCrawl {
    public:
      ParallelCrawl(IndexDb& db, CrawlStats& stats, dev_t device) : m_db(db), m_stats(stats), m_device(device) {}

      std::int64_t newId() { return m_nextId.fetch_add(1); }

      void push(Job job) {
        const std::lock_guard lock(m_jobsMutex);
        m_jobs.push_back(std::move(job));
        m_jobsCv.notify_one();
      }

      void run() {
        const unsigned threads = std::max(kMinThreads, std::thread::hardware_concurrency());
        std::vector<std::jthread> pool;
        for (unsigned i = 0; i < threads; ++i) {
          pool.emplace_back([this] { work(); });
        }
        write(threads);
      }

    private:
      void work() {
        std::vector<Row> rows;
        while (true) {
          Job job;
          {
            std::unique_lock lock(m_jobsMutex);
            m_jobsCv.wait(lock, [this] { return !m_jobs.empty() || m_active == 0; });
            if (m_jobs.empty()) {
              break;
            }
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            ++m_active;
          }
          std::vector<Job> children;
          readFolder(job, rows, children);
          {
            const std::lock_guard lock(m_jobsMutex);
            for (Job& child : children) {
              m_jobs.push_back(std::move(child));
            }
            --m_active;
          }
          m_jobsCv.notify_all();
          if (rows.size() >= kBatchRows) {
            flush(rows);
          }
        }
        flush(rows);
        const std::lock_guard lock(m_rowsMutex);
        ++m_finished;
        m_rowsCv.notify_one();
      }

      void readFolder(const Job& job, std::vector<Row>& rows, std::vector<Job>& children) {
        const int fd = open(job.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
          return;
        }
        DIR* stream = fdopendir(fd);
        if (stream == nullptr) {
          close(fd);
          return;
        }
        std::vector<std::string> names;
        while (const dirent* e = readdir(stream)) {
          const std::string_view name = e->d_name;
          if (name == "." || name == "..") {
            continue;
          }
          if (skipMarkerName(name) && !job.root) {
            m_skipped.fetch_add(1);
            closedir(stream);
            return;
          }
          if (!hiddenName(name)) {
            names.emplace_back(name);
          }
        }
        m_folders.fetch_add(1);
        for (std::string& name : names) {
          struct stat st {};
          if (fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || S_ISLNK(st.st_mode)) {
            continue;
          }
          const bool isDir = S_ISDIR(st.st_mode);
          // Another filesystem below home is a mount (phone, network share, removable drive): never crossed.
          if ((!isDir && !S_ISREG(st.st_mode)) || (isDir && (skipFolderName(name) || st.st_dev != m_device))) {
            m_skipped.fetch_add(isDir ? 1 : 0);
            continue;
          }
          const std::int64_t id = newId();
          if (isDir) {
            children.push_back({job.path == "/" ? "/" + name : job.path + "/" + name, id, false});
          } else {
            m_files.fetch_add(1);
          }
          rows.push_back({id, job.id, std::move(name), st.st_mtim.tv_sec, isDir ? 0 : st.st_size, isDir});
        }
        closedir(stream);
      }

      void flush(std::vector<Row>& rows) {
        if (rows.empty()) {
          return;
        }
        const std::lock_guard lock(m_rowsMutex);
        m_batches.push_back(std::move(rows));
        rows = {};
        m_rowsCv.notify_one();
      }

      void write(unsigned threads) {
        using Clock = std::chrono::steady_clock;
        std::int64_t dbUs = 0;
        while (true) {
          std::vector<Row> batch;
          {
            std::unique_lock lock(m_rowsMutex);
            m_rowsCv.wait(lock, [&] { return !m_batches.empty() || m_finished == threads; });
            if (m_batches.empty()) {
              break;
            }
            batch = std::move(m_batches.front());
            m_batches.pop_front();
          }
          const auto t = Clock::now();
          m_db.begin();
          for (const Row& row : batch) {
            m_db.insert(row.id, row.parent, row.name, row.mtime, row.size, row.isDir);
          }
          m_db.commit();
          dbUs += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t).count();
        }
        m_stats.files = m_files.load();
        m_stats.folders = m_folders.load();
        m_stats.skippedFolders = m_skipped.load();
        m_stats.dbMs = dbUs / 1000;
      }

      IndexDb& m_db;
      CrawlStats& m_stats;
      const dev_t m_device;
      std::atomic<std::int64_t> m_nextId{1};
      std::atomic<std::int64_t> m_files{0};
      std::atomic<std::int64_t> m_folders{0};
      std::atomic<std::int64_t> m_skipped{0};

      std::mutex m_jobsMutex;
      std::condition_variable m_jobsCv;
      std::deque<Job> m_jobs;
      int m_active = 0;

      std::mutex m_rowsMutex;
      std::condition_variable m_rowsCv;
      std::deque<std::vector<Row>> m_batches;
      unsigned m_finished = 0;
    };
  } // namespace

  CrawlStats crawl(IndexDb& db, const std::filesystem::path& root) {
    using Clock = std::chrono::steady_clock;
    CrawlStats stats;
    const auto start = Clock::now();
    struct stat rootStat {};
    stat(root.c_str(), &rootStat);
    ParallelCrawl walk(db, stats, rootStat.st_dev);
    const std::int64_t rootId = walk.newId();
    db.insert(rootId, 0, root.string(), rootStat.st_mtim.tv_sec, 0, true);
    walk.push({root.string(), rootId, true});
    walk.run();
    stats.walkMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    return stats;
  }

} // namespace andromeda::index
