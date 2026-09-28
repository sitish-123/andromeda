#pragma once

#include "shell/bar/widget.h"

#include <string>

class Glyph;
class Label;
class LinkService;

// A Link phone while one is connected: its battery glyph and percent, with the full status in the tooltip. Hidden
// while no phone is connected.
class PhoneWidget : public Widget {
public:
  explicit PhoneWidget(LinkService* link);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);

  LinkService* m_link = nullptr;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  // Never a real key, so the first sync always applies and hides the widget when no phone is connected.
  std::string m_lastKey = "\n";
};
