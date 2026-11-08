#include "preview_window.h"

#include "app/app_loop.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "app/foreign_parent.h"
#include "preview_service.h"
#include "render/core/texture_manager.h"
#include "ui/builders.h"
#include "core/files/file_info.h"
#include "ui/controls/image.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "render/core/render_styles.h"
#include "ui/controls/screen_corner.h"
#include "ui/controls/spinner.h"
#include "ui/controls/virtual_list_view.h"
#include "files/file_ops_client.h"
#include "ui/files/file_format.h"
#include "ui/files/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>
#include <unordered_map>
#include <array>
#include <cctype>
#include <optional>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <span>
#include <string_view>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace preview {

  namespace {
    constexpr Logger kLog("preview");
    constexpr float kHeaderHeight = 40.0F;
    constexpr float kDocumentWidth = 720.0F;
    constexpr float kPageGap = 12.0F;
    constexpr float kStripWidth = 40.0F;      // mockup §15 page strip
    constexpr float kStripPageHeight = 52.0F;
    constexpr std::size_t kPeakCount = 64; // waveform bars
    constexpr float kWaveHeight = 36.0F;   // mockup .wave tallest bar
    constexpr float kCoverSize = 160.0F;
    // Decoded at 2x for HiDPI outputs.
    constexpr std::uint32_t kCoverBox = 320;
    constexpr std::uint32_t kImageBox = 4096;
    constexpr std::uint32_t kMaxPageHeight = 16384;
    constexpr const char* kMonospace = "monospace";

    std::string guessContentType(const std::string& path) {
      GFile* file = g_file_new_for_path(path.c_str());
      GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE,
                                          nullptr, nullptr);
      std::string type;
      if (info != nullptr) {
        if (const char* t = g_file_info_get_content_type(info); t != nullptr) {
          type = t;
        }
        g_object_unref(info);
      }
      g_object_unref(file);
      return type;
    }

    // The app that opens this type, for "Open With <App>"; empty when none is set.
    std::string defaultAppName(const std::string& contentType) {
      if (contentType.empty()) {
        return {};
      }
      GAppInfo* app = g_app_info_get_default_for_type(contentType.c_str(), FALSE);
      if (app == nullptr) {
        return {};
      }
      std::string name = g_app_info_get_display_name(app) != nullptr ? g_app_info_get_display_name(app) : "";
      g_object_unref(app);
      return name;
    }

    const char* kindGlyph(FileKind kind) {
      switch (kind) {
      case FileKind::Image:
        return "photo";
      case FileKind::Pdf:
        return "file-type-pdf";
      case FileKind::Text:
        return "file-code";
      case FileKind::Video:
        return "movie";
      case FileKind::Audio:
        return "music";
      case FileKind::Archive:
        return "file-zip";
      case FileKind::Font:
        return "typography";
      case FileKind::Folder:
        return "folder";
      case FileKind::Office:
        return "file-type-doc";
      default:
        return "file";
      }
    }

    // Box-filters a BGRA page down to the strip's pixel size, so the strip never holds full-size page textures.
    struct Scaled {
      std::vector<std::uint8_t> bytes;
      int width = 0;
      int height = 0;
    };
    Scaled shrinkPage(std::span<const std::uint8_t> src, int width, int height, int stride, int maxHeight) {
      const int step = std::max(1, height / std::max(1, maxHeight));
      Scaled out{.width = std::max(1, width / step), .height = std::max(1, height / step)};
      out.bytes.resize(static_cast<std::size_t>(out.width) * out.height * 4);
      for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
          std::uint32_t sum[4] = {};
          for (int dy = 0; dy < step; ++dy) {
            const std::uint8_t* row = src.data() + static_cast<std::size_t>(y * step + dy) * stride + x * step * 4;
            for (int dx = 0; dx < step; ++dx) {
              for (int c = 0; c < 4; ++c) {
                sum[c] += row[dx * 4 + c];
              }
            }
          }
          std::uint8_t* dst = out.bytes.data() + (static_cast<std::size_t>(y) * out.width + x) * 4;
          for (int c = 0; c < 4; ++c) {
            dst[c] = static_cast<std::uint8_t>(sum[c] / static_cast<std::uint32_t>(step * step));
          }
        }
      }
      return out;
    }

    void setStripPage(Image& image, Renderer& renderer, std::span<const std::uint8_t> src, std::uint32_t width,
                      std::uint32_t height, std::uint32_t stride, float bufferScale) {
      const auto px = static_cast<int>(std::lround(kStripPageHeight * bufferScale));
      const Scaled s = shrinkPage(src, static_cast<int>(width), static_cast<int>(height), static_cast<int>(stride), px);
      image.setSourceRaw(renderer, s.bytes.data(), s.bytes.size(), s.width, s.height, s.width * 4, PixmapFormat::BGRA);
    }

    std::string baseName(const std::string& path) {
      const auto slash = path.rfind('/');
      return slash == std::string::npos ? path : path.substr(slash + 1);
    }
  } // namespace

  namespace {
    // A token scan, not a parser (spec §10): comments, strings and keywords of the common languages by extension.
    struct Syntax {
      std::string_view lineComment; // empty = none
      bool blockComments = false;   // /* … */
    };

    std::optional<Syntax> syntaxFor(const std::string& path) {
      const auto dot = path.rfind('.');
      const std::string ext = dot == std::string::npos ? std::string() : path.substr(dot + 1);
      static constexpr std::array kSlash = {"c", "h", "cc", "cpp", "cxx", "hpp", "hh", "rs", "js", "mjs", "ts",
                                            "tsx", "jsx", "java", "kt", "go", "swift", "cs", "dart", "scala",
                                            "glsl", "frag", "vert", "qml", "css", "scss", "json", "jsonc", "zig"};
      static constexpr std::array kHash = {"py", "sh", "bash", "zsh", "fish", "rb", "pl", "toml", "yaml", "yml",
                                           "conf", "ini", "cmake", "nix", "r", "jl", "build"};
      static constexpr std::array kDash = {"lua", "luau", "sql", "hs"};
      if (std::ranges::find(kSlash, ext) != kSlash.end()) {
        return Syntax{.lineComment = "//", .blockComments = true};
      }
      if (std::ranges::find(kHash, ext) != kHash.end() || path.ends_with("/meson.build")) {
        return Syntax{.lineComment = "#"};
      }
      if (std::ranges::find(kDash, ext) != kDash.end()) {
        return Syntax{.lineComment = "--"};
      }
      return std::nullopt;
    }

    bool isKeyword(std::string_view word) {
      static constexpr std::array<std::string_view, 72> kWords = {
          "auto", "break", "case", "catch", "class", "const", "constexpr", "continue", "def", "default", "delete",
          "do", "elif", "else", "enum", "export", "extern", "false", "fn", "for", "from", "func", "function", "if",
          "impl", "import", "in", "include", "inline", "interface", "let", "local", "loop", "match", "mod", "mut",
          "namespace", "new", "nil", "None", "null", "nullptr", "operator", "override", "package", "private",
          "protected", "pub", "public", "return", "self", "static", "struct", "super", "switch", "template", "then",
          "this", "throw", "trait", "true", "True", "False", "try", "type", "typename", "use", "using", "var",
          "virtual", "void", "while"};
      return std::ranges::find(kWords, word) != kWords.end();
    }

    struct Token {
      std::string_view text;
      ColorRole role;
    };

    // Scans one line; `inBlock` carries an open /* comment from line to line.
    std::vector<Token> scanLine(std::string_view line, const Syntax& syntax, bool& inBlock) {
      std::vector<Token> out;
      std::size_t plainStart = 0;
      std::size_t i = 0;
      const auto flush = [&](std::size_t end) {
        if (end > plainStart) {
          out.push_back({line.substr(plainStart, end - plainStart), ColorRole::Label1});
        }
      };
      const auto emit = [&](std::size_t start, std::size_t end, ColorRole role) {
        flush(start);
        out.push_back({line.substr(start, end - start), role});
        plainStart = end;
        i = end;
      };
      if (inBlock) {
        const auto close = line.find("*/");
        if (close == std::string_view::npos) {
          return {{line, ColorRole::Label3}};
        }
        inBlock = false;
        emit(0, close + 2, ColorRole::Label3);
      }
      while (i < line.size()) {
        const std::string_view rest = line.substr(i);
        const char c = line[i];
        if (!syntax.lineComment.empty() && rest.starts_with(syntax.lineComment)) {
          emit(i, line.size(), ColorRole::Label3);
        } else if (syntax.blockComments && rest.starts_with("/*")) {
          const auto close = line.find("*/", i + 2);
          inBlock = close == std::string_view::npos;
          emit(i, inBlock ? line.size() : close + 2, ColorRole::Label3);
        } else if (c == '"' || c == '\'') {
          std::size_t end = i + 1;
          while (end < line.size() && line[end] != c) {
            end += line[end] == '\\' ? 2 : 1;
          }
          emit(i, std::min(end + 1, line.size()), ColorRole::Success);
        } else if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
          std::size_t end = i;
          while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) != 0 || line[end] == '_')) {
            ++end;
          }
          if (isKeyword(line.substr(i, end - i))) {
            emit(i, end, ColorRole::Accent);
          } else {
            i = end;
          }
        } else {
          ++i;
        }
      }
      flush(line.size());
      return out;
    }
  } // namespace

  // One row per line (VirtualListView builds only the visible ones): the number in label_3, then the tokens.
  class CodeLines final : public VirtualListAdapter {
  public:
    // Syntax lives in this file's anonymous namespace, so the class keeps its two fields, not the struct.
    CodeLines(const std::string& text, std::string_view lineComment, bool blockComments)
        : m_lineComment(lineComment), m_blockComments(blockComments) {
      const Syntax syntax{.lineComment = m_lineComment, .blockComments = m_blockComments};
      constexpr std::size_t kMaxColumns = 400;
      std::size_t start = 0;
      // A final newline ends the last line; it does not start an empty one.
      while (start < text.size() || (start == 0 && text.empty())) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) {
          end = text.size();
        }
        std::string line;
        for (std::size_t i = start; i < end && line.size() < kMaxColumns; ++i) {
          if (text[i] == '\t') {
            line.append(4 - line.size() % 4, ' ');
          } else if (text[i] != '\r') {
            line.push_back(text[i]);
          }
        }
        m_lines.push_back(std::move(line));
        start = end + 1;
      }
      // The /* state at each line start, so a row can be scanned alone.
      bool inBlock = false;
      m_blockAtStart.reserve(m_lines.size());
      for (const std::string& line : m_lines) {
        m_blockAtStart.push_back(inBlock);
        if (syntax.blockComments) {
          (void)scanLine(line, syntax, inBlock);
        }
      }
      m_numberWidth = static_cast<float>(std::to_string(m_lines.size()).size()) * kDigitWidth;
    }

    [[nodiscard]] std::size_t itemCount() const override { return m_lines.size(); }
    [[nodiscard]] float measureItem(Renderer&, std::size_t, float) override { return Tokens::spaceP16; }
    [[nodiscard]] std::unique_ptr<Node> createItem() override {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP12, .height = Tokens::spaceP16});
      row->addChild(ui::label({.fontSize = Style::fontSizeCaption, .fontFamily = kMonospace,
                               .color = colorSpecFromRole(ColorRole::Label3), .width = m_numberWidth,
                               .textAlign = TextAlign::End}));
      row->addChild(ui::row({.align = FlexAlign::Center, .clipChildren = true, .flexGrow = 1.0F}));
      return row;
    }
    void bindItem(Renderer&, Node& item, std::size_t index, float, bool) override {
      static_cast<Label*>(item.children()[0].get())->setText(std::to_string(index + 1));
      auto* tokens = static_cast<Flex*>(item.children()[1].get());
      while (!tokens->children().empty()) {
        (void)tokens->removeChild(tokens->children().back().get());
      }
      bool inBlock = m_blockAtStart[index];
      const Syntax syntax{.lineComment = m_lineComment, .blockComments = m_blockComments};
      for (const Token& token : scanLine(m_lines[index], syntax, inBlock)) {
        tokens->addChild(ui::label({.text = std::string(token.text), .fontSize = Style::fontSizeCaption,
                                    .fontFamily = kMonospace, .color = colorSpecFromRole(token.role)}));
      }
    }

  private:
    static constexpr float kDigitWidth = 8.0F; // one mono 12 digit, rounded up
    std::string_view m_lineComment; // a literal from syntaxFor()
    bool m_blockComments = false;
    std::vector<std::string> m_lines;
    std::vector<bool> m_blockAtStart;
    float m_numberWidth = 0.0F;
  };

  PreviewWindow::PreviewWindow(AppLoop& loop, desk::ForeignParent& parent, FileOpsClient& fileOps, Callbacks callbacks)
      : m_loop(loop), m_parent(parent), m_fileOps(fileOps), m_callbacks(std::move(callbacks)) {
    m_loop.wayland().setKeyboardEventCallback([this](const KeyboardEvent& event) { onKeyboard(event); });
    m_loop.wayland().setPointerEventCallback([this](const PointerEvent& event) { onPointer(event); });
  }

  PreviewWindow::~PreviewWindow() { close(); }

  void PreviewWindow::show(const std::string& path, const std::string& windowHandle, bool closeIfShown) {
    if (isOpen() && path == m_path && closeIfShown) {
      close();
      return;
    }
    load(path);
    if (!isOpen()) {
      openSurface(windowHandle);
    } else {
      m_parent.setParent(m_surface->wlSurface(), windowHandle);
      invalidate();
    }
  }

  void PreviewWindow::load(const std::string& path) {
    ++m_generation;
    m_decoder.cancel();
    m_pendingUploads.clear();
    dropHelper();
    m_path = path;
    m_contentType = guessContentType(path);
    m_kind = kindForContentType(m_contentType);
    m_openWith = defaultAppName(m_contentType);
    m_image = {};
    m_pages.clear();
    m_text.clear();
    m_textTruncated = false;
    m_archive.clear();
    m_archiveTotal = 0;
    m_fontPage = {};
    m_fontRequested = false;
    m_folderItems = 0;
    m_folderBytes = 0;
    m_folderCut = false;
    m_folderPeek.clear();
    m_currentPage = 0;
    m_failed = false;
    m_loading = true;
    m_dirty = true;
    const auto generation = m_generation;
    m_spinner = false;
    m_spinnerTimer.start(std::chrono::milliseconds(200), [this, generation] {
      if (generation == m_generation && m_loading) {
        m_spinner = true;
        invalidate();
      }
    });

    switch (m_kind) {
    case FileKind::Image:
      m_decoder.request(path, kImageBox, kImageBox, [this, generation](Pixels pixels) {
        if (generation != m_generation) {
          return;
        }
        m_loading = false;
        m_failed = pixels.width == 0;
        m_image = std::move(pixels);
        if (!m_failed) {
          m_shownNote = std::format("shown image {}x{}", m_image.width, m_image.height);
        }
        invalidate();
      });
      break;
    case FileKind::Pdf:
      loadPdf(path);
      break;
    case FileKind::Office:
      loadOffice();
      break;
    case FileKind::Text:
      m_helper = HelperClient::start(HelperKind::Text, path);
      if (!m_helper) {
        m_loading = false;
        m_failed = true;
        break;
      }
      m_loop.addSource(m_helper.get());
      m_helper->request({.op = op::Text}, [this, generation](const HelperReply& reply) {
        if (generation != m_generation) {
          return;
        }
        m_loading = false;
        if (reply.msg.op == op::TextReply && reply.data != nullptr) {
          const auto bytes = reply.data->bytes();
          m_text.assign(reinterpret_cast<const char*>(bytes.data()), std::min<std::size_t>(reply.msg.a, bytes.size()));
          m_textTruncated = reply.msg.b != 0;
          m_shownNote = std::format("shown text {} bytes", m_text.size());
        } else {
          m_failed = true;
        }
        invalidate();
      });
      break;
    case FileKind::Folder:
      // Names and sizes only (no file contents), off the main thread; the result comes back on the main loop.
      std::thread([this, generation, path] {
        namespace fs = std::filesystem;
        constexpr std::size_t kMaxWalk = 200000;
        std::size_t items = 0;
        std::size_t walked = 0;
        std::uint64_t bytes = 0;
        std::vector<std::pair<fs::file_time_type, std::string>> newest;
        std::error_code ec;
        for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
          const auto& entry = *it;
          if (entry.path().filename().string().starts_with('.')) {
            continue; // hidden items are not counted, as the Files views hide them
          }
          ++items;
          std::error_code e2;
          newest.emplace_back(entry.last_write_time(e2), entry.path().string());
        }
        std::ranges::sort(newest, std::greater{}, &decltype(newest)::value_type::first);
        newest.resize(std::min<std::size_t>(newest.size(), 4));
        for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && walked < kMaxWalk; it.increment(ec), ++walked) {
          std::error_code e2;
          if (it->is_regular_file(e2) && !it->is_symlink(e2)) {
            bytes += it->file_size(e2);
          }
        }
        std::vector<std::string> peek;
        for (auto& [time, name] : newest) {
          peek.push_back(std::move(name));
        }
        DeferredCall::callLater([this, generation, items, bytes, cut = walked >= kMaxWalk, peek = std::move(peek)] {
          if (generation != m_generation) {
            return;
          }
          m_loading = false;
          m_folderItems = items;
          m_folderBytes = bytes;
          m_folderCut = cut;
          m_folderPeek = peek;
          m_shownNote = std::format("shown folder {} items", items);
          invalidate();
        });
      }).detach();
      break;
    case FileKind::Font:
      // The specimen is requested at the first build, once the body width is known (requestFont).
      m_helper = HelperClient::start(HelperKind::Font, path);
      if (!m_helper) {
        m_loading = false;
        m_failed = true;
        break;
      }
      m_loop.addSource(m_helper.get());
      break;
    case FileKind::Archive:
      m_fileOps.listArchive(path, [this, generation](std::vector<FileOpsClient::ArchiveEntry> entries,
                                                     const std::string& error, bool) {
        if (generation != m_generation) {
          return;
        }
        m_loading = false;
        m_failed = !error.empty();
        m_archiveTotal = entries.size();
        // A long archive shows its first rows and a count; the window builds no node per entry beyond them.
        constexpr std::size_t kRows = 500;
        for (std::size_t i = 0; i < entries.size() && i < kRows; ++i) {
          m_archive.push_back({.path = entries[i].path.string(), .size = entries[i].size, .isDir = entries[i].isDir});
        }
        if (!m_failed) {
          m_shownNote = std::format("shown archive {} entries", m_archiveTotal);
        }
        invalidate();
      });
      break;
    case FileKind::Video:
    case FileKind::Audio: {
      // ANDROMEDA_PREVIEW_MEDIA=sw starts on the software path the window falls back to when dmabuf import fails.
      const char* media = std::getenv("ANDROMEDA_PREVIEW_MEDIA");
      loadMedia(media != nullptr && std::string_view(media) == "sw" ? HelperKind::MediaSoftware : HelperKind::Media);
      break;
    }
    default:
      m_loading = false;
      m_shownNote = "shown unsupported";
      break;
    }
  }

  // LibreOffice converts the file to a PDF once per path, size and mtime; the PDF renderer shows it. Without
  // LibreOffice the window shows the file card with Open With.
  void PreviewWindow::loadOffice() {
    namespace fs = std::filesystem;
    struct stat st{};
    gchar* soffice = g_find_program_in_path("soffice");
    if (soffice == nullptr || ::stat(m_path.c_str(), &st) != 0) {
      g_free(soffice);
      m_loading = false;
      m_shownNote = "shown unsupported";
      return;
    }
    const fs::path dir = fs::path(g_get_user_cache_dir()) / "andromeda" / "preview-office";
    const std::string key = std::format("{:016x}", std::hash<std::string>{}(
                                                       std::format("{}\n{}\n{}", m_path, st.st_size, st.st_mtim.tv_sec)));
    const fs::path pdf = dir / (key + ".pdf");
    std::error_code ec;
    if (fs::exists(pdf, ec)) {
      g_free(soffice);
      kLog.info("office {} from cache", m_path);
      loadPdf(pdf.string());
      return;
    }
    const auto generation = m_generation;
    std::thread([this, generation, source = m_path, dir, key, pdf, program = std::string(soffice)] {
      std::error_code err;
      const fs::path work = dir / (key + ".d");
      fs::create_directories(work, err);
      const auto start = std::chrono::steady_clock::now();
      // Own profile: a running LibreOffice with the user's profile would take the job and never write the file.
      const std::string profile = "-env:UserInstallation=file://" + (dir / "profile").string();
      const std::string outdir = work.string();
      const char* argv[] = {"timeout",     "60",          program.c_str(), profile.c_str(), "--headless",
                            "--norestore", "--convert-to", "pdf",          "--outdir",      outdir.c_str(),
                            source.c_str(), nullptr};
      gint status = -1;
      g_spawn_sync(nullptr, const_cast<gchar**>(argv), nullptr,
                   static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                   nullptr, nullptr, nullptr, nullptr, &status, nullptr);
      const fs::path made = work / fs::path(source).filename().replace_extension(".pdf");
      const bool ok = fs::exists(made, err) && (fs::rename(made, pdf, err), !err);
      fs::remove_all(work, err);
      const auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      DeferredCall::callLater([this, generation, ok, ms, pdf, source] {
        kLog.info("office {} converted {} in {} ms", source, ok ? "ok" : "failed", ms);
        if (generation != m_generation) {
          return;
        }
        if (!ok) {
          helperFailed();
          return;
        }
        loadPdf(pdf.string());
      });
    }).detach();
    g_free(soffice);
  }

  void PreviewWindow::loadPdf(const std::string& path) {
    m_helper = HelperClient::start(HelperKind::Pdf, path);
    if (!m_helper) {
      m_loading = false;
      m_failed = true;
      return;
    }
    m_loop.addSource(m_helper.get());
    const auto generation = m_generation;
    m_helper->request({.op = op::Info}, [this, generation](const HelperReply& reply) {
      if (generation != m_generation) {
        return;
      }
      if (reply.msg.op != op::InfoReply || reply.msg.b == 0) {
        helperFailed();
        return;
      }
      if (reply.msg.c > 0 && reply.msg.d > 0) {
        m_pageAspect = static_cast<float>(reply.msg.d) / static_cast<float>(reply.msg.c);
      }
      m_pages.resize(std::min<std::uint32_t>(reply.msg.b, 10000));
      m_loading = false;
      requestPage(0);
      invalidate();
    });
  }

  void PreviewWindow::dropHelper() {
    m_video.reset();
    m_videoBox = nullptr;
    m_videoCorners = {};
    m_letterbox = {};
    m_audio.reset();
    m_progressTimer.stop();
    m_progress = nullptr;
    m_time = nullptr;
    m_peaks.clear();
    if (m_peaksHelper) {
      m_loop.removeSource(m_peaksHelper.get());
      m_peaksHelper.reset();
    }
    m_audioOnly = false;
    m_paused = false;
    m_durationMs = 0;
    m_title.clear();
    m_artist.clear();
    m_cover = {};
    if (m_coverFd >= 0) {
      ::close(m_coverFd);
      m_coverFd = -1;
    }
    if (m_helper) {
      m_loop.removeSource(m_helper.get());
      m_helper.reset();
    }
  }

  void PreviewWindow::loadMedia(HelperKind kind) {
    m_helper = HelperClient::start(kind, m_path);
    if (!m_helper) {
      m_loading = false;
      m_failed = true;
      return;
    }
    m_loop.addSource(m_helper.get());
    const auto generation = m_generation;
    m_helper->request({.op = op::Info}, [this, generation, kind](const HelperReply& reply) {
      if (generation != m_generation) {
        return;
      }
      constexpr std::uint32_t kFlagVideo = 1;
      constexpr std::uint32_t kFlagAudio = 2;
      const bool video = (reply.msg.e & kFlagVideo) != 0;
      const bool audio = (reply.msg.e & kFlagAudio) != 0;
      if (reply.msg.op != op::InfoReply || !(video || audio) || !m_surface) {
        helperFailed();
        return;
      }
      m_durationMs = reply.msg.d;
      if (!video) {
        m_audioOnly = true;
        startAudio(false);
        return;
      }
      m_video = std::make_unique<VideoView>(
          m_loop.wayland(), *m_helper,
          VideoView::Callbacks{
              .firstFrame =
                  [kind](std::uint32_t w, std::uint32_t h) {
                    kLog.info("shown video {}x{}{}", w, h, kind == HelperKind::MediaSoftware ? " (software)" : "");
                  },
              .failed =
                  [this, kind](bool unsupported) {
                    if (!unsupported || kind != HelperKind::Media) {
                      helperFailed();
                      return;
                    }
                    // Runs inside the helper's callback: restart after it returns.
                    DeferredCall::callLater([this, generation = m_generation] {
                      if (generation == m_generation) {
                        dropHelper();
                        loadMedia(HelperKind::MediaSoftware);
                      }
                    });
                  },
              .moved =
                  [this] {
                    if (m_surface) {
                      m_surface->requestRedraw();
                    }
                  },
          }
      );
      m_video->setVideoSize(reply.msg.b, reply.msg.c);
      // Audio first: the helper starts playing on the first request, and audio decoded before OP_AUDIO is dropped.
      if (audio) {
        startAudio(true);
        m_video->setClock([this]() -> std::optional<std::chrono::nanoseconds> {
          return m_audio ? m_audio->position() : std::nullopt;
        });
      }
      if (!m_video->attach(m_surface->wlSurface())) {
        helperFailed();
        return;
      }
      m_loading = false;
      startProgress();
      invalidate();
    });
  }

  void PreviewWindow::startProgress() {
    m_progressTimer.startRepeating(std::chrono::milliseconds(250), [this] { updateProgress(); });
  }

  void PreviewWindow::updateProgress() {
    if (!m_surface) {
      return;
    }
    std::chrono::nanoseconds position{0};
    if (m_audio) {
      position = m_audio->position().value_or(position);
    } else if (m_video) {
      position = m_video->shownPts();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(position).count();
    const auto clock = [](std::int64_t t) { return std::format("{}:{:02}", t / 60000, (t / 1000) % 60); };
    if (m_progress != nullptr && m_durationMs > 0) {
      m_progress->setProgress(std::clamp(static_cast<float>(ms) / static_cast<float>(m_durationMs), 0.0F, 1.0F));
    }
    if (m_time != nullptr
        && m_time->setText(m_durationMs > 0 ? clock(ms) + " / " + clock(m_durationMs) : clock(ms))) {
      m_surface->requestLayout();
    }
    m_surface->requestRedraw();
  }

  // Mockup .vbar: play/pause 12, a 4 px progress capsule (fill_2 track, accent level), "0:17 / 0:42" 500 12 label_2.
  // Display only: the helper cannot seek.
  std::unique_ptr<Node> PreviewWindow::buildMediaBar() {
    auto bar = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8, .height = Tokens::spaceP24});
    bar->addChild(ui::button({.glyph = std::string(m_paused ? "player-play" : "player-pause"), .glyphSize = 12.0F,
                              .variant = ButtonVariant::Ghost, .tooltip = std::string(m_paused ? "Play (K)" : "Pause (K)"),
                              .radius = Tokens::spaceP24 / 2.0F, .width = Tokens::spaceP24, .height = Tokens::spaceP24,
                              .onClick = [this] { togglePlayback(); }}));
    bar->addChild(ui::progressBar({.out = &m_progress, .fill = colorSpecFromRole(ColorRole::Accent),
                                   .track = colorSpecFromRole(ColorRole::Fill2), .radius = 2.0F, .progress = 0.0F,
                                   .height = 4.0F, .flexGrow = 1.0F}));
    bar->addChild(ui::label({.out = &m_time, .text = std::string("0:00"), .fontSize = Style::fontSizeCaption,
                             .fontWeight = FontWeight::Medium, .color = colorSpecFromRole(ColorRole::Label2)}));
    return bar;
  }

  void PreviewWindow::requestPeaks() {
    // Cached per path and mtime: in memory while the previewer runs, on disk across runs.
    struct stat st{};
    if (::stat(m_path.c_str(), &st) != 0) {
      return;
    }
    const std::string key = std::format("{}\n{}.{}", m_path, st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    static std::unordered_map<std::string, std::vector<std::uint8_t>> memory;
    if (const auto hit = memory.find(key); hit != memory.end()) {
      m_peaks = hit->second;
      return;
    }
    const char* cacheHome = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path dir = cacheHome != nullptr && *cacheHome != '\0'
        ? std::filesystem::path(cacheHome) / "andromeda" / "peaks"
        : std::filesystem::path(home != nullptr ? home : "/tmp") / ".cache" / "andromeda" / "peaks";
    const auto file = dir / std::format("{:016x}", std::hash<std::string>{}(key));
    if (std::FILE* in = std::fopen(file.c_str(), "rb"); in != nullptr) {
      std::vector<std::uint8_t> bytes(kPeakCount);
      const bool whole = std::fread(bytes.data(), 1, bytes.size(), in) == bytes.size();
      std::fclose(in);
      if (whole) {
        memory[key] = bytes;
        m_peaks = std::move(bytes);
        return;
      }
    }
    m_peaksHelper = HelperClient::start(HelperKind::Media, m_path);
    if (!m_peaksHelper) {
      return;
    }
    m_loop.addSource(m_peaksHelper.get());
    m_peaksHelper->request(
        {.op = op::Peaks, .a = static_cast<std::uint32_t>(kPeakCount)},
        [this, generation = m_generation, key, dir, file](const HelperReply& reply) {
          if (generation != m_generation) {
            return;
          }
          // Runs inside the peaks helper's own callback: drop it after it returns.
          DeferredCall::callLater([this, generation] {
            if (generation == m_generation && m_peaksHelper) {
              m_loop.removeSource(m_peaksHelper.get());
              m_peaksHelper.reset();
            }
          });
          if (reply.msg.op != op::PeaksReply || reply.data == nullptr || reply.msg.a != kPeakCount
              || reply.data->bytes().size() < kPeakCount) {
            return;
          }
          const auto bytes = reply.data->bytes();
          m_peaks.assign(bytes.begin(), bytes.begin() + kPeakCount);
          memory[key] = m_peaks;
          std::error_code ec;
          std::filesystem::create_directories(dir, ec);
          if (std::FILE* out = std::fopen(file.c_str(), "wb"); out != nullptr) {
            std::fwrite(m_peaks.data(), 1, m_peaks.size(), out);
            std::fclose(out);
          }
          kLog.info("waveform {} bars", m_peaks.size());
          invalidate();
        }
    );
  }

  void PreviewWindow::requestFont(float width) {
    if (!m_helper || !m_surface) {
      return;
    }
    m_fontRequested = true;
    const float scale = m_surface->effectiveBufferScale();
    const auto px = static_cast<std::uint32_t>(std::lround(std::max(1.0F, width - 2 * Tokens::spaceP24) * scale));
    const Color& ink = colorForRole(ColorRole::Label1);
    const auto channel = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
    const std::uint32_t rgb = (channel(ink.r) << 16) | (channel(ink.g) << 8) | channel(ink.b);
    m_helper->request(
        {.op = op::Render, .a = 0, .b = px, .c = 4096, .d = rgb, .e = static_cast<std::uint32_t>(std::lround(scale * 100.0F))},
        [this, generation = m_generation](const HelperReply& reply) {
          if (generation != m_generation) {
            return;
          }
          const auto& m = reply.msg;
          // Untrusted sizes, checked as for PDF pages.
          const bool valid = m.op == op::Page && reply.data != nullptr && m.b > 0 && m.c > 0 && m.b <= 8192
              && m.c <= 4096 && m.b <= m.d / 4 && static_cast<std::uint64_t>(m.d) * m.c <= reply.data->bytes().size();
          if (!valid) {
            helperFailed();
            return;
          }
          m_fontPage = {m.b, m.c, m.d, reply.data, true};
          m_loading = false;
          m_shownNote = std::format("shown font {}x{}", m.b, m.c);
          invalidate();
        }
    );
  }

  void PreviewWindow::startAudio(bool withVideo) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
      kLog.warn("audio: no pipe");
    } else {
      // About 1.4 s of PCM in flight: the helper blocks on a full pipe, which is the backpressure.
      fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);
      m_audio = AudioOut::start(fds[0]);
      if (m_audio) {
        m_helper->request({.op = op::Audio}, fds[1], [](const HelperReply& reply) {
          if (reply.msg.op != op::AudioReply) {
            kLog.warn("audio: the helper cannot stream ({})", reply.msg.a);
          }
        });
      }
      ::close(fds[1]);
    }
    if (withVideo) {
      return;
    }
    requestPeaks();
    const auto generation = m_generation;
    m_helper->request({.op = op::Tags}, [this, generation](const HelperReply& reply) {
      if (generation != m_generation) {
        return;
      }
      if (reply.msg.op == op::TextReply && reply.data != nullptr) {
        const auto bytes = reply.data->bytes();
        const std::string text(reinterpret_cast<const char*>(bytes.data()), std::min<std::size_t>(reply.msg.a, bytes.size()));
        const auto first = text.find('\n');
        m_title = text.substr(0, first);
        if (first != std::string::npos) {
          const auto second = text.find('\n', first + 1);
          m_artist = text.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
        }
      }
      m_loading = false;
      m_shownNote = std::format("shown audio {}", m_title.empty() ? baseName(m_path) : m_title);
      startProgress();
      invalidate();
    });
    m_helper->request({.op = op::Cover}, [this, generation](const HelperReply& reply) {
      if (generation != m_generation || reply.msg.op != op::CoverReply || reply.data == nullptr) {
        return;
      }
      const auto bytes = reply.data->bytes();
      const int fd = memfd_create("preview-cover", MFD_CLOEXEC);
      const std::size_t size = std::min<std::size_t>(reply.msg.a, bytes.size());
      if (fd < 0 || write(fd, bytes.data(), size) != static_cast<ssize_t>(size)) {
        if (fd >= 0) {
          ::close(fd);
        }
        return;
      }
      m_coverFd = fd;
      m_decoder.request(std::format("/proc/self/fd/{}", fd), kCoverBox, kCoverBox, [this, generation](Pixels pixels) {
        if (generation != m_generation) {
          return;
        }
        if (m_coverFd >= 0) {
          ::close(m_coverFd);
          m_coverFd = -1;
        }
        m_cover = std::move(pixels);
        invalidate();
      });
    });
  }

  void PreviewWindow::togglePlayback() {
    m_paused = !m_paused;
    kLog.info("playback {}", m_paused ? "paused" : "playing");
    if (m_video) {
      m_video->togglePlaying();
    }
    if (m_audio) {
      m_audio->setPlaying(!m_paused);
    }
    invalidate(); // the bar's play/pause glyph
  }

  std::unique_ptr<Node> PreviewWindow::buildAudio(Renderer& renderer, float width) {
    auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm,
                           .flexGrow = 1.0F});
    if (!m_cover.bytes.empty()) {
      auto cover = ui::image({.fit = ImageFit::Contain, .width = kCoverSize, .height = kCoverSize});
      cover->setSourceRaw(renderer, m_cover.bytes.data(), m_cover.bytes.size(), static_cast<int>(m_cover.width),
                          static_cast<int>(m_cover.height), static_cast<int>(m_cover.stride), PixmapFormat::RGBA);
      box->addChild(std::move(cover));
    } else {
      box->addChild(ui::glyph({.glyph = "music", .glyphSize = kCoverSize / 2.0F,
                               .color = colorSpecFromRole(ColorRole::Label3), .width = kCoverSize,
                               .height = kCoverSize}));
    }
    box->addChild(ui::label({.text = m_title.empty() ? baseName(m_path) : m_title, .fontSize = Style::fontSizeTitle,
                             .color = colorSpecFromRole(ColorRole::Label1), .maxWidth = width - 2 * Style::spaceLg,
                             .maxLines = 2, .textAlign = TextAlign::Center}));
    if (!m_artist.empty()) {
      box->addChild(ui::label({.text = m_artist, .fontSize = Style::fontSizeCaption,
                               .color = colorSpecFromRole(ColorRole::Label2), .maxWidth = width - 2 * Style::spaceLg,
                               .maxLines = 1}));
    }
    auto column = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8, .flexGrow = 1.0F});
    column->addChild(std::move(box));
    if (!m_peaks.empty()) {
      // Mockup .wave: accent bars, radius 2, gap 4, tallest 36.
      auto wave = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4, .height = kWaveHeight});
      // Scaled to the loudest bar, so a quiet recording still shows its shape.
      const float loudest = std::max<float>(1.0F, *std::ranges::max_element(m_peaks));
      for (const std::uint8_t peak : m_peaks) {
        const float h = std::max(Tokens::spaceP4, kWaveHeight * static_cast<float>(peak) / loudest);
        wave->addChild(ui::column({.fill = colorSpecFromRole(ColorRole::Accent), .radius = 2.0F, .height = h,
                                   .flexGrow = 1.0F}));
      }
      column->addChild(std::move(wave));
    }
    column->addChild(buildMediaBar());
    return column;
  }

  void PreviewWindow::requestPage(std::size_t index) {
    if (!m_helper || index >= m_pages.size() || m_pages[index].requested) {
      return;
    }
    m_pages[index].requested = true;
    const float scale = m_surface != nullptr ? m_surface->effectiveBufferScale() : 1.0F;
    const auto width = static_cast<std::uint32_t>(std::lround(kDocumentWidth * scale));
    const auto generation = m_generation;
    m_helper->request(
        {.op = op::Render, .a = static_cast<std::uint32_t>(index), .b = width, .c = kMaxPageHeight},
        [this, generation, index](const HelperReply& reply) {
          if (generation == m_generation) {
            onPage(index, reply);
          }
        }
    );
  }

  void PreviewWindow::onPage(std::size_t index, const HelperReply& reply) {
    if (reply.msg.op == 0) {
      helperFailed();
      return;
    }
    const auto& m = reply.msg;
    // Untrusted sizes: rows must fit the stride and the mapping, and stay inside the helper's caps.
    const bool valid = m.op == op::Page && reply.data != nullptr && index < m_pages.size() && m.b > 0 && m.c > 0
        && m.b <= 8192 && m.c <= kMaxPageHeight && m.b <= m.d / 4
        && static_cast<std::uint64_t>(m.d) * m.c <= reply.data->bytes().size();
    if (!valid) {
      return;
    }
    const bool sameShape = m_pages[index].pixels == nullptr
        ? std::abs(static_cast<float>(m.c) / static_cast<float>(m.b) - m_pageAspect) < 0.01F
        : m_pages[index].width == m.b && m_pages[index].height == m.c;
    m_pages[index] = {m.b, m.c, m.d, reply.data, true};
    if (index == 0) {
      m_shownNote = std::format("shown pdf page 1 of {}", m_pages.size());
    }
    if (sameShape && index < m_pageImages.size() && !m_dirty) {
      m_pendingUploads.push_back(index);
      if (m_surface) {
        m_surface->requestLayout();
      }
    } else {
      invalidate();
    }
  }

  void PreviewWindow::helperFailed() {
    // Runs inside the helper client's own callback: drop the client after it returns.
    DeferredCall::callLater([this, generation = m_generation] {
      if (generation == m_generation) {
        dropHelper();
      }
    });
    m_loading = false;
    m_failed = true;
    invalidate();
  }

  void PreviewWindow::uploadPendingPages(Renderer& renderer) {
    for (const std::size_t index : m_pendingUploads) {
      if (index >= m_pageImages.size() || index >= m_pages.size()) {
        continue;
      }
      const Page& page = m_pages[index];
      if (page.pixels == nullptr) {
        // The strip keeps its scaled page: it costs a texture, not the full-size mapping.
        m_pageImages[index]->clear(renderer);
        continue;
      }
      const auto bytes = page.pixels->bytes();
      m_pageImages[index]->setSourceRaw(renderer, bytes.data(), bytes.size(), static_cast<int>(page.width),
                                        static_cast<int>(page.height), static_cast<int>(page.stride),
                                        PixmapFormat::BGRA);
      if (index < m_stripImages.size() && !m_stripImages[index]->hasImage()) {
        setStripPage(*m_stripImages[index], renderer, bytes, page.width, page.height, page.stride,
                     m_surface->effectiveBufferScale());
      }
    }
    m_pendingUploads.clear();
  }

  void PreviewWindow::requestVisiblePages() {
    if (m_scroll == nullptr || m_pages.empty()) {
      return;
    }
    const float pageHeight = kDocumentWidth * m_pageAspect + kPageGap;
    const float top = m_scroll->scrollOffset();
    const float bottom = top + m_scroll->height() * 2.0F;
    const auto first = static_cast<std::size_t>(std::max(0.0F, top / pageHeight));
    const auto last = static_cast<std::size_t>(std::max(0.0F, bottom / pageHeight));
    for (std::size_t i = first; i <= last && i < m_pages.size(); ++i) {
      requestPage(i);
    }
    // Keep a few rendered pages around the view; a long document must not hold every page in memory.
    constexpr std::size_t kKeep = 3;
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
      if ((i + kKeep < first || i > last + kKeep) && m_pages[i].pixels != nullptr) {
        m_pages[i].pixels.reset();
        m_pages[i].requested = false;
        m_pendingUploads.push_back(i);
      }
    }
    if (!m_pendingUploads.empty() && m_surface) {
      m_surface->requestLayout();
    }
  }

  std::pair<std::uint32_t, std::uint32_t> PreviewWindow::windowSizeFor(const WaylandOutput* output) const {
    std::pair<float, float> size{480.0F, 320.0F};
    switch (m_kind) {
    case FileKind::Image:
      size = {kDocumentWidth + 2 * (Style::spaceLg + Tokens::spaceP16), 760.0F};
      break;
    case FileKind::Pdf:
    case FileKind::Office:
      size = {kDocumentWidth + 2 * (Style::spaceLg + Tokens::spaceP16) + kStripWidth + Tokens::spaceP8, 760.0F};
      break;
    case FileKind::Text:
      size = {720.0F, 560.0F};
      break;
    case FileKind::Archive:
    case FileKind::Folder:
      size = {560.0F, 480.0F};
      break;
    case FileKind::Font:
      size = {560.0F, 320.0F};
      break;
    case FileKind::Video: // a 16:9 picture inside the 16 inset, its bar (24 + gap 8), the header and the footer
      size = {854.0F, (854.0F - 2 * Tokens::spaceP16) * 9.0F / 16.0F + Tokens::spaceP24 + Tokens::spaceP8
                          + 2 * kHeaderHeight};
      break;
    default:
      break;
    }
    // Leave room around the window on small outputs.
    if (output != nullptr && output->hasUsableGeometry()) {
      size.first = std::min(size.first, 0.85F * static_cast<float>(output->effectiveLogicalWidth()));
      size.second = std::min(size.second, 0.85F * static_cast<float>(output->effectiveLogicalHeight()));
    }
    return {static_cast<std::uint32_t>(size.first), static_cast<std::uint32_t>(size.second)};
  }

  void PreviewWindow::openSurface(const std::string& windowHandle) {
    m_surface = std::make_unique<ToplevelSurface>(m_loop.wayland());
    m_surface->setRenderContext(&m_loop.renderContext());
    m_surface->setAnimationManager(&m_animations);
    m_surface->setClosedCallback([this] { DeferredCall::callLater([this] { close(); }); });
    m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
      if (m_surface) {
        m_surface->requestLayout();
      }
    });
    m_surface->setPrepareFrameCallback([this](bool, bool needsLayout) { prepareFrame(needsLayout); });
    m_surface->setUpdateCallback([] {});
    wl_output* output = m_loop.wayland().lastPointerOutput();
    if (output == nullptr && !m_loop.wayland().outputs().empty()) {
      output = m_loop.wayland().outputs().front().output;
    }
    const auto [width, height] = windowSizeFor(m_loop.wayland().findOutputByWl(output));
    const ToplevelSurfaceConfig config{
        .width = width,
        .height = height,
        .minWidth = 320,
        .minHeight = 200,
        .title = baseName(m_path),
        .appId = "andromeda-preview",
    };
    if (!m_surface->initialize(output, config)) {
      kLog.warn("cannot create the preview window");
      m_surface.reset();
      ++m_generation;
      m_decoder.cancel();
      dropHelper();
      if (m_callbacks.closed) {
        m_callbacks.closed();
      }
      return;
    }
    m_parent.setParent(m_surface->wlSurface(), windowHandle);
    m_dirty = true;
  }

  void PreviewWindow::close() {
    if (!m_surface) {
      return;
    }
    ++m_generation;
    dropHelper();
    m_input.pointerLeave();
    m_input.setSceneRoot(nullptr);
    m_surface->setSceneRoot(nullptr);
    m_scroll = nullptr;
    m_pageImages.clear();
    m_pendingUploads.clear();
    m_decoder.cancel();
    m_root.reset();
    m_code.reset();
    m_parent.clear();
    m_surface.reset();
    m_pages.clear();
    m_image = {};
    m_text.clear();
    m_path.clear();
    if (m_callbacks.closed) {
      m_callbacks.closed();
    }
  }

  void PreviewWindow::invalidate() {
    m_dirty = true;
    if (m_surface) {
      m_surface->requestLayout();
    }
  }

  void PreviewWindow::prepareFrame(bool needsLayout) {
    if (!m_surface) {
      return;
    }
    const auto width = static_cast<float>(m_surface->width());
    const auto height = static_cast<float>(m_surface->height());
    if (width <= 0.0F || height <= 0.0F) {
      return;
    }
    m_loop.renderContext().makeCurrent(m_surface->renderTarget());
    Renderer& renderer = m_surface->renderTarget().renderer();
    if (!m_root || m_dirty) {
      m_dirty = false;
      buildScene(renderer, width, height);
    } else {
      uploadPendingPages(renderer);
      if (needsLayout || m_root->width() != width || m_root->height() != height) {
        m_root->setSize(width, height);
        m_root->layout(renderer);
      }
    }
    if (m_video && m_videoBox != nullptr) {
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(m_videoBox, x, y);
      m_video->place(x, y, m_videoBox->width(), m_videoBox->height());
      // The picture rect in box coordinates: the letterbox covers the rest of the hole, the masks its corners.
      const auto pic = m_video->pictureRect();
      const float bw = m_videoBox->width();
      const float bh = m_videoBox->height();
      const float px = pic[2] > 0.0F ? pic[0] - x : 0.0F;
      const float py = pic[3] > 0.0F ? pic[1] - y : 0.0F;
      const float pw = pic[2] > 0.0F ? pic[2] : 0.0F;
      const float ph = pic[3] > 0.0F ? pic[3] : 0.0F;
      const std::array<std::array<float, 4>, 4> bars{{{0.0F, 0.0F, bw, py},
                                                      {0.0F, py + ph, bw, bh - py - ph},
                                                      {0.0F, py, px, ph},
                                                      {px + pw, py, bw - px - pw, ph}}};
      for (std::size_t i = 0; i < m_letterbox.size(); ++i) {
        if (m_letterbox[i] != nullptr) {
          m_letterbox[i]->setPosition(bars[i][0], bars[i][1]);
          m_letterbox[i]->setSize(std::max(0.0F, bars[i][2]), std::max(0.0F, bars[i][3]));
        }
      }
      const float r = std::min({Tokens::radiusR12, pw / 2.0F, ph / 2.0F});
      const std::array<std::pair<float, float>, 4> at{
          {{px, py}, {px + pw - r, py}, {px + pw - r, py + ph - r}, {px, py + ph - r}}};
      for (std::size_t i = 0; i < m_videoCorners.size(); ++i) {
        if (m_videoCorners[i] != nullptr) {
          m_videoCorners[i]->setPosition(at[i].first, at[i].second);
          m_videoCorners[i]->setSize(r, r);
        }
      }
    }
    // Logged once the decoded content is on screen (the E2E waits for it before its screenshot).
    if (!m_shownNote.empty()) {
      m_surface->callOnNextPresent([note = std::move(m_shownNote)] { kLog.info("{}", note); });
      m_shownNote.clear();
    }
  }

  void PreviewWindow::buildScene(Renderer& renderer, float width, float height) {
    const float offset = m_scroll != nullptr ? m_scroll->scrollOffset() : 0.0F;
    // The old tree's code list points at the old adapter: it goes only after the tree is replaced.
    const std::unique_ptr<CodeLines> retired = std::move(m_code);
    m_input.setSceneRoot(nullptr);
    m_scroll = nullptr;
    m_videoBox = nullptr;
    m_videoCorners = {};
    m_letterbox = {};
    m_progress = nullptr;
    m_time = nullptr;
    m_pageImages.clear();
    m_stripFrames.clear();
    m_stripImages.clear();
    m_pageLabel = nullptr;
    m_pendingUploads.clear();
    // Mockup .ql (files-v2-live §3): thick sheet; header 40 (padding 0 8 0 16, gap 8, 600 13), the content inset 16
    // in a radius-12 clip, a 40 hint footer.
    auto root = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
    // A video plays in a subsurface under the window: every part but the picture paints the window colour, so the
    // picture shows through a hole whose corners the masks round.
    const bool hole = m_video != nullptr && !m_audioOnly && !m_loading && !m_failed;
    const ColorSpec sheet = colorSpecFromRole(ColorRole::Popover);
    if (!hole) {
      root->setFill(sheet);
    }

    auto header = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8, .height = kHeaderHeight});
    header->setPadding(0.0F, Tokens::spaceP8, 0.0F, Tokens::spaceP16);
    header->addChild(ui::glyph({.glyph = kindGlyph(m_kind), .glyphSize = Tokens::sizeSymbol,
                                .color = colorSpecFromRole(ColorRole::Label2)}));
    header->addChild(ui::label({.text = baseName(m_path), .fontSize = Style::fontSizeBody,
                                .fontWeight = FontWeight::SemiBold, .color = colorSpecFromRole(ColorRole::Label1),
                                .maxLines = 1, .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
    if ((m_kind == FileKind::Pdf || m_kind == FileKind::Office) && !m_pages.empty()) {
      header->addChild(ui::label({.out = &m_pageLabel, .text = std::format("Page {} of {}", m_currentPage + 1, m_pages.size()),
                                  .fontSize = Style::fontSizeCaption, .fontWeight = FontWeight::Medium,
                                  .color = colorSpecFromRole(ColorRole::Label2)}));
    }
    // Mockup §15 header detail: a code file's line count, a song's length.
    std::string detail;
    if (m_kind == FileKind::Text && !m_loading && !m_failed && syntaxFor(m_path).has_value()) {
      const auto lines = std::ranges::count(m_text, '\n') + (m_text.empty() || m_text.ends_with('\n') ? 0 : 1);
      detail = std::format("{} lines{}", lines, m_textTruncated ? "+" : "");
    } else if (m_audioOnly && m_durationMs > 0) {
      detail = std::format("{}:{:02}", m_durationMs / 60000, (m_durationMs / 1000) % 60);
    }
    if (!detail.empty()) {
      header->addChild(ui::label({.text = detail, .fontSize = Style::fontSizeCaption, .fontWeight = FontWeight::Medium,
                                  .color = colorSpecFromRole(ColorRole::Label2)}));
    }
    if (!m_openWith.empty()) {
      header->addChild(ui::button({.text = "Open With " + m_openWith, .fontSize = Style::fontSizeBody,
                                   .variant = ButtonVariant::Default, .radius = Tokens::radiusR8,
                                   .height = Tokens::sizeControlM, .onClick = [this] { openInDefaultApp(); }}));
    }
    header->addChild(ui::button({.glyph = std::string("x"), .glyphSize = Tokens::sizeSymbol,
                                 .variant = ButtonVariant::Ghost, .tooltip = std::string("Close (Space)"),
                                 .radius = Tokens::sizeControlM / 2.0F, .width = Tokens::sizeControlM,
                                 .height = Tokens::sizeControlM,
                                 .onClick = [this] { DeferredCall::callLater([this] { close(); }); }}));
    if (hole) {
      header->setFill(sheet);
    }
    root->addChild(std::move(header));
    auto body = ui::column({.align = FlexAlign::Stretch, .radius = Tokens::radiusR12, .clipChildren = true,
                            .flexGrow = 1.0F});
    body->addChild(buildBody(renderer, width - 2.0F * Tokens::spaceP16));
    auto inset = ui::row({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
    // The side insets paint only over the hole: else the root fill already covers them (a translucent sheet twice
    // would read darker).
    const std::optional<ColorSpec> sideFill = hole ? std::optional<ColorSpec>(sheet) : std::nullopt;
    inset->addChild(ui::column({.fill = sideFill, .width = Tokens::spaceP16}));
    body->setFlexGrow(1.0F);
    inset->addChild(std::move(body));
    inset->addChild(ui::column({.fill = sideFill, .width = Tokens::spaceP16}));
    root->addChild(std::move(inset));

    auto footer = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP16,
                           .height = kHeaderHeight});
    const ColorSpec hint = colorSpecFromRole(ColorRole::Label2);
    footer->addChild(ui::button({.glyph = std::string("chevron-left"), .glyphSize = Tokens::sizeSymbol,
                                 .variant = ButtonVariant::Ghost, .tooltip = std::string("Previous file (←)"),
                                 .radius = Tokens::sizeControlS / 2.0F, .width = Tokens::sizeControlS,
                                 .height = Tokens::sizeControlS,
                                 .onClick = [this] { m_callbacks.selection(direction::Left); }}));
    footer->addChild(ui::label({.text = std::string("← → next file · Space closes"), .fontSize = Style::fontSizeCaption,
                                .fontWeight = FontWeight::Medium, .color = hint}));
    footer->addChild(ui::button({.glyph = std::string("chevron-right"), .glyphSize = Tokens::sizeSymbol,
                                 .variant = ButtonVariant::Ghost, .tooltip = std::string("Next file (→)"),
                                 .radius = Tokens::sizeControlS / 2.0F, .width = Tokens::sizeControlS,
                                 .height = Tokens::sizeControlS,
                                 .onClick = [this] { m_callbacks.selection(direction::Right); }}));
    if (hole) {
      footer->setFill(sheet);
    }
    root->addChild(std::move(footer));

    root->setSize(width, height);
    root->layout(renderer);
    m_root = std::move(root);
    if (m_scroll != nullptr && offset > 0.0F) {
      m_scroll->setScrollOffset(offset);
    }
    m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_loop.wayland().setCursorShape(serial, shape);
    });
    m_input.setSceneRoot(m_root.get());
    m_surface->setSceneRoot(m_root.get());
    markCurrentPage();
    if (m_time != nullptr) {
      updateProgress();
    }
  }

  void PreviewWindow::markCurrentPage() {
    if (m_scroll == nullptr || m_stripFrames.empty() || m_pageStep <= 0.0F) {
      return;
    }
    const auto current = std::min(static_cast<std::size_t>(std::lround(m_scroll->scrollOffset() / m_pageStep)),
                                  m_stripFrames.size() - 1);
    for (std::size_t i = 0; i < m_stripFrames.size(); ++i) {
      if (i == current) {
        m_stripFrames[i]->setBorder(colorSpecFromRole(ColorRole::Accent), 2.0F);
      } else {
        m_stripFrames[i]->setBorder(clearColorSpec(), 0.0F);
      }
    }
    if (m_pageLabel != nullptr && current != m_currentPage) {
      m_pageLabel->setText(std::format("Page {} of {}", current + 1, m_pages.size()));
      m_surface->requestLayout();
    }
    m_currentPage = current;
  }

  std::unique_ptr<Node> PreviewWindow::buildBody(Renderer& renderer, float width) {
    auto message = [&](const std::string& text) {
      auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm,
                             .flexGrow = 1.0F});
      box->addChild(ui::label({.text = baseName(m_path), .fontSize = Style::fontSizeTitle,
                               .color = colorSpecFromRole(ColorRole::Label1), .maxWidth = width - 2 * Style::spaceLg,
                               .maxLines = 2, .textAlign = TextAlign::Center}));
      box->addChild(ui::label({.text = text, .fontSize = Style::fontSizeCaption,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
      return box;
    };
    // Mockup §24: the file's page art, one line, and Open With; with no default app the line says so instead.
    auto state = [&](const std::string& text) {
      auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Tokens::spaceP12,
                             .flexGrow = 1.0F});
      auto icon = std::make_unique<FileIcon>(Tokens::componentFilesPreviewArt / 2.0F);
      if (const auto info = statFile(std::filesystem::path(m_path)); info.has_value()) {
        icon->bind(renderer, *info, m_path, nullptr);
      }
      box->addChild(std::move(icon));
      box->addChild(ui::label({.text = text, .fontSize = Style::fontSizeCaption, .fontWeight = FontWeight::Medium,
                               .color = colorSpecFromRole(ColorRole::Label2), .maxWidth = width - 2 * Tokens::spaceP16,
                               .maxLines = 2, .textAlign = TextAlign::Center}));
      if (m_openWith.empty()) {
        box->addChild(ui::label({.text = std::string("No app is set to open this kind of file"),
                                 .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::Label2)}));
      } else {
        box->addChild(ui::button({.text = "Open With " + m_openWith, .fontSize = Style::fontSizeBody,
                                  .variant = ButtonVariant::Default, .radius = Tokens::radiusR8,
                                  .height = Tokens::sizeControlM, .onClick = [this] { openInDefaultApp(); }}));
      }
      return box;
    };
    if (m_loading) {
      if (m_kind == FileKind::Font && !m_fontRequested) {
        requestFont(width);
      }
      auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0F});
      if (m_spinner) {
        box->addChild(ui::spinner({.color = colorSpecFromRole(ColorRole::Label2), .spinnerSize = Tokens::sizeControlS,
                                   .spinning = true}));
      }
      return box;
    }
    if (m_failed) {
      return state("This file cannot be previewed");
    }
    // An Office file shows its converted PDF pages; without LibreOffice it has none and shows the file card.
    switch (m_kind == FileKind::Office && !m_pages.empty() ? FileKind::Pdf : m_kind) {
    case FileKind::Image: {
      auto image = ui::image({.fit = ImageFit::Contain, .padding = Style::spaceLg, .flexGrow = 1.0F});
      image->setSourceRaw(renderer, m_image.bytes.data(), m_image.bytes.size(), static_cast<int>(m_image.width),
                          static_cast<int>(m_image.height), static_cast<int>(m_image.stride), PixmapFormat::RGBA);
      return image;
    }
    case FileKind::Pdf: {
      auto split = ui::row({.align = FlexAlign::Stretch, .gap = Tokens::spaceP8, .flexGrow = 1.0F});
      auto strip = ui::scrollView({.scrollbarVisible = false, .viewportPaddingV = Tokens::spaceP8,
                                   .width = kStripWidth});
      strip->content()->setGap(Tokens::spaceP8);
      strip->content()->setAlign(FlexAlign::Center);
      const float thumbWidth = kStripPageHeight / m_pageAspect;
      for (std::size_t i = 0; i < m_pages.size(); ++i) {
        const Page& page = m_pages[i];
        // A page not rendered yet shows as a grey sheet (mockup §15).
        auto frame = ui::column({.fill = colorSpecFromRole(ColorRole::Label3), .radius = Tokens::radiusR4,
                                 .clipChildren = true, .width = thumbWidth, .height = kStripPageHeight});
        auto image = ui::image({.fit = ImageFit::Contain, .width = thumbWidth, .height = kStripPageHeight});
        if (page.pixels != nullptr) {
          setStripPage(*image, renderer, page.pixels->bytes(), page.width, page.height, page.stride,
                       m_surface->effectiveBufferScale());
        }
        m_stripImages.push_back(image.get());
        frame->addChild(std::move(image));
        // The current-page ring draws over the page: a Flex border paints under its children.
        auto ring = ui::column({.radius = Tokens::radiusR4, .width = thumbWidth, .height = kStripPageHeight,
                                .participatesInLayout = false});
        m_stripFrames.push_back(ring.get());
        frame->addChild(std::move(ring));
        auto hit = ui::inputArea({.tooltip = std::format("Page {}", i + 1), .width = thumbWidth,
                                  .height = kStripPageHeight,
                                  .onClick = [this, i](const InputArea::PointerData&) {
                                    if (m_scroll != nullptr) {
                                      m_scroll->scrollBy(static_cast<float>(i) * m_pageStep - m_scroll->scrollOffset());
                                    }
                                  }});
        hit->addChild(std::move(frame));
        strip->content()->addChild(std::move(hit));
      }
      split->addChild(std::move(strip));

      auto scroll = ui::scrollView({.viewportPaddingH = Style::spaceLg, .viewportPaddingV = Style::spaceLg});
      scroll->setFlexGrow(1.0F);
      scroll->content()->setGap(kPageGap);
      scroll->content()->setAlign(FlexAlign::Center);
      const float pageWidth = std::min(kDocumentWidth, width - kStripWidth - Tokens::spaceP8 - 2 * Style::spaceLg);
      m_pageStep = pageWidth * m_pageAspect + kPageGap;
      for (const Page& page : m_pages) {
        const float pageHeight = page.pixels != nullptr
            ? pageWidth * static_cast<float>(page.height) / static_cast<float>(page.width)
            : pageWidth * m_pageAspect;
        // The sheet shows while the page renders.
        auto sheet = ui::column({.fill = colorSpecFromRole(ColorRole::Fill1), .width = pageWidth, .height = pageHeight});
        auto image = ui::image({.fit = ImageFit::Contain, .width = pageWidth, .height = pageHeight});
        if (page.pixels != nullptr) {
          const auto bytes = page.pixels->bytes();
          image->setSourceRaw(renderer, bytes.data(), bytes.size(), static_cast<int>(page.width),
                              static_cast<int>(page.height), static_cast<int>(page.stride), PixmapFormat::BGRA);
        }
        m_pageImages.push_back(image.get());
        sheet->addChild(std::move(image));
        scroll->content()->addChild(std::move(sheet));
      }
      scroll->setOnScrollChanged([this](float) {
        requestVisiblePages();
        markCurrentPage();
      });
      m_scroll = scroll.get();
      split->addChild(std::move(scroll));
      return split;
    }
    case FileKind::Text: {
      if (const auto syntax = syntaxFor(m_path); syntax.has_value()) {
        m_code = std::make_unique<CodeLines>(m_textTruncated ? m_text + "\n…" : m_text, syntax->lineComment,
                                             syntax->blockComments);
        auto list = ui::virtualListView({.adapter = m_code.get(), .flexGrow = 1.0F});
        // Mockup §15 code card: the lines sit on a fill_1 panel, radius 8.
        auto box = ui::column({.align = FlexAlign::Stretch, .padding = Tokens::spaceP12,
                               .fill = colorSpecFromRole(ColorRole::Fill1), .radius = Tokens::radiusR8,
                               .clipChildren = true, .flexGrow = 1.0F});
        box->addChild(std::move(list));
        return box;
      }
      auto scroll = ui::scrollView({.viewportPaddingH = Style::spaceLg, .viewportPaddingV = Style::spaceMd});
      scroll->setFlexGrow(1.0F);
      std::string text = m_text;
      if (m_textTruncated) {
        text += "\n…";
      }
      scroll->content()->addChild(ui::label({.text = std::move(text), .fontSize = Style::fontSizeCaption,
                                             .fontFamily = kMonospace, .color = colorSpecFromRole(ColorRole::Label1),
                                             .maxWidth = width - 2 * Style::spaceLg}));
      m_scroll = scroll.get();
      return scroll;
    }
    case FileKind::Folder: {
      // Mockup §15 folder card: "12 items · 48 MB", then the newest items as their icons.
      auto box = ui::column({.align = FlexAlign::Start, .gap = Tokens::spaceP12, .padding = Tokens::spaceP24,
                             .flexGrow = 1.0F});
      box->addChild(ui::label({.text = std::format("{} {} · {}{}", m_folderItems, m_folderItems == 1 ? "item" : "items",
                                                   m_folderCut ? "at least " : "", formatFileSize(m_folderBytes)),
                               .fontSize = Style::fontSizeBody, .color = colorSpecFromRole(ColorRole::Label2)}));
      auto icons = ui::row({.align = FlexAlign::Start, .gap = Tokens::spaceP12});
      for (const std::string& item : m_folderPeek) {
        auto icon = std::make_unique<FileIcon>(Tokens::componentFilesIconMin);
        if (const auto info = statFile(std::filesystem::path(item)); info.has_value()) {
          icon->bind(renderer, *info, item, nullptr);
        }
        icons->addChild(std::move(icon));
      }
      box->addChild(std::move(icons));
      return box;
    }
    case FileKind::Font: {
      // Mockup §15 font card: "Aa Bb Cc", the pangram and the digits, drawn by the helper in label_1.
      auto box = ui::column({.align = FlexAlign::Start, .padding = Tokens::spaceP24, .flexGrow = 1.0F});
      if (m_fontPage.pixels != nullptr) {
        const float scale = m_surface->effectiveBufferScale();
        auto image = ui::image({.fit = ImageFit::Contain, .width = static_cast<float>(m_fontPage.width) / scale,
                                .height = static_cast<float>(m_fontPage.height) / scale});
        const auto bytes = m_fontPage.pixels->bytes();
        image->setSourceRaw(renderer, bytes.data(), bytes.size(), static_cast<int>(m_fontPage.width),
                            static_cast<int>(m_fontPage.height), static_cast<int>(m_fontPage.stride), PixmapFormat::BGRA);
        box->addChild(std::move(image));
      }
      return box;
    }
    case FileKind::Archive: {
      // Spec §10: the daemon's entry list, name and size; folders first as the archive orders them.
      auto scroll = ui::scrollView({.viewportPaddingH = Tokens::spaceP8, .viewportPaddingV = Tokens::spaceP8});
      scroll->setFlexGrow(1.0F);
      scroll->content()->setAlign(FlexAlign::Stretch);
      for (const ArchiveRow& entry : m_archive) {
        auto row = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8, .paddingH = Tokens::spaceP8,
                            .height = Tokens::sizeControlS});
        row->addChild(ui::glyph({.glyph = entry.isDir ? "folder" : "file", .glyphSize = Tokens::sizeSymbol,
                                 .color = colorSpecFromRole(ColorRole::Label2)}));
        row->addChild(ui::label({.text = entry.path, .fontSize = Style::fontSizeBody,
                                 .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                                 .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
        if (!entry.isDir) {
          row->addChild(ui::label({.text = formatFileSize(static_cast<std::uint64_t>(std::max<std::int64_t>(entry.size, 0))),
                                   .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::Label2)}));
        }
        scroll->content()->addChild(std::move(row));
      }
      if (m_archiveTotal > m_archive.size()) {
        scroll->content()->addChild(ui::label({.text = std::format("and {} more", m_archiveTotal - m_archive.size()),
                                               .fontSize = Style::fontSizeCaption,
                                               .color = colorSpecFromRole(ColorRole::Label2)}));
      } else if (m_archive.empty()) {
        scroll->content()->addChild(ui::label({.text = std::string("This archive is empty"),
                                               .fontSize = Style::fontSizeCaption,
                                               .color = colorSpecFromRole(ColorRole::Label2)}));
      }
      m_scroll = scroll.get();
      return scroll;
    }
    case FileKind::Video:
    case FileKind::Audio:
      if (m_audioOnly) {
        return buildAudio(renderer, width);
      }
      if (m_video) {
        // The video subsurface covers this box; the window draws only the letterbox.
        auto column = ui::column({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
        auto box = ui::column({.flexGrow = 1.0F});
        m_videoBox = box.get();
        const Color sheet = colorForRole(ColorRole::Popover);
        for (Node*& bar : m_letterbox) {
          auto fill = ui::column({.fill = colorSpecFromRole(ColorRole::Popover), .participatesInLayout = false});
          bar = fill.get();
          box->addChild(std::move(fill));
        }
        for (std::size_t i = 0; i < m_videoCorners.size(); ++i) {
          auto corner = std::make_unique<ScreenCorner>();
          corner->setColor(sheet);
          corner->setCorner(static_cast<ScreenCornerPosition>(i)); // TopLeft, TopRight, BottomRight, BottomLeft
          corner->setExponent(2.0F);                                // a circular arc, as every r12 shape
          corner->setParticipatesInLayout(false);
          m_videoCorners[i] = corner.get();
          box->addChild(std::move(corner));
        }
        column->addChild(std::move(box));
        // The bar sits on the window colour: under it is the hole's edge, not the picture.
        auto bar = ui::column({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Popover)});
        bar->setPadding(Tokens::spaceP8, 0.0F, 0.0F, 0.0F);
        bar->addChild(buildMediaBar());
        column->addChild(std::move(bar));
        return column;
      }
      return message("Video");
    default:
      return state("No preview for “" + baseName(m_path) + "”");
    }
  }

  void PreviewWindow::openInDefaultApp() {
    GFile* file = g_file_new_for_path(m_path.c_str());
    char* uri = g_file_get_uri(file);
    GError* error = nullptr;
    if (!g_app_info_launch_default_for_uri(uri, nullptr, &error)) {
      kLog.warn("cannot open {}: {}", m_path, error != nullptr ? error->message : "unknown");
      g_clear_error(&error);
    }
    g_free(uri);
    g_object_unref(file);
    DeferredCall::callLater([this] { close(); });
  }

  void PreviewWindow::onKeyboard(const KeyboardEvent& event) {
    if (!m_surface || !event.pressed || m_loop.wayland().lastKeyboardSurface() != m_surface->wlSurface()) {
      return;
    }
    switch (event.sym) {
    case XKB_KEY_Escape:
    case XKB_KEY_space:
      DeferredCall::callLater([this] { close(); });
      break;
    case XKB_KEY_k:
      if (m_video || m_audio) {
        togglePlayback();
      }
      break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      openInDefaultApp();
      break;
    case XKB_KEY_Left:
      m_callbacks.selection(direction::Left);
      break;
    case XKB_KEY_Right:
      m_callbacks.selection(direction::Right);
      break;
    case XKB_KEY_Up:
      m_callbacks.selection(direction::Up);
      break;
    case XKB_KEY_Down:
      m_callbacks.selection(direction::Down);
      break;
    default:
      break;
    }
  }

  void PreviewWindow::onPointer(const PointerEvent& event) {
    if (!m_surface) {
      return;
    }
    const bool onThis = event.surface == m_surface->wlSurface();
    const auto x = static_cast<float>(event.sx);
    const auto y = static_cast<float>(event.sy);
    switch (event.type) {
    case PointerEvent::Type::Enter:
      if (onThis) {
        m_pointerInside = true;
        m_input.pointerEnter(x, y, event.serial);
      }
      break;
    case PointerEvent::Type::Leave:
      if (onThis) {
        m_pointerInside = false;
        m_input.pointerLeave();
      }
      break;
    case PointerEvent::Type::Motion:
      if (m_pointerInside) {
        m_input.pointerMotion(x, y, event.serial);
      }
      break;
    case PointerEvent::Type::Button:
      if (m_pointerInside) {
        m_input.pointerMotion(x, y, event.serial);
        if (event.pressed && event.button == BTN_LEFT && m_video && m_video->contains(x, y)) {
          togglePlayback();
        } else if (event.pressed && event.button == BTN_LEFT && y < kHeaderHeight && m_input.hoveredArea() == nullptr) {
          m_surface->beginMove(event.serial);
        } else {
          m_input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
        }
      }
      break;
    case PointerEvent::Type::Axis:
      if (m_pointerInside) {
        m_input.pointerAxis(x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete,
                            event.axisValue120, event.axisLines);
      }
      break;
    }
    m_surface->requestRedraw();
  }

} // namespace preview
