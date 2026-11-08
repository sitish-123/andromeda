#include "terminal_pane.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"

#include <algorithm>
#include <glib.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glib-unix.h>
#include <pty.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vterm.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace fs = std::filesystem;

namespace {
  constexpr Logger kLog("terminal");
  constexpr std::uint32_t kForeground = 0xe6e6ea; // mockup .term
  constexpr std::uint32_t kBackground = 0x0f0f14;
  constexpr float kFont = 12.0F;
  constexpr float kLine = 20.0F;
  constexpr float kMargin = 8.0F;
  constexpr float kPadV = 8.0F;
  constexpr float kPadH = 12.0F;
  constexpr std::size_t kScrollback = 2000;
  constexpr std::size_t kReadChunk = 16 * 1024;

  ColorSpec fixed(std::uint32_t rgb) { return ColorSpec{.role = std::nullopt, .fixed = rgbHex(rgb), .alpha = 1.0F}; }

  std::string shellPath() {
    if (const char* env = std::getenv("SHELL"); env != nullptr && *env == '/' && ::access(env, X_OK) == 0) {
      return env;
    }
    if (const passwd* pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_shell != nullptr && *pw->pw_shell == '/') {
      return pw->pw_shell;
    }
    return "/bin/sh";
  }

  std::string quoted(const std::string& path) {
    std::string out = "'";
    for (const char c : path) {
      out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
  }

  void appendUtf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xc0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xe0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (c & 0x3f));
    }
  }
} // namespace

TerminalPane::TerminalPane(fs::path dir, Host host) : m_host(std::move(host)), m_cwd(dir) {
  m_vt = vterm_new(kRows, m_cols);
  vterm_set_utf8(m_vt, 1);
  vterm_output_set_callback(m_vt, &TerminalPane::onOutput, this);
  m_screen = vterm_obtain_screen(m_vt);
  static const VTermScreenCallbacks callbacks{
      .damage = [](VTermRect r, void* self) { return onDamage(r.start_row, r.end_row, r.start_col, r.end_col, self); },
      .moverect = nullptr,
      .movecursor = [](VTermPos pos, VTermPos old, int visible, void* self) {
        return onMoveCursor(pos.row, pos.col, old.row, old.col, visible, self);
      },
      .settermprop = nullptr,
      .bell = nullptr,
      .resize = nullptr,
      .sb_pushline = &TerminalPane::onPushLine,
      .sb_popline = &TerminalPane::onPopLine,
      .sb_clear = nullptr,
  };
  vterm_screen_set_callbacks(m_screen, &callbacks, this);
  vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
  vterm_screen_enable_altscreen(m_screen, 1);
  vterm_screen_reset(m_screen, 1);
  spawn(dir);
}

TerminalPane::~TerminalPane() {
  hangUp();
  vterm_free(m_vt);
}

void TerminalPane::hangUp() {
  if (m_watch != 0) {
    g_source_remove(m_watch);
    m_watch = 0;
  }
  if (m_master >= 0) {
    ::close(m_master);
    m_master = -1;
  }
  if (m_pid > 0) {
    ::kill(m_pid, SIGHUP);
    // The shell exits on the hang-up; the reap must not block the UI on one that ignores it.
    if (::waitpid(m_pid, nullptr, WNOHANG) == 0) {
      g_child_watch_add(m_pid, [](GPid pid, gint, gpointer) { g_spawn_close_pid(pid); }, nullptr);
    }
    m_pid = -1;
  }
}

bool TerminalPane::spawn(const fs::path& dir) {
  winsize size{.ws_row = static_cast<unsigned short>(kRows), .ws_col = static_cast<unsigned short>(m_cols),
               .ws_xpixel = 0, .ws_ypixel = 0};
  int master = -1;
  const std::string shell = shellPath();
  const pid_t pid = ::forkpty(&master, nullptr, nullptr, &size);
  if (pid < 0) {
    kLog.warn("forkpty: {}", std::strerror(errno));
    return false;
  }
  if (pid == 0) {
    if (::chdir(dir.c_str()) != 0) {
      (void)::chdir("/");
    }
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("COLORTERM", "truecolor", 1);
    ::unsetenv("COLUMNS");
    ::unsetenv("LINES");
    const std::string name = fs::path(shell).filename().string();
    ::execl(shell.c_str(), name.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  m_pid = pid;
  m_master = master;
  ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL) | O_NONBLOCK);
  ::fcntl(m_master, F_SETFD, FD_CLOEXEC);
  m_watch = g_unix_fd_add(
      m_master, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
      [](gint, GIOCondition, gpointer self) -> gboolean {
        auto* pane = static_cast<TerminalPane*>(self);
        pane->onReadable();
        return pane->m_master >= 0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
      },
      this);
  kLog.info("terminal started {} in {}", shell, dir.string());
  return true;
}

