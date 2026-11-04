#pragma once

#include "layout/layout.h"

namespace andromeda {
  class FloatingLayout final : public Layout {
  public:
    [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Floating; }
    [[nodiscard]] int columnOf(const View*) const override { return -1; }
    [[nodiscard]] int rowOf(const View*) const override { return -1; }
    [[nodiscard]] LayoutCapture captureState() const override;
    bool restoreState(const LayoutSnapshot& snapshot, std::span<const LayoutMember> members) override;
    void insertView(View*, int) override {}
    bool swapViews(View*, View*) override { return false; }
    void removeView(View*) override {}
    void arrange(const wlr_box&) override {}
    [[nodiscard]] wlr_box targetBox(const View*) const override;
    [[nodiscard]] InitialSize
    initialSize(const wlr_box&, bool, std::optional<double>, std::optional<int>, const View*) const override {
      return {};
    }
    [[nodiscard]] const std::vector<Column>& columns() const override;
  };
} // namespace andromeda
