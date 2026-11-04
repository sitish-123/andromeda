#include "config/action_groups.h"

namespace andromeda {

  const char* groupTitle(Group group) {
    switch (group) {
    case Group::Apps:
      return "Apps";
    case Group::Screencasting:
      return "Screencasting";
    case Group::Focus:
      return "Focus";
    case Group::MoveSize:
      return "Move & size";
    case Group::Windows:
      return "Windows";
    case Group::Scratchpad:
      return "Scratchpad";
    case Group::Workspaces:
      return "Workspaces";
    case Group::Overview:
      return "Overview";
    case Group::System:
      return "System";
    default:
      return nullptr;
    }
  }

  Group groupForAction(KeybindAction action) {
    using A = KeybindAction;
    switch (action) {
    case A::Spawn:
      return Group::Apps;
    case A::ScreenCastClear:
    case A::ScreenCastSetOutput:
    case A::ScreenCastSetWindow:
    case A::ScreenCastFollowWindow:
    case A::ScreenCastFollowOutput:
    case A::ScreenCastFollowStop:
      return Group::Screencasting;
    case A::WindowFocusLeft:
    case A::WindowFocusRight:
    case A::WindowFocusOrOutputLeft:
    case A::WindowFocusOrOutputRight:
    case A::ColumnFocusFirst:
    case A::ColumnFocusLast:
    case A::ColumnFocusTab:
    case A::ColumnFocusTabNext:
    case A::ColumnFocusTabPrevious:
    case A::WindowFocusUp:
    case A::WindowFocusDown:
    case A::WindowFocusOrWorkspaceUp:
    case A::WindowFocusOrWorkspaceDown:
    case A::WindowFocusOrOutputUp:
    case A::WindowFocusOrOutputDown:
    case A::WindowFocusNext:
    case A::WindowFocusPrevious:
    case A::WindowFocusId:
    case A::WindowFocusWarpId:
    case A::WindowFocusSwitchFloating:
    case A::WindowFocusLast:
    case A::WorkspaceFocusLast:
      return Group::Focus;
    case A::ColumnMoveLeft:
    case A::ColumnMoveRight:
    case A::WindowMoveOrOutputLeft:
    case A::WindowMoveOrOutputRight:
    case A::ColumnMoveToFirst:
    case A::ColumnMoveToLast:
    case A::ColumnCenter:
    case A::WindowMoveUp:
    case A::WindowMoveDown:
    case A::WindowMoveLeft:
    case A::WindowMoveRight:
    case A::WindowMoveOrWorkspaceUp:
    case A::WindowMoveOrWorkspaceDown:
    case A::WindowMoveOrOutputUp:
    case A::WindowMoveOrOutputDown:
    case A::WindowConsumeFromLeft:
    case A::WindowConsumeFromRight:
    case A::WindowConsumeLeft:
    case A::WindowConsumeOrExpelLeft:
    case A::WindowConsumeRight:
    case A::WindowConsumeOrExpelRight:
    case A::WindowCyclePrimaryExtent:
    case A::WindowCyclePrimaryExtentBack:
    case A::WindowSetPrimaryExtent:
    case A::WindowModifyPrimaryExtent:
    case A::WindowModifyWidthLeft:
    case A::WindowModifyWidthRight:
    case A::WindowSetSecondaryExtent:
    case A::WindowModifySecondaryExtent:
    case A::WindowModifyHeightUp:
    case A::WindowModifyHeightDown:
    case A::WindowCycleSecondaryExtent:
    case A::WindowCycleSecondaryExtentBack:
    case A::WindowSnap:
    case A::WindowUnsnap:
    case A::WindowCenter:
    case A::WindowMoveTo:
    case A::WindowMoveBy:
    case A::LayoutScrollLeft:
    case A::LayoutScrollRight:
    case A::LayoutScrollUp:
    case A::LayoutScrollDown:
    case A::LayoutScrollDrag:
    case A::WindowMoveToOutputLeft:
    case A::WindowMoveToOutputRight:
    case A::WindowMoveToOutputUp:
    case A::WindowMoveToOutputDown:
    case A::WindowMoveToOutputNext:
    case A::WindowMoveToOutputPrevious:
    case A::ColumnMoveToOutputLeft:
    case A::ColumnMoveToOutputRight:
    case A::ColumnMoveToOutputUp:
    case A::ColumnMoveToOutputDown:
    case A::WindowSwapNext:
    case A::WindowSwapPrevious:
    case A::WindowSwapLeft:
    case A::WindowSwapRight:
    case A::WindowSwapUp:
    case A::WindowSwapDown:
    case A::LayoutMasterCountIncrease:
    case A::LayoutMasterCountDecrease:
    case A::LayoutToggleSplit:
      return Group::MoveSize;
    case A::WindowClose:
    case A::WindowKill:
    case A::WindowMinimize:
    case A::WindowRestoreMinimized:
    case A::ToggleFloating:
    case A::ToggleMaximize:
    case A::ToggleMaximizeToEdges:
    case A::ToggleFullscreen:
    case A::TogglePinned:
    case A::ColumnToggleTabbed:
    case A::ColumnSetDisplay:
    case A::ColumnMoveTabNext:
    case A::ColumnMoveTabPrevious:
    case A::ColumnToggleTabBar:
    case A::ColumnShowTabBar:
    case A::ColumnHideTabBar:
      return Group::Windows;
    case A::WindowMoveToScratchpad:
    case A::WindowRestoreFromScratchpad:
    case A::WindowToggleScratchpad:
    case A::ScratchpadToggle:
    case A::ScratchpadFocusNext:
    case A::ScratchpadWindowShowNext:
    case A::ScratchpadWindowShowPrevious:
      return Group::Scratchpad;
    case A::WorkspaceSwitch:
    case A::WorkspaceToggleShowDesktop:
    case A::ColumnMoveToWorkspace:
    case A::ColumnMoveToWorkspaceNext:
    case A::ColumnMoveToWorkspacePrevious:
    case A::WindowMoveToWorkspace:
    case A::WindowMoveToWorkspaceSilent:
    case A::WindowMoveToWorkspaceSilentNext:
    case A::WindowMoveToWorkspaceSilentPrevious:
    case A::WindowMoveToWorkspaceNext:
    case A::WindowMoveToWorkspacePrevious:
    case A::WorkspaceNext:
    case A::WorkspacePrevious:
    case A::WorkspaceMoveDown:
    case A::WorkspaceMoveUp:
    case A::WorkspaceSetLayout:
    case A::WorkspaceMoveToOutputLeft:
    case A::WorkspaceMoveToOutputRight:
    case A::WorkspaceMoveToOutputUp:
    case A::WorkspaceMoveToOutputDown:
    case A::WorkspaceSwapActiveOutputLeft:
    case A::WorkspaceSwapActiveOutputRight:
    case A::WorkspaceSwapActiveOutputUp:
    case A::WorkspaceSwapActiveOutputDown:
    case A::WorkspaceSwapActiveOutputNext:
    case A::WorkspaceSwapActiveOutputPrevious:
      return Group::Workspaces;
    case A::OutputFocusLeft:
    case A::OutputFocusRight:
    case A::OutputFocusUp:
    case A::OutputFocusDown:
    case A::OutputFocusNext:
    case A::OutputFocusPrevious:
      return Group::Focus;
    case A::OverviewToggle:
    case A::OverviewOpen:
    case A::OverviewClose:
      return Group::Overview;
    case A::ConfigReload:
    case A::CursorHide:
    case A::CursorShow:
    case A::DpmsOff:
    case A::DpmsOn:
    case A::OutputDisable:
    case A::OutputEnable:
    case A::OutputToggle:
    case A::OutputSdrWhite:
    case A::SessionQuit:
    case A::Submap:
    case A::KeyboardLayoutNext:
    case A::ShortcutsInhibitToggle:
      return Group::System;
    default:
      return Group::System;
    }
  }

  std::span<const Group> fixedGroupOrder() {
    static constexpr Group kFixedGroups[] = {
        Group::Apps,       Group::Screencasting, Group::Focus,    Group::MoveSize, Group::Windows,
        Group::Scratchpad, Group::Workspaces,    Group::Overview, Group::System,
    };
    return kFixedGroups;
  }

} // namespace andromeda