void TerminalPane::onReadable() {
  char buf[kReadChunk];
  for (;;) {
    const ssize_t n = ::read(m_master, buf, sizeof buf);
    if (n > 0) {
      vterm_input_write(m_vt, buf, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
      break;
    }
    // EOF or EIO: the shell is gone.
    kLog.info("terminal shell exited");
    m_watch = 0; // the callback returns G_SOURCE_REMOVE
    ::close(m_master);
    m_master = -1;
    ::waitpid(m_pid, nullptr, WNOHANG);
    m_pid = -1;
    DeferredCall::callLater([alive = std::weak_ptr<bool>(m_alive), exited = m_host.exited] {
      if (!alive.expired() && exited) {
        exited();
      }
    });
    return;
  }
  const fs::path cwd = shellCwd();
  if (!cwd.empty() && cwd != m_cwd) {
    m_cwd = cwd;
    kLog.info("terminal cwd {}", cwd.string());
    if (m_host.cwdChanged) {
      m_host.cwdChanged(cwd);
    }
  }
}

fs::path TerminalPane::shellCwd() const {
  if (m_pid <= 0) {
    return {};
  }
  std::error_code ec;
  fs::path cwd = fs::read_symlink(std::format("/proc/{}/cwd", m_pid), ec);
  return ec ? fs::path() : cwd;
}

void TerminalPane::followFolder(const fs::path& dir) {
  if (m_master < 0 || dir.empty() || dir == m_cwd || !dir.is_absolute()) {
    return;
  }
  // A running job (vim, a build) owns the terminal: typing a cd into it would do harm.
  if (::tcgetpgrp(m_master) != m_pid) {
    kLog.debug("terminal busy, no cd to {}", dir.string());
    return;
  }
  // The path is typed into the shell: a control byte in a folder name (Ctrl+U, Return) would edit the line and run
  // what follows it, and invalid UTF-8 can carry C1 controls. Such a folder is not followed.
  const std::string text = dir.string();
  const bool control = std::ranges::any_of(text, [](char c) {
    const auto b = static_cast<unsigned char>(c);
    return b < 0x20 || b == 0x7f;
  });
  if (control || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    kLog.warn("terminal does not follow a folder whose name has control characters");
    return;
  }
  m_cwd = dir;
  // Ctrl+E Ctrl+U clears a half-typed line; the leading space keeps the cd out of the history (ignorespace).
  sendText("\x05\x15 cd -- " + quoted(dir.string()) + "\r");
  kLog.info("terminal follows {}", dir.string());
}

void TerminalPane::sendText(const std::string& text) {
  if (m_master < 0) {
    return;
  }
  std::size_t done = 0;
  while (done < text.size()) {
    const ssize_t n = ::write(m_master, text.data() + done, text.size() - done);
    if (n < 0 && errno != EINTR && errno != EAGAIN) {
      return;
    }
    done += n > 0 ? static_cast<std::size_t>(n) : 0;
  }
}

void TerminalPane::onOutput(const char* bytes, std::size_t len, void* self) {
  static_cast<TerminalPane*>(self)->sendText(std::string(bytes, len));
}

int TerminalPane::onDamage(int, int, int, int, void* self) {
  static_cast<TerminalPane*>(self)->scheduleRefresh();
  return 1;
}

int TerminalPane::onMoveCursor(int row, int col, int, int, int visible, void* self) {
  auto* pane = static_cast<TerminalPane*>(self);
  pane->m_cursorRow = row;
  pane->m_cursorCol = col;
  pane->m_cursorVisible = visible != 0;
  pane->scheduleRefresh();
  return 1;
}

int TerminalPane::onPushLine(int cols, const VTermScreenCell* cells, void* self) {
  auto* pane = static_cast<TerminalPane*>(self);
  pane->m_scrollback.emplace_back(cells, cells + cols);
  if (pane->m_scrollback.size() > kScrollback) {
    pane->m_scrollback.pop_front();
  }
  if (pane->m_scrollOffset > 0) {
    pane->m_scrollOffset = std::min<int>(pane->m_scrollOffset + 1, static_cast<int>(pane->m_scrollback.size()));
  }
  return 1;
}

int TerminalPane::onPopLine(int cols, VTermScreenCell* cells, void* self) {
  auto* pane = static_cast<TerminalPane*>(self);
  if (pane->m_scrollback.empty()) {
    return 0;
  }
  const auto& line = pane->m_scrollback.back();
  for (int i = 0; i < cols; ++i) {
    if (static_cast<std::size_t>(i) < line.size()) {
      cells[i] = line[static_cast<std::size_t>(i)];
    } else {
      cells[i] = VTermScreenCell{};
      cells[i].width = 1;
    }
  }
  pane->m_scrollback.pop_back();
  pane->m_scrollOffset = std::min<int>(pane->m_scrollOffset, static_cast<int>(pane->m_scrollback.size()));
  return 1;
}

void TerminalPane::scheduleRefresh() {
  if (m_refreshQueued) {
    return;
  }
  m_refreshQueued = true;
  DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
    if (!alive.expired()) {
      m_refreshQueued = false;
      refresh();
    }
  });
}

