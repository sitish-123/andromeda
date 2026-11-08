#pragma once

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>
#include <vterm.h>

class Flex;
class InputArea;
class Node;
class Renderer;

// Dolphin-style terminal pane (F4, mockup files-v2-live.html §16, FILES-UI-SPEC §18): the user's shell on a pty under
// the view. libvterm parses the output; the cells draw as label runs. The shell follows the folder (a " cd" while no
// job runs) and the folder follows the shell's cd (its /proc cwd).
class TerminalPane {
public:
  struct Host {
    std::function<void()> invalidate; // cells changed in place
    std::function<void(const std::filesystem::path&)> cwdChanged;
    std::function<void()> exited; // the shell ended (exit, Ctrl+D)
    std::function<void(InputArea*)> focus;
  };

  TerminalPane(std::filesystem::path dir, Host host);
  ~TerminalPane();
  TerminalPane(const TerminalPane&) = delete;
  TerminalPane& operator=(const TerminalPane&) = delete;

  [[nodiscard]] bool running() const noexcept { return m_pid > 0; }
  [[nodiscard]] std::unique_ptr<Node> build(Renderer& renderer, float scale, float width);
  void focus();
  // The pane left the scene (F4 hid it): its input area is gone.
  void detach();
  [[nodiscard]] bool focused() const;
  // The view moved to `dir`: cd there unless a job runs in the shell or it is there already.
  void followFolder(const std::filesystem::path& dir);
  void sendText(const std::string& text);
  [[nodiscard]] std::string screenText() const;

  static constexpr float kHeight = 216.0F; // 10 rows of 20 + padding 8
  static constexpr int kRows = 10;

private:
  bool spawn(const std::filesystem::path& dir);
  void onReadable();
  void refresh();
  void scheduleRefresh();
  void resize(int cols);
  void key(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers);
  void scroll(float delta);
  [[nodiscard]] std::unique_ptr<Node> buildRow(int row);
  [[nodiscard]] std::filesystem::path shellCwd() const;
  void hangUp();

  static int onDamage(int startRow, int endRow, int startCol, int endCol, void* self);
  static int onMoveCursor(int row, int col, int oldRow, int oldCol, int visible, void* self);
  static int onPushLine(int cols, const VTermScreenCell* cells, void* self);
  static int onPopLine(int cols, VTermScreenCell* cells, void* self);
  static void onOutput(const char* bytes, std::size_t len, void* self);

  Host m_host;
  pid_t m_pid = -1;
  int m_master = -1;
  unsigned m_watch = 0;
  VTerm* m_vt = nullptr;
  VTermScreen* m_screen = nullptr;
  int m_cols = 80;
  int m_cursorRow = 0;
  int m_cursorCol = 0;
  bool m_cursorVisible = true;
  std::deque<std::vector<VTermScreenCell>> m_scrollback; // oldest first
  int m_scrollOffset = 0;                                 // lines scrolled back into m_scrollback
  bool m_refreshQueued = false;
  std::filesystem::path m_cwd;
  float m_scale = 1.0F;
  float m_cellWidth = 7.0F; // logical px of one monospace cell
  Flex* m_rows = nullptr;
  InputArea* m_area = nullptr; // m_rows and m_area are null while the pane is hidden: its nodes are gone
  bool m_focusWanted = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
