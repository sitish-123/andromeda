#pragma once

#include "index_db.h"

#include <poll.h>

#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace andromeda::index {

  // Answers name queries on a Unix stream socket, one line per request:
  //   query <limit> <text>\n
  // A word kind:<folder|pdf|image|video|audio|doc|sheet|slides> in text keeps rows of that kind; with no other
  // words the answer is the newest entries of that kind. An unknown kind gets the empty line only.
  // Each answer is one row per line, "path\tmtime_ns\tsize\tkind\tmatch" (kind f or d; match n = name, c = text
  // inside the file, searched from 4 characters), and ends with an empty line.
  // An unknown request gets the empty line only. Limit is clamped to 1..kMaxLimit.
  class QueryServer {
  public:
    static constexpr int kMaxLimit = 100;

    // Takes ownership of listenFd, a listening socket.
    QueryServer(const IndexDb& db, int listenFd);
    ~QueryServer();
    QueryServer(const QueryServer&) = delete;
    QueryServer& operator=(const QueryServer&) = delete;

    // Appends the fds to poll: the listening socket, then the clients.
    void addPollFds(std::vector<pollfd>& fds) const;
    // Handles the entries addPollFds() appended, after poll() returned.
    void handle(std::span<const pollfd> fds);

    // The socket systemd passed (socket activation), else a new one bound at path; -1 on error.
    static int listenSocket(const std::filesystem::path& path);
    // The answer to one request line, final empty line included.
    [[nodiscard]] std::string answer(std::string_view line) const;

  private:
    void accept();
    // Reads what the client sent and answers each full line; false when the client is gone or misbehaves.
    bool serve(int fd);

    const IndexDb& m_db;
    int m_listen = -1;
    std::unordered_map<int, std::string> m_clients; // fd -> unanswered partial line
  };

} // namespace andromeda::index
