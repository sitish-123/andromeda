#pragma once

#include "files/file_ops_client.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Node;

// The header jobs pill and its popover (FILES-UI-SPEC §14, mockup §4 .pop/.ring/.prog).
namespace job_pill {
  // "holiday.mov → Camera" for one job; "N items → Camera" for several.
  [[nodiscard]] std::string title(const FileOpsClient::Job& job);
  // "1.2 of 1.9 GB · 15 s left" (bytes known) or "412 of 2 048 files".
  [[nodiscard]] std::string detail(const FileOpsClient::Job& job, double bytesPerSecond);
  [[nodiscard]] float progress(const FileOpsClient::Job& job);

  // The 32 high pill: ring of the combined progress + the job count. `out` gets the node the popover hangs under.
  [[nodiscard]] std::unique_ptr<Node> pill(const std::vector<FileOpsClient::Job>& jobs, float scale,
                                           std::function<void()> onClick, Node** out);
  // The popover content, 224 wide; `height` gets its logical height before scale.
  [[nodiscard]] std::unique_ptr<Node> popover(const std::vector<FileOpsClient::Job>& jobs, float scale,
                                              const std::function<double(const std::string& path)>& rate,
                                              std::function<void(const std::string& path)> cancel);
  [[nodiscard]] float popoverHeight(std::size_t jobs);
  inline constexpr float kPopoverWidth = 224.0F;
} // namespace job_pill
