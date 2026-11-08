#pragma once

#include "core/files/file_info.h"

#include "render/scene/node.h"

#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <functional>
#include <string>
#include <vector>

class Box;
class Image;
class Glyph;
class Label;
class Renderer;
class ThumbnailService;

// The drawn icon of one file or folder (DESIGN.md "Files v2 icons and previews", mockup sections 0-1): full-colour
// folders that show up to 3 of their items, an emblem on the user folders, and a page per document type with its
// coloured badge. Square, `size` logical px (64 in the icons view, 16 to 20 in lists).
class FileIcon final : public Node {
public:
  // What a folder shows inside it, newest first.
  enum class Peek : std::uint8_t { Page, Sheet, Pdf, Image };

  explicit FileIcon(float size);
  ~FileIcon() override;
  // `thumbnails` (may be null) supplies image and video previews; the drawn art shows until one is ready.
  void bind(Renderer& renderer, const FileInfo& info, const std::filesystem::path& path, ThumbnailService* thumbnails);

  // `changed` runs on the main thread when a folder's peek finishes reading; remove it before its owner dies.
  static int addPeekListener(std::function<void()> changed);
  static void removePeekListener(int id);

protected:
  void doLayout(Renderer& renderer) override;

private:
  void bindFolder(Renderer& renderer, const std::filesystem::path& path, std::int64_t mtimeNs);
  void bindPage(Renderer& renderer, const FileInfo& info);
  void bindArt(Renderer& renderer, const char* asset, float fraction);
  void bindVideoCard();
  void layoutPlay(float d);
  void hideAll();
  void releaseThumb();
  bool bindThumb(Renderer& renderer, const std::filesystem::path& path, ThumbnailService* thumbnails, bool video);

  float m_size;
  Image* m_back = nullptr;   // folder rear, or the whole art for single-picture kinds
  std::vector<Box*> m_minis; // up to 3 items inside a folder
  std::vector<std::array<Box*, 6>> m_miniLines; // per mini: a coloured head (or sheet header), then 5 text lines
  Image* m_front = nullptr;  // folder front flap
  Image* m_emblem = nullptr; // user folder emblem
  Box* m_page = nullptr;
  std::vector<Box*> m_lines;
  std::vector<Box*> m_art; // per-kind page art: chart bars, sheet grid, zipper, slide bars
  Box* m_badge = nullptr;
  Label* m_badgeText = nullptr;
  Image* m_thumb = nullptr;
  Box* m_play = nullptr;
  Glyph* m_playGlyph = nullptr;
  ThumbnailService* m_thumbs = nullptr;
  std::string m_thumbPath; // acquired from m_thumbs; released on rebind
  int m_thumbPx = 0;
  // The last bind, so a move to an output with another scale draws the art again at the new pixel size.
  std::optional<FileInfo> m_boundInfo;
  std::filesystem::path m_boundPath;
  ThumbnailService* m_boundThumbs = nullptr;
  float m_boundScale = 0.0F;
};

// One place icon from the Files art set (sidebar rows, tabs, This PC): the texture loads at the first layout, so a
// row can be built without a renderer.
class PlaceIcon final : public Node {
public:
  PlaceIcon(std::string asset, float size);

protected:
  void doLayout(Renderer& renderer) override;

private:
  std::string m_asset;
  float m_size;
  Image* m_image = nullptr;
  int m_loadedPx = 0; // texture size in buffer px; reloads when the output scale changes
};