void TerminalPane::resize(int cols) {
  cols = std::max(cols, 20);
  if (cols == m_cols) {
    return;
  }
  m_cols = cols;
  vterm_set_size(m_vt, kRows, m_cols);
  if (m_master >= 0) {
    winsize size{.ws_row = static_cast<unsigned short>(kRows), .ws_col = static_cast<unsigned short>(m_cols),
                 .ws_xpixel = 0, .ws_ypixel = 0};
    ::ioctl(m_master, TIOCSWINSZ, &size);
  }
}

std::unique_ptr<Node> TerminalPane::buildRow(int row) {
  const float s = m_scale;
  auto line = ui::row({.align = FlexAlign::Center, .clipChildren = true, .height = kLine * s});
  // Row 0 is the top of the view; scrolled back, the first rows come from the scrollback.
  const int fromBack = m_scrollOffset - row;
  const std::vector<VTermScreenCell>* saved =
      fromBack > 0 ? &m_scrollback[m_scrollback.size() - static_cast<std::size_t>(fromBack)] : nullptr;
  const int screenRow = row - m_scrollOffset;

  struct Run {
    std::string text;
    std::uint32_t fg = kForeground;
    std::uint32_t bg = kBackground;
    bool bold = false;
    int cells = 0;
  };
  std::vector<Run> runs;
  for (int col = 0; col < m_cols;) {
    VTermScreenCell cell{};
    if (saved != nullptr) {
      if (static_cast<std::size_t>(col) < saved->size()) {
        cell = (*saved)[static_cast<std::size_t>(col)];
      }
    } else {
      vterm_screen_get_cell(m_screen, VTermPos{.row = screenRow, .col = col}, &cell);
    }
    const int width = std::max<int>(cell.width, 1);
    std::uint32_t fg = kForeground;
    std::uint32_t bg = kBackground;
    if (!VTERM_COLOR_IS_DEFAULT_FG(&cell.fg)) {
      vterm_screen_convert_color_to_rgb(m_screen, &cell.fg);
      fg = (std::uint32_t{cell.fg.rgb.red} << 16) | (std::uint32_t{cell.fg.rgb.green} << 8) | cell.fg.rgb.blue;
    }
    if (!VTERM_COLOR_IS_DEFAULT_BG(&cell.bg)) {
      vterm_screen_convert_color_to_rgb(m_screen, &cell.bg);
      bg = (std::uint32_t{cell.bg.rgb.red} << 16) | (std::uint32_t{cell.bg.rgb.green} << 8) | cell.bg.rgb.blue;
    }
    const bool cursor = saved == nullptr && m_cursorVisible && screenRow == m_cursorRow && col == m_cursorCol && focused();
    if ((cell.attrs.reverse != 0) != cursor) {
      std::swap(fg, bg);
    }
    std::string text;
    if (cell.chars[0] == 0) {
      text = " ";
    } else {
      for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; ++i) {
        appendUtf8(text, cell.chars[i]);
      }
    }
    const bool bold = cell.attrs.bold != 0;
    if (runs.empty() || runs.back().fg != fg || runs.back().bg != bg || runs.back().bold != bold) {
      runs.push_back({.text = {}, .fg = fg, .bg = bg, .bold = bold, .cells = 0});
    }
    runs.back().text += text;
    runs.back().cells += width;
    col += width;
  }
  // Trailing blanks on the default background draw nothing.
  while (!runs.empty() && runs.back().bg == kBackground &&
         runs.back().text.find_first_not_of(' ') == std::string::npos) {
    runs.pop_back();
  }
  for (Run& run : runs) {
    // Each run sits in a box of its exact cell width, so a fallback glyph cannot shift the columns after it.
    auto box = ui::row({.align = FlexAlign::Center,
                        .fill = run.bg == kBackground ? clearColorSpec() : fixed(run.bg), .clipChildren = true,
                        .width = static_cast<float>(run.cells) * m_cellWidth * s, .height = kLine * s});
    box->addChild(ui::label({.text = std::move(run.text), .fontSize = kFont * s,
                             .fontWeight = run.bold ? FontWeight::SemiBold : FontWeight::Normal,
                             .fontFamily = std::string("monospace"), .color = fixed(run.fg), .maxLines = 1}));
    line->addChild(std::move(box));
  }
  return line;
}

