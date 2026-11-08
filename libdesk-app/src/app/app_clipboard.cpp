#include "app/app_clipboard.h"

#include "wayland/drag_and_drop.h"
#include "wayland/primary_selection.h"
#include "wayland/wayland_connection.h"

#include "core/log.h"

#include <vector>

namespace {
  constexpr std::size_t kMaxTextBytes = 1U << 20; // text fields paste up to 1 MiB; a bigger clipboard is not read ahead
  constexpr Logger kLog("app-clipboard");
  const std::vector<std::string> kTextTypes{"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"};
}

AppClipboard::AppClipboard(WaylandConnection& wayland) : m_wayland(wayland) {
  if (DragAndDrop* dnd = m_wayland.dragAndDrop()) {
    dnd->setSelectionChanged([this] { refresh(); });
  }
  (void)m_wayland.primarySelection(); // its device must exist to hear selections before the first middle click
}

AppClipboard::~AppClipboard() {
  if (DragAndDrop* dnd = m_wayland.dragAndDrop()) {
    dnd->setSelectionChanged({});
  }
}

void AppClipboard::refresh() {
  DragAndDrop* dnd = m_wayland.dragAndDrop();
  const unsigned generation = ++m_generation;
  dnd->readSelection(
      kTextTypes,
      [this, generation, alive = std::weak_ptr<bool>(m_alive)](const std::string& mime, std::string bytes) {
        if (alive.expired() || generation != m_generation) {
          return; // gone, or a newer clipboard arrived while this one was read
        }
        m_text = mime.empty() ? std::nullopt : std::optional(std::move(bytes));
        kLog.debug("clipboard text {}", m_text.has_value() ? std::to_string(m_text->size()) + " bytes" : "none");
      },
      kMaxTextBytes);
}

void AppClipboard::setClipboardText(std::string text) {
  DragAndDrop* dnd = m_wayland.dragAndDrop();
  if (dnd == nullptr) {
    return;
  }
  m_text = text;
  ++m_generation;
  dnd->setSelection(m_wayland.lastInputSerial(),
                    {.mimeTypes = kTextTypes, .data = [text = std::move(text)](const std::string&) { return text; }});
}

std::optional<std::string> AppClipboard::primaryText() {
  PrimarySelection* primary = m_wayland.primarySelection();
  return primary != nullptr ? primary->text() : std::nullopt;
}

void AppClipboard::setPrimaryText(std::string text) {
  if (PrimarySelection* primary = m_wayland.primarySelection()) {
    primary->setText(m_wayland.lastInputSerial(), std::move(text));
  }
}

void AppClipboard::requestPrimaryText(std::function<void(std::optional<std::string>)> done) {
  if (PrimarySelection* primary = m_wayland.primarySelection()) {
    primary->whenText(std::move(done));
  } else {
    done(std::nullopt);
  }
}
