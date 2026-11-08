#pragma once

#include "shell/bar/widget.h"

class Box;
class Node;

// Show Desktop: a narrow strip at the bar's far end behind a hairline (Desktop Layouts, Windows); a click minimizes
// every window on the workspace and a second click brings them back. The click is the widget's default gesture.
class ShowDesktopWidget : public Widget {
public:
  struct Options {};

  ShowDesktopWidget(bool verticalBar, Options options);

  void create() override;
  bool noGapAroundMe() const noexcept override { return true; }

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  Node* m_hairline = nullptr;
  Box* m_hover = nullptr;
  bool m_verticalBar = false;
};