void TerminalPane::refresh() {
  if (m_rows == nullptr) {
    return;
  }
  while (!m_rows->children().empty()) {
    (void)m_rows->removeChild(m_rows->children().back().get());
  }
  for (int row = 0; row < kRows; ++row) {
    m_rows->addChild(buildRow(row));
  }
  if (m_host.invalidate) {
    m_host.invalidate();
  }
}

std::unique_ptr<Node> TerminalPane::build(Renderer& renderer, float scale, float width) {
  m_scale = scale;
  const float s = scale;
  const TextFont mono{.weight = FontWeight::Normal, .family = "monospace"};
  // 10 cells measured together: one cell's advance alone rounds.
  const float measured = renderer.measureText("MMMMMMMMMM", kFont * s, mono).width / 10.0F / s;
  if (measured > 1.0F) {
    m_cellWidth = measured;
  }
  const float inner = width / s - 2.0F * kMargin - 2.0F * kPadH;
  resize(static_cast<int>(inner / m_cellWidth));

  auto frame = ui::column({.align = FlexAlign::Stretch, .paddingV = kPadV * s, .paddingH = kPadH * s,
                           .fill = fixed(kBackground), .radius = 8.0F * s, .clipChildren = true,
                           .width = width - 2.0F * kMargin * s, .height = (kHeight - 2.0F * kMargin) * s});
  m_rows = nullptr;
  frame->addChild(ui::column({.out = &m_rows, .align = FlexAlign::Stretch}));
  refresh();
  auto area = ui::inputArea(
      {.out = &m_area, .focusable = true, .width = width - 2.0F * kMargin * s, .height = (kHeight - 2.0F * kMargin) * s,
       .onClick = [this](const InputArea::PointerData&) { focus(); },
       .configure =
           [this](InputArea& a) {
             a.setCapturesAllKeys(true);
             a.setOnKeyDown([this](const InputArea::KeyData& k) { key(k.sym, k.utf32, k.modifiers); });
             a.setOnFocusGain([this] { scheduleRefresh(); });
             a.setOnFocusLoss([this] { scheduleRefresh(); });
             a.setOnAxis([this](const InputArea::PointerData& p) {
               scroll(static_cast<float>(p.axisValue));
             });
           }},
      std::move(frame));
  auto outer = ui::column({.align = FlexAlign::Stretch, .padding = kMargin * s, .height = kHeight * s});
  outer->addChild(std::move(area));
  return outer;
}

