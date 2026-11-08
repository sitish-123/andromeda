#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Node;

// This PC (mockup section 0, Explorer "This PC"): the user folders with a picture of what is inside, then the drives
// with usage bars (danger when 90 % full) and the host's extra places (Trash, Network) as drive cells.
struct ThisPcPlace {
  std::string label;
  std::filesystem::path path; // empty for a host place
  std::string art;            // files-icons/<art>.svg for drive cells
  std::string caption;        // drive cells: the line under the name
  bool warn = false;          // the caption in danger text
};

struct ThisPcOptions {
  std::vector<ThisPcPlace> folders;
  std::vector<ThisPcPlace> extra; // after the drives
  std::function<void(const ThisPcPlace&)> open;
};

[[nodiscard]] std::unique_ptr<Node> buildThisPc(float scale, ThisPcOptions options);

// Pages in the This PC look (Network, phone): sections of drive cells under a heading. `action` (optional) is a
// capsule button right of the first heading, e.g. "Connect to Server…". A place with `warn` set shows its caption in
// danger text (a server that does not answer).
struct PlaceSection {
  std::string title;
  std::string caption; // right of the title in label_3, e.g. "Looking…"
  std::vector<ThisPcPlace> places;
};
struct PlaceAction {
  std::string label;
  std::function<void()> run;
};
[[nodiscard]] std::unique_ptr<Node> buildPlaceSections(float scale, std::vector<PlaceSection> sections,
                                                       std::function<void(const ThisPcPlace&)> open,
                                                       std::optional<PlaceAction> action = std::nullopt);

