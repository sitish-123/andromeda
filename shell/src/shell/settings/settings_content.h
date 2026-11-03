#pragma once

#include "shell/settings/settings_registry.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Flex;
class InputArea;
class Label;
class Node;
class ConfigService;
class SettingsControl;

namespace settings {

  // Pango line budget for setting descriptions: wrap up to this many lines, then ellipsize.
  inline constexpr int kSettingDescriptionMaxLines = 5;

  [[nodiscard]] std::unique_ptr<Label> makeSettingSubtitleLabel(std::string_view text, float scale);

  struct SearchPickerOpenRequest {
    std::string title;
    std::vector<SelectOption> options;
    std::string selectedValue;
    std::string placeholder;
    std::string emptyText;
    std::vector<std::string> settingPath;
    // When set, replaces the default "empty clears, otherwise set" commit for settingPath.
    // Needed where the chosen option is not itself the stored value.
    std::function<void(const std::string&)> onSelect;
  };

  enum class RowScope : std::uint8_t { Main, Expert };

  struct SettingsContentContext {
    const Config& config;
    ConfigService* configService = nullptr;
    const SettingsControl* compositorSettings = nullptr;
    float scale = 1.0F;
    std::string_view searchQuery;
    std::string_view selectedSection;
    const BarConfig* selectedBar = nullptr;
    std::string_view selectedMonitorOverride;
    // The page's own rows (Main) or the rows behind its last row (Expert).
    RowScope rowScope = RowScope::Main;
    // Filters the expert rows of the open page; empty shows all.
    std::string_view pageQuery;
    // Key of the row a search result opened; drawn highlighted and scrolled into view.
    std::string_view highlightKey;
    std::vector<SelectOption> batteryDeviceOptions;
    // logind CanHibernate=yes and a trial hibernate resumed; gates the Hibernate idle action.
    bool canHibernate = false;

    std::string& editingWidgetName;
    std::string& editingCapsuleGroupId;
    std::vector<std::string>& selectedLaneWidgets;
    std::string& pendingDeleteWidgetSettingPath;
    std::string& renamingWidgetName;
    std::string& pendingGestureKey;
    std::string& pendingGestureVerb;
    std::string& actionsExpandedFor;
    std::unordered_map<std::string, std::unordered_set<std::string>>& expandedGroupsByPage;
    // Bindable IPC commands for the gesture action picker: value = command, label = usage,
    // description = the command's --help text.
    std::vector<GestureActionOption> actionCatalog;

    // Opens the expert rows of the open page.
    std::function<void()> openExpert;
    // Opens the page a search result belongs to, with the row highlighted.
    std::function<void(const SettingEntry&)> openSearchResult;
    std::function<void()> requestRebuild;
    std::function<void()> requestContentRebuild;
    std::function<void()> resetContentScroll;
    std::function<void(Node*)> setScrollTarget;
    std::function<void(const Node&)> scrollContentToTop;
    std::function<void(InputArea*)> focusArea;
    std::function<void(const std::vector<std::string>&)> openBarWidgetAddPopup;
    std::function<void(SearchPickerOpenRequest request)> openSearchPickerPopup;
    std::function<void(std::vector<std::string>, ConfigOverrideValue)> setOverride;
    std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)> setOverrides;
    std::function<void(std::vector<std::string>)> clearOverride;
    std::function<void(std::vector<std::vector<std::string>>)> clearOverrides;
    std::function<void(std::vector<std::string>)> resetBarLane;
    std::function<bool(const std::vector<std::vector<std::string>>&)> isResetConfirmationPending;
    std::function<void(std::vector<std::vector<std::string>>)> requestResetConfirmation;
    std::function<void(std::string, std::string, std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)>
        renameWidgetInstance;

    std::function<void(std::size_t)> openSessionActionEntryEditor;
    std::function<void(std::size_t)> openIdleBehaviorEntryEditor;
    std::function<void()> openIdleBehaviorCreateEditor;
    std::function<void(std::size_t)> openNotificationFilterEntryEditor;
    std::function<void()> openNotificationFilterCreateEditor;
    std::function<void(std::vector<std::string> laneListPath, std::string widgetName)> openWidgetInspectorEditor;
    std::function<void(std::vector<std::string> laneListPath, std::string groupId)> openCapsuleGroupEditor;

    std::function<void(Label*)> registerIdleLiveStatusLabel;
    std::function<void(std::size_t, Label*)> registerSessionActionSummaryLabel;
    std::function<void(std::shared_ptr<std::vector<SessionPanelActionConfig>>)> bindSessionActionsEditState;

    // When set (session action entry popup), called after commits instead of requestRebuild.
    std::function<void()> afterSessionActionsCommit;
    std::function<void()> afterIdleBehaviorApply;
    std::function<void()> afterNotificationFilterApply;
    std::function<void()> closeHostedEditor;
    std::function<void(std::string, std::string, std::string, std::function<void()>)> confirmDelete;
    bool supportsTaskbarWorkspaceGrouping = true;
  };

  // Global search: a list of matching rows from every page, expert ones included; a result opens its page.
  std::size_t
  addSettingsSearchResults(Flex& content, const std::vector<SettingEntry>& registry, const SettingsContentContext& ctx);

  // True when the window adds this page's custom content after the generated rows; its expert row then comes last.
  [[nodiscard]] bool settingsPageAddsContentAfterRows(std::string_view selectedSection);
  // The page's last row: it opens the expert options. Adds nothing in expert scope or without expert rows.
  void addSettingsExpertRow(Flex& content, const std::vector<SettingEntry>& registry, const SettingsContentContext& ctx);

  std::size_t
  addSettingsContentSections(Flex& content, const std::vector<SettingEntry>& registry, SettingsContentContext ctx);

  void buildSessionActionEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, SessionPanelActionConfig& row, const std::function<void()>& persist
  );
  void buildIdleBehaviorEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, IdleBehaviorConfig& row, const std::function<void()>& persist
  );
  void buildNotificationFilterEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, NotificationFilterConfig& row, const std::function<void()>& persist
  );

} // namespace settings
