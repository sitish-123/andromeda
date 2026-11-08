#pragma once

#include "core/poll_source.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct FileInfo;

// Search This PC (FILES-UI-SPEC §15): name and content queries to the andromeda-index daemon over its socket
// ($XDG_RUNTIME_DIR/andromeda-index.sock, protocol in indexer/src/query_server.h). Answers come in request order;
// each reaches `answer` with the text it was asked for.
class IndexSearch final : public PollSource {
public:
  using Answer = std::function<void(const std::string& text, std::vector<std::shared_ptr<const FileInfo>> rows)>;

  explicit IndexSearch(Answer answer);
  ~IndexSearch() override;
  IndexSearch(const IndexSearch&) = delete;
  IndexSearch& operator=(const IndexSearch&) = delete;

  [[nodiscard]] static bool available(); // the daemon's socket exists
  void query(const std::string& text, std::string_view kind);

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  bool connect();
  void disconnect();
  void flush();

  Answer m_answer;
  int m_fd = -1;
  std::deque<std::string> m_asked; // texts of the unanswered requests, oldest first
  std::string m_out;
  std::string m_in;
  std::vector<std::shared_ptr<const FileInfo>> m_rows; // the answer being read
};
