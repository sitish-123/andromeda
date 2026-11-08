#include "viewer_window.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "core/text_clipboard.h"
#include "ui/controls/input.h"
#include "core/deferred_call.h"
#include "core/files/directory_scanner.h"
#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <format>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace viewer {

  namespace {
    constexpr Logger kLog("viewer");
    constexpr float kToolbarHeight = 44.0F;
    constexpr float kSidebarWidth = 148.0F;
    constexpr float kThumbWidth = 104.0F;
    constexpr float kPagePadding = 24.0F;
    constexpr std::uint32_t kMaxRenderWidth = 8192;
    constexpr std::uint32_t kMaxPageHeight = 16384;
    constexpr std::uint32_t kImageBox = 8192;
    // proto MAX_PAYLOAD: a longer request ends the helper.
    constexpr std::size_t kMaxQuery = 256;

    std::string contentTypeOf(const std::filesystem::path& path) {
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

    // Cuts at a UTF-8 character boundary.
    std::string capQuery(std::string query) {
      if (query.size() <= kMaxQuery) {
        return query;
      }
      std::size_t end = kMaxQuery;
      while (end > 0 && (static_cast<unsigned char>(query[end]) & 0xC0U) == 0x80U) {
        --end;
      }
      query.resize(end);
      return query;
    }

    std::vector<RectF> readRects(const std::uint8_t* bytes, std::size_t count) {
      std::vector<RectF> rects(count);
      std::memcpy(rects.data(), bytes, count * sizeof(RectF));
      return rects;
    }

    std::vector<std::uint8_t> areaPayload(float x0, float y0, float x1, float y1) {
      std::vector<std::uint8_t> out(16);
      const float v[4] = {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
      std::memcpy(out.data(), v, sizeof(v));
      return out;
    }
  } // namespace

  ViewerWindow::ViewerWindow(AppLoop& loop, AppWindow& window) : m_loop(loop), m_window(window) {
    m_window.setContentBuilder([this](Renderer& renderer, float width, float height) {
      return build(renderer, width, height);
    });
    m_window.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    // A rebuilt ScrollView clamps its offset to its content, which is empty until the first layout: restore after it.
    m_window.setLayoutCallback([this] {
      // Once per rebuild: a document shorter than the old offset clamps, and must not loop.
      if (m_restoreScroll && m_scroll != nullptr) {
        m_restoreScroll = false;
        if (m_scroll->scrollOffset() != m_scrollOffset) {
          m_scroll->setScrollOffset(m_scrollOffset);
          m_window.invalidate();
        }
      }
    });
  }

  ViewerWindow::~ViewerWindow() {
    if (m_unsaved) {
      kLog.info("unsaved edits dropped: {}", m_path.string());
    }
    m_decoder.cancel();
    dropHelper();
  }

  void ViewerWindow::dropHelper() {
    if (m_helper) {
      m_loop.removeSource(m_helper.get());
      m_helper.reset();
    }
  }

  void ViewerWindow::open(const std::filesystem::path& path) {
    ++m_generation;
    m_decoder.cancel();
    dropHelper();
    m_edits.clear();
    m_unsaved = false;
    m_notePoint.reset();
    m_signing = false;
    m_drawing = false;
    m_path = path;
    m_pages.clear();
    m_image = {};
    m_selection.reset();
    m_searchText.clear();
    m_model.clearSearch();
    m_scrollOffset = 0.0F;
    m_error.clear();
    m_window.setTitle(path.filename().string());
    const auto kind = preview::kindForContentType(contentTypeOf(path));
    if (kind == preview::FileKind::Pdf) {
      openPdf();
    } else if (kind == preview::FileKind::Image) {
      loadFolder();
      openImage();
    } else {
      fail("This file type cannot be shown");
    }
    rebuild();
  }

  void ViewerWindow::fail(std::string reason) {
    m_mode = Mode::Error;
    m_error = std::move(reason);
    rebuild();
  }

  void ViewerWindow::openPdf() {
    m_mode = Mode::Loading;
    m_helper = preview::HelperClient::start(preview::HelperKind::Pdf, m_path.string());
    if (!m_helper) {
      fail("This file cannot be opened");
      return;
    }
    m_loop.addSource(m_helper.get());
    m_helper->request({.op = preview::op::Info}, [this, generation = m_generation](const preview::HelperReply& reply) {
      if (generation != m_generation) {
        return;
      }
      if (reply.msg.op != preview::op::InfoReply || reply.msg.b == 0) {
        DeferredCall::callLater([this, generation] {
          if (generation == m_generation) {
            dropHelper();
          }
        });
        fail("This file cannot be opened");
        return;
      }
      m_model.reset(std::min<std::uint32_t>(reply.msg.b, 100000), static_cast<float>(reply.msg.c) / 100.0F,
                    static_cast<float>(reply.msg.d) / 100.0F);
      m_pages.assign(m_model.pageCount(), {});
      m_mode = Mode::Pdf;
      kLog.info("opened pdf {} pages", m_model.pageCount());
      rebuild();
    });
  }

  void ViewerWindow::loadFolder() {
    m_folder.clear();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(m_path.parent_path(), ec)) {
      if (entry.is_regular_file(ec) && DirectoryScanner::isImagePath(entry.path())) {
        m_folder.push_back(entry.path());
      }
    }
    std::ranges::sort(m_folder);
    const auto it = std::ranges::find(m_folder, m_path);
    m_folderIndex = it != m_folder.end() ? static_cast<std::size_t>(it - m_folder.begin()) : 0;
  }

  void ViewerWindow::openImage() {
    m_mode = Mode::Loading;
    m_decoder.request(m_path.string(), kImageBox, kImageBox, [this, generation = m_generation](preview::Pixels pixels) {
      if (generation != m_generation) {
        return;
      }
      if (pixels.width == 0) {
        fail("This image cannot be opened");
        return;
      }
      m_image = std::move(pixels);
      m_mode = Mode::Image;
      kLog.info("opened image {}x{}", m_image.width, m_image.height);
      rebuild();
    });
  }

  void ViewerWindow::step(int direction) {
    if (m_mode == Mode::Image || (m_mode == Mode::Loading && !m_folder.empty() && !m_helper)) {
      if (m_folder.size() < 2) {
        return;
      }
      const auto n = static_cast<long>(m_folder.size());
      const long next = (static_cast<long>(m_folderIndex) + direction + n) % n;
      const auto path = m_folder[static_cast<std::size_t>(next)];
      ++m_generation;
      m_decoder.cancel();
      m_path = path;
      m_folderIndex = static_cast<std::size_t>(next);
      m_window.setTitle(path.filename().string());
      openImage();
      rebuild();
      return;
    }
    if (m_mode == Mode::Pdf && m_scroll != nullptr) {
      const std::size_t page = currentPage();
      const std::size_t target = direction < 0 ? (page == 0 ? 0 : page - 1) : std::min(page + 1, m_model.pageCount() - 1);
      m_scroll->setScrollOffset(m_model.pageBox(target).y0);
      onScroll();
    }
  }

  void ViewerWindow::rebuild() { m_window.requestRebuild(); }

  std::size_t ViewerWindow::currentPage() const {
    return m_scroll != nullptr ? m_model.pageAt(m_scroll->scrollOffset() + 1.0F) : 0;
  }

  void ViewerWindow::onScroll() {
    if (m_scroll != nullptr) {
      m_scrollOffset = m_scroll->scrollOffset();
    }
    requestVisiblePages();
  }

  void ViewerWindow::requestVisiblePages() {
    if (m_mode != Mode::Pdf || !m_helper || m_scroll == nullptr) {
      return;
    }
    const float top = m_scroll->scrollOffset();
    auto [first, last] = m_model.visiblePages(top, top + m_scroll->height() * 1.5F);
    const float scale = m_window.uiScale();
    // Keep a few rendered pages around the view; a long document must not hold every page in memory.
    constexpr std::size_t kKeep = 3;
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
      if ((i + kKeep < first || i > last + kKeep) && m_pages[i].pixels != nullptr) {
        m_pages[i] = {};
      }
    }
    for (std::size_t i = first; i <= last && i < m_pages.size(); ++i) {
      const RectF box = m_model.pageBox(i);
      const auto width = std::min(kMaxRenderWidth, static_cast<std::uint32_t>(std::lround((box.x1 - box.x0) * scale)));
      // Render again only when the zoom changed the width noticeably.
      if (m_pages[i].requestedWidth != 0 && std::abs(static_cast<float>(m_pages[i].requestedWidth) - static_cast<float>(width))
                                                < 0.1F * static_cast<float>(width)) {
        continue;
      }
      m_pages[i].requestedWidth = width;
      m_helper->request(
          {.op = preview::op::Render, .a = static_cast<std::uint32_t>(i), .b = width, .c = kMaxPageHeight},
          [this, i, generation = m_generation](const preview::HelperReply& reply) {
            if (generation != m_generation || i >= m_pages.size()) {
              return;
            }
            const auto& m = reply.msg;
            // Untrusted sizes: rows must fit the stride and the mapping.
            const bool valid = m.op == preview::op::Page && reply.data != nullptr && m.b > 0 && m.c > 0
                && m.b <= kMaxRenderWidth && m.c <= kMaxPageHeight && m.b <= m.d / 4
                && static_cast<std::uint64_t>(m.d) * m.c <= reply.data->bytes().size();
            if (!valid) {
              m_pages[i].requestedWidth = 0;
              return;
            }
            m_pages[i] = {reply.data, m.b, m.c, m.d, m_pages[i].requestedWidth};
            if (i == 0) {
              kLog.info("shown pdf page 1");
            }
            rebuild();
          }
      );
    }
  }

  void ViewerWindow::search(const std::string& query) {
    m_searchText = capQuery(query);
    if (m_searchText.empty()) {
      m_model.clearSearch();
    } else {
      m_model.startSearch(m_searchText);
      // Its own counter: a new search drops the old search's finds, not the edits, renders or saves in flight.
      ++m_searchGeneration;
      // With a find of the old search in flight, its reply starts the new scan.
      scanNext();
    }
    rebuild();
  }

  void ViewerWindow::scanNext() {
    const auto page = m_model.nextPageToScan();
    if (!page || !m_helper || m_scanPending) {
      return;
    }
    m_scanPending = true;
    const std::string& q = m_searchText;
    m_helper->request(
        {.op = preview::op::Find, .a = static_cast<std::uint32_t>(*page)},
        std::vector<std::uint8_t>(q.begin(), q.end()),
        [this, page = *page, generation = m_generation, search = m_searchGeneration](const preview::HelperReply& reply) {
          if (generation != m_generation) {
            return;
          }
          m_scanPending = false;
          if (search != m_searchGeneration) {
            scanNext();
            return;
          }
          std::vector<RectF> hits;
          if (reply.msg.op == preview::op::FindReply && reply.msg.b > 0 && reply.data != nullptr
              && reply.data->bytes().size() >= static_cast<std::size_t>(reply.msg.b) * sizeof(RectF)) {
            hits = readRects(reply.data->bytes().data(), reply.msg.b);
          }
          const bool first = m_model.hitCount() == 0 && !hits.empty();
          m_model.setHits(page, std::move(hits));
          if (first && m_model.nextHit(currentPage())) {
            showCurrentHit();
          }
          if (m_model.searchDone()) {
            kLog.info("search \"{}\" {}", m_searchText, m_model.countText());
          }
          scanNext();
          rebuild();
        }
    );
  }

  void ViewerWindow::showCurrentHit() {
    const auto hit = m_model.currentHit();
    if (!hit || m_scroll == nullptr) {
      return;
    }
    const RectF box = m_model.pageBox(hit->page);
    const RectF r = m_model.hits(hit->page)[hit->index];
    const float y = box.y0 + r.y0 * m_model.zoom();
    const float top = m_scroll->scrollOffset();
    if (y < top || y > top + m_scroll->height() - 40.0F) {
      m_scrollOffset = std::max(0.0F, y - m_scroll->height() / 3.0F);
      m_scroll->setScrollOffset(m_scrollOffset);
      requestVisiblePages();
    }
    kLog.info("shown hit page {}", hit->page + 1);
  }

  void ViewerWindow::moveHit(bool forward) {
    if (forward ? m_model.nextHit(currentPage()) : m_model.previousHit(currentPage())) {
      showCurrentHit();
      rebuild();
    }
  }

  void ViewerWindow::beginSelect(std::size_t page, float x, float y) {
    m_selection = Selection{.page = page, .x0 = x, .y0 = y, .x1 = x, .y1 = y};
    m_selecting = true;
  }

  void ViewerWindow::updateSelect(float x, float y) {
    if (!m_selection) {
      return;
    }
    m_selection->x1 = x;
    m_selection->y1 = y;
    requestSelect();
  }

  void ViewerWindow::requestSelect() {
    // One select in flight; a newer drag position waits for it and goes next.
    if (m_selectPending) {
      m_selectDirty = true;
      return;
    }
    if (!m_selection || !m_helper) {
      return;
    }
    m_selectPending = true;
    m_selectDirty = false;
    const Selection& s = *m_selection;
    m_helper->request(
        {.op = preview::op::Select, .a = static_cast<std::uint32_t>(s.page), .b = preview::kSelectGlyph},
        areaPayload(s.x0, s.y0, s.x1, s.y1),
        [this, generation = m_generation](const preview::HelperReply& reply) {
          if (generation != m_generation) {
            return;
          }
          m_selectPending = false;
          if (m_selection && reply.msg.op == preview::op::SelectReply) {
            const std::size_t count = reply.msg.b;
            const std::size_t length = reply.msg.a;
            const auto bytes = reply.data != nullptr ? reply.data->bytes() : std::span<const std::uint8_t>{};
            if (count * sizeof(RectF) + length <= bytes.size()) {
              m_selection->rects = readRects(bytes.data(), count);
              m_selection->text.assign(reinterpret_cast<const char*>(bytes.data()) + count * sizeof(RectF), length);
            } else {
              m_selection->rects.clear();
              m_selection->text.clear();
            }
            rebuild();
          }
          if (m_selectDirty) {
            requestSelect();
          } else if (m_highlightOnSelect && !m_selecting && m_selection) {
            m_highlightOnSelect = false;
            if (!m_selection->rects.empty()) {
              addAnnotation(m_selection->page, preview::kAnnotHighlight, 0xffd400, annot::highlightBody(m_selection->rects));
            }
            m_selection.reset();
          }
        }
    );
  }

  void ViewerWindow::copySelection() {
    TextClipboard* clipboard = Input::textClipboard();
    if (m_selection && !m_selection->text.empty() && clipboard != nullptr) {
      clipboard->setClipboardText(m_selection->text);
      kLog.info("copied {} bytes", m_selection->text.size());
    }
  }

  void ViewerWindow::searchFor(const std::string& query) {
    if (m_searchInput != nullptr) {
      m_searchInput->setValue(query);
    }
    search(query);
  }

  void ViewerWindow::focusSearch() {
    if (m_searchInput != nullptr) {
      m_window.input().setFocus(m_searchInput->inputArea());
    }
  }

  void ViewerWindow::zoomIn() { zoomTo([this] { m_model.zoomBy(1.25F); }); }

  void ViewerWindow::zoomOut() { zoomTo([this] { m_model.zoomBy(0.8F); }); }

  void ViewerWindow::zoomFit() { zoomTo([this] { m_model.fitWidth(); }); }

  void ViewerWindow::zoomTo(const std::function<void()>& apply) {
    // The pages re-render at the new zoom; the column then springs from the old size to the new one around the
    // middle of the view, and the scroll keeps that middle in place.
    const float before = m_model.zoom();
    apply();
    const float after = m_model.zoom();
    if (after <= 0.0F || before == after) {
      return;
    }
    const float f = after / before;
    if (m_scroll != nullptr) {
      const float half = m_scroll->height() / 2.0F;
      m_scrollOffset = std::max(0.0F, (m_scroll->scrollOffset() + half) * f - half);
    }
    m_zoomFrom = 1.0F / f;
    rebuild();
  }

  void ViewerWindow::toggleSidebar() {
    m_sidebar = !m_sidebar;
    rebuild();
  }

  bool ViewerWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed) {
      return false;
    }
    const bool shift = (event.modifiers & KeyMod::Shift) != 0;
    switch (event.sym) {
    case XKB_KEY_Left:
    case XKB_KEY_Page_Up:
      step(-1);
      return true;
    case XKB_KEY_Right:
    case XKB_KEY_Page_Down:
      step(1);
      return true;
    case XKB_KEY_Return:
      if (!m_searchText.empty()) {
        moveHit(!shift);
        return true;
      }
      return false;
    case XKB_KEY_Escape:
      if (m_signing || m_notePoint) {
        m_signing = false;
        m_pendingSign.reset();
        m_notePoint.reset();
        rebuild();
        return true;
      }
      if (!m_searchText.empty()) {
        if (m_searchInput != nullptr) {
          m_searchInput->setValue("");
        }
        search("");
        return true;
      }
      m_selection.reset();
      rebuild();
      return true;
    default:
      return false;
    }
  }

  std::unique_ptr<Node> ViewerWindow::build(Renderer& renderer, float width, float height) {
    const float s = m_window.uiScale();
    m_scroll = nullptr;
    m_searchInput = nullptr;
    auto root = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
    root->addChild(buildToolbar(width));
    root->addChild(ui::separator({}));
    switch (m_mode) {
    case Mode::Empty:
      root->addChild(buildMessage("Open a document or image", "or drop a file here"));
      break;
    case Mode::Loading:
      root->addChild(ui::node({.flexGrow = 1.0F}));
      break;
    case Mode::Error:
      root->addChild(buildMessage(m_path.filename().string(), m_error));
      break;
    case Mode::Image: {
      auto image = ui::image({.fit = ImageFit::Contain, .padding = Style::spaceLg * s, .flexGrow = 1.0F});
      image->setSourceRaw(renderer, m_image.bytes.data(), m_image.bytes.size(), static_cast<int>(m_image.width),
                          static_cast<int>(m_image.height), static_cast<int>(m_image.stride), PixmapFormat::RGBA);
      root->addChild(std::move(image));
      break;
    }
    case Mode::Pdf: {
      if (m_signing) {
        root->addChild(buildSignaturePad());
        break;
      }
      auto body = ui::row({.align = FlexAlign::Stretch, .flexGrow = 1.0F});
      if (m_sidebar) {
        body->addChild(buildSidebar(renderer));
        body->addChild(ui::separator({.orientation = SeparatorOrientation::VerticalRule}));
      }
      body->addChild(buildPages(renderer));
      root->addChild(std::move(body));
      break;
    }
    }
    if (m_mode == Mode::Pdf) {
      DeferredCall::callLater([this, generation = m_generation] {
        if (generation == m_generation) {
          requestVisiblePages();
        }
      });
    }
    return root;
  }

  std::unique_ptr<Node> ViewerWindow::buildToolbar(float width) {
    const float s = m_window.uiScale();
    auto bar = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s, .paddingH = Style::spaceMd * s,
                        .height = kToolbarHeight * s});
    if (m_mode == Mode::Pdf) {
      bar->addChild(ui::button({.glyph = std::string("layout-sidebar"), .variant = ButtonVariant::Ghost,
                                .tooltip = std::string("Sidebar"), .onClick = [this] { toggleSidebar(); }}));
    }
    bar->addChild(ui::label({.text = m_path.empty() ? std::string("Viewer") : m_path.filename().string(),
                             .fontSize = Style::fontSizeBody * s, .color = colorSpecFromRole(ColorRole::Label1),
                             .maxLines = 1, .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
    std::string position;
    if (m_mode == Mode::Pdf) {
      position = std::format("{} / {}", currentPage() + 1, m_model.pageCount());
    } else if (m_mode == Mode::Image && m_folder.size() > 1) {
      position = std::format("{} of {}", m_folderIndex + 1, m_folder.size());
    }
    if (!position.empty()) {
      bar->addChild(ui::label({.text = position, .fontSize = Style::fontSizeCaption * s,
                               .color = colorSpecFromRole(ColorRole::Label2)}));
    }
    if (m_mode == Mode::Pdf) {
      bar->addChild(ui::button({.glyph = std::string("minus"), .variant = ButtonVariant::Ghost,
                                .tooltip = std::string("Zoom Out"), .onClick = [this] { zoomOut(); }}));
      bar->addChild(ui::button({.text = std::format("{}%", std::lround(m_model.zoom() * 100.0F)),
                                .variant = ButtonVariant::Ghost, .tooltip = std::string("Fit Width"),
                                .onClick = [this] { zoomFit(); }}));
      bar->addChild(ui::button({.glyph = std::string("plus"), .variant = ButtonVariant::Ghost,
                                .tooltip = std::string("Zoom In"), .onClick = [this] { zoomIn(); }}));
      const auto tool = [&](Tool t, const char* glyph, const char* tip) {
        bar->addChild(ui::button({.glyph = std::string(glyph), .selected = m_tool == t,
                                  .variant = m_tool == t ? ButtonVariant::TabActive : ButtonVariant::Ghost,
                                  .tooltip = std::string(tip), .onClick = [this, t] { setTool(t); }}));
      };
      tool(Tool::Select, "cursor-text", "Select Text");
      tool(Tool::Highlight, "highlight", "Highlight");
      tool(Tool::Note, "note", "Note");
      tool(Tool::Draw, "pencil", "Draw");
      tool(Tool::Sign, "signature", "Sign");
      if (m_notePoint) {
        bar->addChild(ui::input({
            .placeholder = std::string("Note"),
            .width = std::min(200.0F * s, width / 4.0F),
            .onSubmit =
                [this](const std::string& text) {
                  if (m_notePoint && !text.empty()) {
                    addAnnotation(m_notePoint->first, preview::kAnnotNote, 0xffd400,
                                  annot::noteBody(m_notePoint->second, text));
                  }
                  m_notePoint.reset();
                  rebuild();
                },
        }));
      }
      auto field = ui::input({
          .value = m_searchText,
          .placeholder = std::string("Search"),
          .clearButtonEnabled = true,
          .width = std::min(240.0F * s, width / 3.0F),
          .onChange = [this](const std::string& text) { search(text); },
          .onSubmit = [this](const std::string&) { moveHit(true); },
      });
      m_searchInput = field.get();
      bar->addChild(std::move(field));
      const std::string count = m_model.countText();
      if (!count.empty()) {
        bar->addChild(ui::label({.text = count, .fontSize = Style::fontSizeCaption * s,
                                 .color = colorSpecFromRole(ColorRole::Label2)}));
      }
    }
    return bar;
  }

  std::unique_ptr<Node> ViewerWindow::buildPages(Renderer& renderer) {
    const float s = m_window.uiScale();
    auto scroll = ui::scrollView({.viewportPaddingH = kPagePadding * s, .viewportPaddingV = kPagePadding * s});
    scroll->setFlexGrow(1.0F);
    scroll->content()->setGap(DocumentModel::kPageGap);
    scroll->content()->setAlign(FlexAlign::Center);
    // Fit-width uses the area inside the padding; the sidebar and the window size change it.
    const float viewport = m_window.contentRect().width - (m_sidebar ? kSidebarWidth * s : 0.0F) - 2.0F * kPagePadding * s;
    m_model.setViewportWidth(std::max(100.0F, viewport));
    const float zoom = m_model.zoom();
    const auto current = m_model.currentHit();
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
      const RectF box = m_model.pageBox(i);
      const float w = box.x1 - box.x0;
      const float h = box.y1 - box.y0;
      auto sheet = ui::inputArea({.cursorShape = 9 /* text */, .width = w, .height = h});
      sheet->setClipChildren(true);
      auto paper = ui::box({.fill = colorSpecFromRole(ColorRole::Fill1), .width = w, .height = h});
      paper->setParticipatesInLayout(false);
      sheet->addChild(std::move(paper));
      if (const Page& page = m_pages[i]; page.pixels != nullptr) {
        auto image = ui::image({.fit = ImageFit::Contain, .width = w, .height = h});
        const auto bytes = page.pixels->bytes();
        image->setSourceRaw(renderer, bytes.data(), bytes.size(), static_cast<int>(page.width),
                            static_cast<int>(page.height), static_cast<int>(page.stride), PixmapFormat::BGRA);
        image->setParticipatesInLayout(false);
        sheet->addChild(std::move(image));
      }
      const auto mark = [&](const RectF& r, ColorRole role, float opacity) {
        auto box2 = ui::box({.fill = colorSpecFromRole(role), .radius = 2.0F, .width = (r.x1 - r.x0) * zoom,
                             .height = (r.y1 - r.y0) * zoom, .opacity = opacity});
        box2->setParticipatesInLayout(false);
        box2->setPosition(r.x0 * zoom, r.y0 * zoom);
        sheet->addChild(std::move(box2));
      };
      const auto& hits = m_model.hits(i);
      for (std::size_t k = 0; k < hits.size(); ++k) {
        const bool isCurrent = current && current->page == i && current->index == k;
        mark(hits[k], ColorRole::Accent, isCurrent ? 0.55F : 0.25F);
      }
      if (m_selection && m_selection->page == i) {
        for (const RectF& r : m_selection->rects) {
          mark(r, ColorRole::Accent, 0.35F);
        }
      }
      if (m_drawing && m_strokePage == i) {
        for (const annot::Point& p : m_stroke) {
          mark({p.x - 1.0F, p.y - 1.0F, p.x + 1.0F, p.y + 1.0F}, ColorRole::Accent, 1.0F);
        }
      }
      sheet->setOnPress([this, i, zoom](const InputArea::PointerData& d) {
        if (d.button == BTN_LEFT) {
          onPagePress(i, d.localX / zoom, d.localY / zoom, d.pressed);
        }
      });
      sheet->setOnMotion([this, zoom](const InputArea::PointerData& d) { onPageMotion(d.localX / zoom, d.localY / zoom); });
      scroll->content()->addChild(std::move(sheet));
    }
    scroll->setOnScrollChanged([this](float) {
      onScroll();
      m_window.invalidate();
    });
    m_scroll = scroll.get();
    m_restoreScroll = m_scrollOffset > 0.0F;
    if (m_zoomFrom != 1.0F) {
      Node* column = scroll->content();
      const AppWindow::Rect view = m_window.contentRect(); // the scroll view is not laid out yet
      column->setTransformOrigin(view.width / 2.0F, m_scrollOffset + view.height / 2.0F);
      column->setScale(m_zoomFrom);
      m_zoomAnim = m_window.animations().drive(
          m_zoomAnim, m_zoomFrom, 1.0F, Style::spring(Tokens::springMove),
          [this, column](float v) {
            if (m_scroll != nullptr && m_scroll->content() == column) {
              column->setScale(v);
              m_window.invalidate();
            }
          },
          [this] { m_zoomAnim = 0; }, nullptr);
      m_zoomFrom = 1.0F;
    }
    return scroll;
  }

  std::unique_ptr<Node> ViewerWindow::buildSidebar(Renderer& renderer) {
    const float s = m_window.uiScale();
    auto scroll = ui::scrollView({.viewportPaddingH = Style::spaceMd * s, .viewportPaddingV = Style::spaceMd * s});
    scroll->setSize(kSidebarWidth * s, 0.0F);
    scroll->content()->setGap(Style::spaceMd * s);
    scroll->content()->setAlign(FlexAlign::Center);
    const std::size_t current = currentPage();
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
      const RectF box = m_model.pageBox(i);
      const float w = kThumbWidth * s;
      const float h = w * (box.y1 - box.y0) / std::max(1.0F, box.x1 - box.x0);
      auto thumb = ui::inputArea({.width = w, .height = h, .onClick = [this, i](const InputArea::PointerData&) {
        if (m_scroll != nullptr) {
          m_scroll->setScrollOffset(m_model.pageBox(i).y0);
          onScroll();
          rebuild();
        }
      }});
      auto frame = ui::box({.fill = colorSpecFromRole(ColorRole::Fill1),
                            .border = colorSpecFromRole(i == current ? ColorRole::Accent : ColorRole::Separator),
                            .borderWidth = i == current ? 2.0F * s : 1.0F, .width = w, .height = h});
      frame->setParticipatesInLayout(false);
      thumb->addChild(std::move(frame));
      if (const Page& page = m_pages[i]; page.pixels != nullptr) {
        auto image = ui::image({.fit = ImageFit::Contain, .width = w, .height = h});
        const auto bytes = page.pixels->bytes();
        image->setSourceRaw(renderer, bytes.data(), bytes.size(), static_cast<int>(page.width),
                            static_cast<int>(page.height), static_cast<int>(page.stride), PixmapFormat::BGRA, true);
        image->setParticipatesInLayout(false);
        thumb->addChild(std::move(image));
      }
      scroll->content()->addChild(std::move(thumb));
    }
    return scroll;
  }

  void ViewerWindow::setTool(Tool tool) {
    m_tool = tool;
    m_selection.reset();
    m_notePoint.reset();
    rebuild();
  }

  void ViewerWindow::onPagePress(std::size_t page, float x, float y, bool pressed) {
    switch (m_tool) {
    case Tool::Select:
    case Tool::Highlight:
      if (pressed) {
        beginSelect(page, x, y);
      } else if (m_selecting) {
        m_selecting = false;
        m_highlightOnSelect = m_tool == Tool::Highlight;
        updateSelect(x, y);
      }
      break;
    case Tool::Note:
      if (pressed) {
        m_notePoint = {page, {x, y}};
        rebuild();
      }
      break;
    case Tool::Draw:
      if (pressed) {
        m_drawing = true;
        m_strokePage = page;
        m_stroke = {{x, y}};
      } else if (m_drawing) {
        m_drawing = false;
        if (m_stroke.size() > 1) {
          addAnnotation(m_strokePage, preview::kAnnotInk, 0x1f5fd6, annot::inkBody({m_stroke}));
        }
        m_stroke.clear();
        rebuild();
      }
      break;
    case Tool::Sign:
      if (pressed) {
        placeSignature(page, x, y);
      }
      break;
    }
  }

  void ViewerWindow::onPageMotion(float x, float y) {
    if (m_tool == Tool::Draw && m_drawing) {
      // Points closer than half a point add nothing to the stroke.
      if (const auto& last = m_stroke.back(); std::hypot(x - last.x, y - last.y) >= 0.5F && m_stroke.size() < 8192) {
        m_stroke.push_back({x, y});
        rebuild();
      }
    } else if (m_selecting) {
      updateSelect(x, y);
    }
  }

  void ViewerWindow::placeSignature(std::size_t page, float x, float y) {
    const auto sig = annot::loadSignature();
    if (!sig) {
      m_pendingSign = {page, {x, y}};
      m_padStrokes.clear();
      m_signing = true;
      rebuild();
      return;
    }
    // 120 pt wide, the image's aspect, centred on the click.
    const float w = 120.0F;
    const float h = w * static_cast<float>(sig->height) / static_cast<float>(sig->width);
    addAnnotation(page, preview::kAnnotStamp, 0,
                  annot::stampBody({x - w / 2.0F, y - h / 2.0F, x + w / 2.0F, y + h / 2.0F}, sig->width, sig->height,
                                   sig->pixels));
  }

  void ViewerWindow::addAnnotation(std::size_t page, std::uint32_t kind, std::uint32_t rgb,
                                   const std::vector<std::uint8_t>& body) {
    if (!m_helper) {
      return;
    }
    const int fd = annot::sealedMemfd(body);
    if (fd < 0) {
      kLog.warn("cannot pass the annotation to the helper");
      return;
    }
    m_helper->request(
        {.op = preview::op::AnnotAdd, .a = static_cast<std::uint32_t>(page), .b = kind, .c = rgb}, fd,
        [this, page, kind, generation = m_generation](const preview::HelperReply& reply) {
          if (generation != m_generation) {
            return;
          }
          if (reply.msg.op != preview::op::AnnotReply) {
            kLog.warn("annotation {} on page {} refused", kind, page + 1);
            return;
          }
          m_edits.push_back(reply.msg.b);
          m_unsaved = true;
          kLog.info("annotated page {} kind {} ({} edits)", page + 1, kind, m_edits.size());
          updateTitle();
          rerenderPage(page);
        }
    );
    close(fd);
  }

  void ViewerWindow::rerenderPage(std::size_t page) {
    if (page < m_pages.size()) {
      m_pages[page].requestedWidth = 0;
    }
    requestVisiblePages();
  }

  void ViewerWindow::undo() {
    if (m_edits.empty() || !m_helper) {
      return;
    }
    const std::uint32_t id = m_edits.back();
    m_edits.pop_back();
    m_helper->request({.op = preview::op::AnnotRemove, .b = id},
                      [this, generation = m_generation](const preview::HelperReply& reply) {
                        if (generation != m_generation || reply.msg.op != preview::op::AnnotReply) {
                          return;
                        }
                        m_unsaved = !m_edits.empty();
                        updateTitle();
                        rerenderPage(reply.msg.a);
                      });
  }

  void ViewerWindow::save() {
    if (!m_unsaved || m_saving || !m_helper) {
      return;
    }
    // A temp file next to the original, renamed over it once complete: a failed save leaves the original intact.
    // The real file behind a symlink is the one replaced; the link stays a link.
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::canonical(m_path, ec);
    struct stat st{};
    if (ec || stat(target.c_str(), &st) != 0) {
      kLog.warn("cannot save {}: {}", m_path.string(), ec ? ec.message() : std::strerror(errno));
      return;
    }
    std::string tmp = (target.parent_path() / ("." + target.filename().string() + ".XXXXXX")).string();
    const int fd = mkostemp(tmp.data(), O_CLOEXEC);
    if (fd < 0) {
      kLog.warn("cannot save {}: {}", m_path.string(), std::strerror(errno));
      return;
    }
    // Same mode and owner as the original (mkostemp makes 0600); the owner change works only for our own files.
    (void)fchmod(fd, st.st_mode & 07777);
    (void)fchown(fd, st.st_uid, st.st_gid);
    // poppler closes the fd it gets; this copy is for the fsync once it reports done.
    const int keep = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    m_saving = true;
    m_helper->request(
        {.op = preview::op::Save}, fd,
        [this, tmp, target, keep, generation = m_generation](const preview::HelperReply& reply) {
          std::error_code err;
          const bool written = reply.msg.op == preview::op::Saved && keep >= 0 && fsync(keep) == 0;
          if (keep >= 0) {
            close(keep);
          }
          if (!written) {
            std::filesystem::remove(tmp, err);
          } else {
            std::filesystem::rename(tmp, target, err);
            if (err) {
              std::filesystem::remove(tmp, err);
            } else if (const int dir = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dir >= 0) {
              // The rename itself reaches the disk with the directory.
              fsync(dir);
              close(dir);
            }
          }
          if (generation != m_generation) {
            return;
          }
          m_saving = false;
          if (!written || err) {
            kLog.warn("saving {} failed", m_path.string());
            return;
          }
          m_unsaved = false;
          kLog.info("saved {}", m_path.string());
          updateTitle();
        }
    );
    close(fd);
  }

  void ViewerWindow::updateTitle() {
    m_window.setTitle((m_unsaved ? "• " : "") + m_path.filename().string());
    rebuild();
  }

  std::unique_ptr<Node> ViewerWindow::buildSignaturePad() {
    const float s = m_window.uiScale();
    constexpr float kPadWidth = 480.0F;
    constexpr float kPadHeight = 200.0F;
    auto sheet = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceMd * s,
                             .flexGrow = 1.0F});
    sheet->addChild(ui::label({.text = std::string("Draw your signature"), .fontSize = Style::fontSizeBody * s,
                               .color = colorSpecFromRole(ColorRole::Label1)}));
    auto pad = ui::inputArea({.cursorShape = 2 /* crosshair */, .width = kPadWidth * s, .height = kPadHeight * s});
    pad->setClipChildren(true);
    auto paper = ui::box({.fill = colorSpecFromRole(ColorRole::Fill1), .border = colorSpecFromRole(ColorRole::Separator),
                          .borderWidth = 1.0F, .radius = Style::scaledRadiusMd(s), .width = kPadWidth * s,
                          .height = kPadHeight * s});
    paper->setParticipatesInLayout(false);
    pad->addChild(std::move(paper));
    for (const auto& stroke : m_padStrokes) {
      for (const annot::Point& p : stroke) {
        auto dot = ui::box({.fill = colorSpecFromRole(ColorRole::Label1), .radius = 1.5F * s, .width = 3.0F * s,
                            .height = 3.0F * s});
        dot->setParticipatesInLayout(false);
        dot->setPosition((p.x - 1.5F) * s, (p.y - 1.5F) * s);
        pad->addChild(std::move(dot));
      }
    }
    pad->setOnPress([this, s](const InputArea::PointerData& d) {
      if (d.button != BTN_LEFT) {
        return;
      }
      m_padDrawing = d.pressed;
      if (d.pressed) {
        m_padStrokes.push_back({{d.localX / s, d.localY / s}});
        rebuild();
      }
    });
    pad->setOnMotion([this, s](const InputArea::PointerData& d) {
      if (m_padDrawing && !m_padStrokes.empty() && m_padStrokes.back().size() < 4096) {
        m_padStrokes.back().push_back({d.localX / s, d.localY / s});
        rebuild();
      }
    });
    sheet->addChild(std::move(pad));
    auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * s});
    buttons->addChild(ui::button({.text = std::string("Clear"), .variant = ButtonVariant::Ghost, .onClick = [this] {
      m_padStrokes.clear();
      rebuild();
    }}));
    buttons->addChild(ui::button({.text = std::string("Cancel"), .variant = ButtonVariant::Ghost, .onClick = [this] {
      m_signing = false;
      m_pendingSign.reset();
      rebuild();
    }}));
    buttons->addChild(ui::button({.text = std::string("Save"), .enabled = !m_padStrokes.empty(),
                                  .variant = ButtonVariant::Primary, .onClick = [this, kPadWidth, kPadHeight] {
      m_signing = false;
      if (annot::saveSignature(m_padStrokes, static_cast<std::uint32_t>(kPadWidth), static_cast<std::uint32_t>(kPadHeight))
          && m_pendingSign) {
        placeSignature(m_pendingSign->first, m_pendingSign->second.x, m_pendingSign->second.y);
      }
      m_pendingSign.reset();
      rebuild();
    }}));
    sheet->addChild(std::move(buttons));
    return sheet;
  }

  std::unique_ptr<Node> ViewerWindow::buildMessage(const std::string& title, const std::string& caption) {
    const float s = m_window.uiScale();
    auto box = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * s,
                           .flexGrow = 1.0F});
    box->addChild(ui::label({.text = title, .fontSize = Style::fontSizeTitle * s,
                             .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 2,
                             .textAlign = TextAlign::Center}));
    box->addChild(ui::label({.text = caption, .fontSize = Style::fontSizeCaption * s,
                             .color = colorSpecFromRole(ColorRole::Label2)}));
    return box;
  }

} // namespace viewer
