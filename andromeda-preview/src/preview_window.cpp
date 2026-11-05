#include "preview_window.h"

#include "app/app_loop.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "foreign_parent.h"
#include "preview_service.h"
#include "render/core/texture_manager.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace preview {

  namespace {
    constexpr Logger kLog("preview");
    constexpr float kHeaderHeight = 40.0F;
    constexpr float kDocumentWidth = 720.0F;
    constexpr float kPageGap = 12.0F;
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

    std::string baseName(const std::string& path) {
      const auto slash = path.rfind('/');
      return slash == std::string::npos ? path : path.substr(slash + 1);
    }
  } // namespace

  PreviewWindow::PreviewWindow(AppLoop& loop, ForeignParent& parent, Callbacks callbacks)
      : m_loop(loop), m_parent(parent), m_callbacks(std::move(callbacks)) {
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
    m_image = {};
    m_pages.clear();
    m_text.clear();
    m_textTruncated = false;
    m_failed = false;
    m_loading = true;
    m_dirty = true;
    const auto generation = m_generation;

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
      loadPdf();
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
    case FileKind::Video:
      loadVideo(HelperKind::Media);
      break;
    default:
      // Audio arrives in S7d; everything else shows the file card.
      m_loading = false;
      break;
    }
  }

  void PreviewWindow::loadPdf() {
    m_helper = HelperClient::start(HelperKind::Pdf, m_path);
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
    if (m_helper) {
      m_loop.removeSource(m_helper.get());
      m_helper.reset();
    }
  }

  void PreviewWindow::loadVideo(HelperKind kind) {
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
      if (reply.msg.op != op::InfoReply || (reply.msg.e & kFlagVideo) == 0 || !m_surface) {
        helperFailed();
        return;
      }
      m_video = std::make_unique<VideoView>(
          m_loop.wayland(), *m_helper,
          VideoView::Callbacks{
              .firstFrame = [](std::uint32_t w, std::uint32_t h) { kLog.info("shown video {}x{}", w, h); },
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
                        loadVideo(HelperKind::MediaSoftware);
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
      if (!m_video->attach(m_surface->wlSurface())) {
        helperFailed();
        return;
      }
      m_loading = false;
      invalidate();
    });
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
        m_pageImages[index]->clear(renderer);
        continue;
      }
      const auto bytes = page.pixels->bytes();
      m_pageImages[index]->setSourceRaw(renderer, bytes.data(), bytes.size(), static_cast<int>(page.width),
                                        static_cast<int>(page.height), static_cast<int>(page.stride),
                                        PixmapFormat::BGRA);
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
    case FileKind::Pdf:
      size = {kDocumentWidth + 2 * Style::spaceLg, 760.0F};
      break;
    case FileKind::Text:
      size = {720.0F, 560.0F};
      break;
    case FileKind::Video:
      size = {854.0F, 480.0F + kHeaderHeight + 1.0F};
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
    }
    // Logged once the decoded content is on screen (the E2E waits for it before its screenshot).
    if (!m_shownNote.empty()) {
      m_surface->callOnNextPresent([note = std::move(m_shownNote)] { kLog.info("{}", note); });
      m_shownNote.clear();
    }
  }

  void PreviewWindow::buildScene(Renderer& renderer, float width, float height) {
    const float offset = m_scroll != nullptr ? m_scroll->scrollOffset() : 0.0F;
    m_input.setSceneRoot(nullptr);
    m_scroll = nullptr;
    m_videoBox = nullptr;
    m_pageImages.clear();
    m_pendingUploads.clear();
    auto root = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
    root->setFill(colorSpecFromRole(ColorRole::SolidRegular));

    auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingH = Style::spaceMd,
                           .height = kHeaderHeight});
    header->addChild(ui::label({.text = baseName(m_path), .fontSize = Style::fontSizeBody,
                                .color = colorSpecFromRole(ColorRole::Label1), .maxLines = 1,
                                .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
    header->addChild(ui::button({.text = "Open", .onClick = [this] { openInDefaultApp(); }}));
    root->addChild(std::move(header));
    root->addChild(ui::separator({}));
    root->addChild(buildBody(renderer, width));

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
    if (m_loading) {
      return message("Loading…");
    }
    if (m_failed) {
      return message("This file cannot be previewed");
    }
    switch (m_kind) {
    case FileKind::Image: {
      auto image = ui::image({.fit = ImageFit::Contain, .padding = Style::spaceLg, .flexGrow = 1.0F});
      image->setSourceRaw(renderer, m_image.bytes.data(), m_image.bytes.size(), static_cast<int>(m_image.width),
                          static_cast<int>(m_image.height), static_cast<int>(m_image.stride), PixmapFormat::RGBA);
      return image;
    }
    case FileKind::Pdf: {
      auto scroll = ui::scrollView({.viewportPaddingH = Style::spaceLg, .viewportPaddingV = Style::spaceLg});
      scroll->setFlexGrow(1.0F);
      scroll->content()->setGap(kPageGap);
      scroll->content()->setAlign(FlexAlign::Center);
      const float pageWidth = std::min(kDocumentWidth, width - 2 * Style::spaceLg);
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
      scroll->setOnScrollChanged([this](float) { requestVisiblePages(); });
      m_scroll = scroll.get();
      return scroll;
    }
    case FileKind::Text: {
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
    case FileKind::Video:
      if (m_video) {
        // The video subsurface covers this box; the window draws only the letterbox.
        auto box = ui::column({.flexGrow = 1.0F});
        m_videoBox = box.get();
        return box;
      }
      return message("Video");
    default:
      return message(m_contentType.empty() ? "File" : std::string(g_content_type_get_description(m_contentType.c_str())));
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
      if (m_video) {
        m_video->togglePlaying();
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
          m_video->togglePlaying();
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
