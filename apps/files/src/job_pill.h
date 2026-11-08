#pragma once

#include "files/file_ops_client.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class CountdownRing;
class Label;
class ProgressBar;
class Node;

// The header jobs pill and its popover (FILES-UI-SPEC §14, mockup §4 .pop/.ring/.prog).
namespace job_pill {
  // "holiday.mov → Camera" for one job; "N items → Camera" for several.
  [[nodiscard]] std::string title(const FileOpsClient::Job& job);
  // "1.2 of 1.9 GB · 15 s left" (bytes known) or "412 of 2 048 files".
  [[nodiscard]] std::string detail(const FileOpsClient::Job& job, double bytesPerSecond);
  [[nodiscard]] float progress(const FileOpsClient::Job& job);

  // The combined progress of these jobs, 0..1.
  [[nodiscard]] float combined(const std::vector<FileOpsClient::Job>& jobs);
  // The 32 high pill: ring of the combined progress + the job count. `out` gets the node the popover hangs under,
  // `ring` the accent ring, so progress updates move it in place.
  [[nodiscard]] std::unique_ptr<Node> pill(const std::vector<FileOpsClient::Job>& jobs, float scale,
                                           std::function<void()> onClick, Node** out, CountdownRing** ring);
  // The nodes of one popover row that progress updates change in place.
  struct RowNodes {
    std::string path;
    CountdownRing* ring = nullptr;
    Label* title = nullptr;
    ProgressBar* bar = nullptr;
    Label* detail = nullptr;
  };
  // The job set and each row's circles: the popover is rebuilt only when this changes.
  [[nodiscard]] std::string layoutKey(const std::vector<FileOpsClient::Job>& jobs);
  // The popover content, 224 wide. A copy or move row has pause (Running) or resume (Paused), every row cancel;
  // `pause(path, true)` pauses. `rows` gets the nodes update() moves.
  [[nodiscard]] std::unique_ptr<Node> popover(const std::vector<FileOpsClient::Job>& jobs, float scale,
                                              const std::function<double(const std::string& path)>& rate,
                                              std::function<void(const std::string& path)> cancel,
                                              std::function<void(const std::string& path, bool pause)> pause,
                                              std::vector<RowNodes>* rows);
  void update(const std::vector<RowNodes>& rows, const std::vector<FileOpsClient::Job>& jobs,
              const std::function<double(const std::string& path)>& rate);
  [[nodiscard]] float popoverHeight(std::size_t jobs);
  inline constexpr float kPopoverWidth = 224.0F;
} // namespace job_pill
