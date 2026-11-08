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
  struct Actions {
    std::function<void()> open;
    std::function<void()> openWith;
  };

  DetailsPane(ThumbnailService* thumbnails, Actions actions);

  // The pane's root, 280 wide; the content follows update() in place, with no window rebuild.
  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  void update(const std::filesystem::path& folder, std::vector<std::filesystem::path> selection);

  // Pixel size from the image header (PNG, JPEG, GIF, WebP), without decoding; nullopt for other files.
  [[nodiscard]] static std::optional<std::pair<std::uint32_t, std::uint32_t>> imageSize(const std::filesystem::path& file);

private:
  void fill();

  ThumbnailService* m_thumbnails;
  Actions m_actions;
  Flex* m_root = nullptr;
  float m_scale = 1.0F;
  std::filesystem::path m_folder;
  std::vector<std::filesystem::path> m_selection;
};
