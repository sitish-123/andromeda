#pragma once

#include "core/files/file_model.h"
#include "ui/files/file_cells.h"

#include <filesystem>
#include <functional>

class ThumbnailService;
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AnimationManager;
class DirectoryWatcher;
class Flex;
class Label;
class Glyph;
class Node;
class Renderer;
class ScrollView;
class VirtualGridView;
class ColumnAdapter;

// Columns view (mockup §3, FILES-UX D7/M37): one column per folder from the root to the current folder, then a
// trailing column with the contents of the one selected folder or the preview of the one selected file. The current
// folder's column shows the browser's rows and selection; the other columns own their models. Columns scroll, never
// slide.
class FileColumns {
public:
  struct Host {
    // The current folder's rows, cell state and pointer input belong to the browser.
    std::function<const std::vector<std::shared_ptr<const FileInfo>>&()> rows;
    std::function<FileCellState(const FileInfo&, std::size_t)> state;
    std::function<void(std::size_t)> press;
    std::function<void(std::size_t)> activate;
    // A press in another column: go to `dir` and select `name` (empty = nothing).
    std::function<void(const std::filesystem::path& dir, const std::string& name)> goSelect;
    std::function<void()> invalidate;
    ThumbnailService* thumbnails = nullptr; // ancestor columns show thumbnails like the current one
  };

  FileColumns(DirectoryWatcher& watcher, AnimationManager& animations, Host host);
  ~FileColumns();
  FileColumns(const FileColumns&) = delete;
  FileColumns& operator=(const FileColumns&) = delete;

  [[nodiscard]] std::unique_ptr<Node> build(float scale);
  // The chain root..location and the browser's single selected row (nullptr = none or several).
  void update(const std::filesystem::path& location, const FileInfo* selected, const FileSort& sort, bool showHidden);
  // The browser's rows or selection changed in place.
  void refreshCurrent();
  // The renderer of the latest layout pass, for the pooled cells.
  void setRenderer(Renderer* renderer);
  void afterLayout(); // runs a pending scroll to the newest column, once the row has its width
  // The grids measure 0 tall and the horizontal scroll sizes the row by measure: the row fills the stage height.
  void setViewportHeight(float height);
  [[nodiscard]] VirtualGridView* currentGrid() const noexcept;
  // E2E aiming (DESK_FILES_GEOMETRY): "column-item <name> x y" per visible row of every column.
  void appendGeometry(std::string& out) const;

private:
  struct Column;
  void rebuildColumns();
  void layoutColumns(float height);
  void scrollToEnd();
  void showPreview(const FileInfo* file);
  void openPreview();
  [[nodiscard]] std::unique_ptr<Column> makeColumn(const std::filesystem::path& dir, bool current);

  DirectoryWatcher& m_watcher;
  AnimationManager& m_animations;
  Host m_host;
  float m_scale = 1.0F;
  FileSort m_sort;
  bool m_showHidden = false;

  std::filesystem::path m_location;
  std::vector<std::unique_ptr<Column>> m_columns; // root..location, then the trailing folder column if any
  std::optional<std::filesystem::path> m_trailingDir;
  std::string m_previewFor; // the file the preview column shows

  ScrollView* m_scroll = nullptr;
  Flex* m_row = nullptr;
  Flex* m_preview = nullptr;
  Flex* m_previewArt = nullptr;
  Label* m_previewName = nullptr;
  Label* m_previewMeta = nullptr;
  std::uint32_t m_scrollAnim = 0;
  bool m_scrollPending = false;
  Renderer* m_renderer = nullptr;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
