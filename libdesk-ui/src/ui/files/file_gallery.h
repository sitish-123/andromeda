#pragma once

#include "render/scene/node.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class Box;
class Flex;
class Image;
class InputArea;
class Label;
class ThumbnailService;

// The immersive Gallery view (DESIGN.md "Files v2 immersive Gallery", mockup section 15): the current photo on an
// ambient wash of its own colours, a floating title capsule, edge arrows and a filmstrip. Follows the light/dark scheme.
class FileGallery final : public Node {
public:
  struct Host {
    std::function<void()> invalidate;
    std::function<void(std::size_t index)> select; // a filmstrip item or an arrow picked another photo
  };

  FileGallery(float scale, ThumbnailService* thumbnails, Host host);
  ~FileGallery() override;
  FileGallery(const FileGallery&) = delete;
  FileGallery& operator=(const FileGallery&) = delete;

  // Images and videos of the folder, in view order; `current` indexes them.
  void setItems(std::vector<std::filesystem::path> items, std::size_t current);
  [[nodiscard]] std::size_t current() const noexcept { return m_current; }
  [[nodiscard]] std::size_t count() const noexcept { return m_items.size(); }

protected:
  void doLayout(Renderer& renderer) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  struct Held {
    std::string path;
    int px = 0;
  };
  void hold(const std::string& path, int px, std::vector<Held>& next);
  void releaseAll(std::vector<Held>& held);
  void rebuildFilm();

  float m_scale;
  ThumbnailService* m_thumbs;
  Host m_host;
  std::vector<std::filesystem::path> m_items;
  std::size_t m_current = 0;
  std::vector<Held> m_held; // acquired from m_thumbs, released on the next layout or at the end

  Box* m_bg = nullptr;
  Image* m_ambient = nullptr;
  Image* m_photo = nullptr;
  Flex* m_title = nullptr;
  Label* m_name = nullptr;
  Label* m_position = nullptr;
  Flex* m_prev = nullptr;
  Flex* m_next = nullptr;
  Flex* m_film = nullptr;
  std::vector<Image*> m_filmImages;
  std::vector<std::size_t> m_filmIndex;
  Box* m_filmRing = nullptr;
  bool m_filmDirty = true;
};
