#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Flex;
class Node;
class ThumbnailService;

// The details pane at the window's right edge (FILES-UI-SPEC §13, mockup §2 .info): preview, name, kind and size,
// properties, tags and actions for the selection; the folder when nothing is selected. Alt+P shows and hides it.
class DetailsPane {
public:
  static constexpr float kPaneWidth = 280.0F; // mockup .info, logical px
  struct Actions {
    std::function<void()> open;
    std::function<void()> openWith;
    std::function<void(float sceneX, float sceneY)> share; // set: a Share button opens the Share rows there
    std::function<void()> changed; // the pane's content changed outside a rebuild: redraw
  };

  DetailsPane(ThumbnailService* thumbnails, Actions actions);
  ~DetailsPane();
  DetailsPane(const DetailsPane&) = delete;
  DetailsPane& operator=(const DetailsPane&) = delete;

  // The pane's root, 280 wide; the content follows update() in place, with no window rebuild.
  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void update(const std::filesystem::path& folder, std::vector<std::filesystem::path> selection);


private:
  // What the pane shows, read off the UI thread: stat, folder count and free space, media header, tags.
  struct Facts;
  void fill();
  void read();

  ThumbnailService* m_thumbnails;
  Actions m_actions;
  Flex* m_root = nullptr;
  float m_scale = 1.0F;
  std::filesystem::path m_folder;
  std::vector<std::filesystem::path> m_selection;
  std::shared_ptr<const Facts> m_facts; // for m_folder and m_selection once read; else the last one shown
  bool m_reading = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
