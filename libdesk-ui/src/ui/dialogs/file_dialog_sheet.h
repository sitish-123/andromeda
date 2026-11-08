#pragma once

#include "ui/dialogs/file_dialog.h"

#include <filesystem>
#include <memory>
#include <optional>

class AnimationManager;
class Button;
class DirectoryWatcher;
class FileBrowser;
class Input;
class InputArea;
class Label;
class Node;
class Renderer;
class ThumbnailService;
struct KeyboardEvent;

class FileDialogHost {
public:
  virtual ~FileDialogHost() = default;

  virtual void requestUpdateOnly() = 0;
  virtual void requestLayout() = 0;
  virtual void requestRedraw() = 0;
  virtual void focusArea(InputArea* area) = 0;
  [[nodiscard]] virtual InputArea* focusedArea() const = 0;
  virtual void accept(std::optional<std::filesystem::path> result) = 0;
  virtual void cancel() = 0;
};

// The shell's Open / Save / Select Folder sheet: a FileBrowser (sidebar, views, keys) with a header and a footer.
class FileDialogSheet {
public:
  explicit FileDialogSheet(ThumbnailService* thumbnails);
  ~FileDialogSheet();

  void setHost(FileDialogHost* host) noexcept { m_host = host; }
  void setAnimationManager(AnimationManager* animations) noexcept { m_animations = animations; }
  void setContentScale(float scale) noexcept { m_scale = scale; }
  [[nodiscard]] float contentScale() const noexcept { return m_scale; }
  [[nodiscard]] Node* root() const noexcept { return m_root ? m_root.get() : m_rootPtr; }

  void create();
  void onOpen(std::string_view context);
  void onClose();
  void layout(Renderer& renderer, float width, float height);
  void update(Renderer& renderer);
  [[nodiscard]] std::unique_ptr<Node> releaseRoot();
  [[nodiscard]] bool handleKey(const KeyboardEvent& event);

  [[nodiscard]] float preferredWidth() const { return 880.0F * m_scale; }
  [[nodiscard]] float preferredHeight() const { return 560.0F * m_scale; }
  [[nodiscard]] InputArea* initialFocusArea() const;

  // The watcher the shell polls for every file dialog (one inotify fd).
  [[nodiscard]] static DirectoryWatcher& watcher();

private:
  void submit();
  void openFile(const std::filesystem::path& path);
  void updateControls();
  [[nodiscard]] bool nameFocused() const;

  ThumbnailService* m_thumbnails;
  FileDialogHost* m_host = nullptr;
  AnimationManager* m_animations = nullptr;
  float m_scale = 1.0F;
  FileDialogOptions m_options;
  std::unique_ptr<FileBrowser> m_browser;
  std::unique_ptr<Node> m_root;
  Node* m_rootPtr = nullptr;
  Label* m_title = nullptr;
  Input* m_name = nullptr;
  Button* m_ok = nullptr;
  std::array<Button*, 2> m_nav{};
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