void TerminalPane::focus() {
  if (m_area != nullptr && m_host.focus) {
    m_host.focus(m_area);
  }
}

bool TerminalPane::focused() const { return m_area != nullptr && m_area->focused(); }

void TerminalPane::scroll(float delta) {
  const int lines = static_cast<int>(delta / 10.0F) != 0 ? static_cast<int>(delta / 10.0F) : (delta > 0 ? 1 : -1);
  const int next = std::clamp(m_scrollOffset - lines, 0, static_cast<int>(m_scrollback.size()));
  if (next != m_scrollOffset) {
    m_scrollOffset = next;
    scheduleRefresh();
  }
}

void TerminalPane::key(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers) {
  int mod = VTERM_MOD_NONE;
  mod |= (modifiers & KeyMod::Shift) != 0 ? VTERM_MOD_SHIFT : 0;
  mod |= (modifiers & KeyMod::Ctrl) != 0 ? VTERM_MOD_CTRL : 0;
  mod |= (modifiers & KeyMod::Alt) != 0 ? VTERM_MOD_ALT : 0;
  const auto vmod = static_cast<VTermModifier>(mod);
  VTermKey vkey = VTERM_KEY_NONE;
  switch (sym) {
  case XKB_KEY_Return: case XKB_KEY_KP_Enter: vkey = VTERM_KEY_ENTER; break;
  case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: vkey = VTERM_KEY_TAB; break;
  case XKB_KEY_BackSpace: vkey = VTERM_KEY_BACKSPACE; break;
  case XKB_KEY_Escape: vkey = VTERM_KEY_ESCAPE; break;
  case XKB_KEY_Up: vkey = VTERM_KEY_UP; break;
  case XKB_KEY_Down: vkey = VTERM_KEY_DOWN; break;
  case XKB_KEY_Left: vkey = VTERM_KEY_LEFT; break;
  case XKB_KEY_Right: vkey = VTERM_KEY_RIGHT; break;
  case XKB_KEY_Insert: vkey = VTERM_KEY_INS; break;
  case XKB_KEY_Delete: vkey = VTERM_KEY_DEL; break;
  case XKB_KEY_Home: vkey = VTERM_KEY_HOME; break;
  case XKB_KEY_End: vkey = VTERM_KEY_END; break;
  case XKB_KEY_Page_Up: vkey = VTERM_KEY_PAGEUP; break;
  case XKB_KEY_Page_Down: vkey = VTERM_KEY_PAGEDOWN; break;
  default:
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) {
      vkey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(static_cast<int>(sym - XKB_KEY_F1) + 1));
    }
    break;
  }
  if (m_scrollOffset != 0 && (vkey != VTERM_KEY_NONE || utf32 != 0)) {
    m_scrollOffset = 0; // typing jumps back to the prompt
    scheduleRefresh();
  }
  if (vkey != VTERM_KEY_NONE) {
    vterm_keyboard_key(m_vt, vkey, vmod);
    return;
  }
  // Ctrl+letter arrives with a control code or none; libvterm wants the letter with the modifier.
  std::uint32_t c = utf32;
  if ((modifiers & KeyMod::Ctrl) != 0 && sym >= 0x20 && sym < 0x7f) {
    c = sym;
  }
  if (c == 0 || (c < 0x20 && (modifiers & KeyMod::Ctrl) == 0)) {
    return;
  }
  // Shift is already in the character ("A", "!").
  vterm_keyboard_unichar(m_vt, c, static_cast<VTermModifier>(mod & ~VTERM_MOD_SHIFT));
}

std::string TerminalPane::screenText() const {
  std::string out;
  for (int row = 0; row < kRows; ++row) {
    std::string line;
    for (int col = 0; col < m_cols; ++col) {
      VTermScreenCell cell{};
      vterm_screen_get_cell(m_screen, VTermPos{.row = row, .col = col}, &cell);
      if (cell.chars[0] == 0) {
        line += ' ';
      } else {
        appendUtf8(line, cell.chars[0]);
      }
    }
    line.erase(line.find_last_not_of(' ') + 1);
    out += line + '\n';
  }
  return out;
}
