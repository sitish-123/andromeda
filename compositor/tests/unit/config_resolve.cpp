#include "check.h"
#include "config/resolve.h"

#include <algorithm>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using andromeda::Config;
using andromeda::ContentType;
using andromeda::LayerRule;
using andromeda::LayoutMode;
using andromeda::OutputIdentity;
using andromeda::OutputRule;
using andromeda::SecurityContextRule;
using andromeda::VrrMode;
using andromeda::WindowRule;
using andromeda::WorkspaceConfig;

namespace {
  constexpr OutputIdentity identity(
      std::string_view connector, std::string_view make = {}, std::string_view model = {}, std::string_view serial = {}
  ) {
    return {.connector = connector, .make = make, .model = model, .serial = serial};
  }
} // namespace

ANDROMEDA_TEST(globalLayoutUsesTheCallerOwnedConfig) {
  Config first;
  first.layout.gap = 11;
  first.appearance.borderWidth = 5;
  first.appearance.outerBorderWidth = 3;
  const andromeda::ResolvedLayoutConfig firstLayout = andromeda::resolveGlobalLayout(first);

  CHECK_EQ(firstLayout.gap, 11);
  CHECK_EQ(firstLayout.totalGap, 27);
  CHECK_EQ(firstLayout.edgePad, 19);
  CHECK_EQ(first.layoutGap(), firstLayout.totalGap);
  CHECK_EQ(first.layoutEdgePad(), firstLayout.edgePad);
  CHECK(!firstLayout.dwindle.preserveSplit);

  Config second;
  second.layout.gap = 4;
  second.appearance.borderWidth = 1;
  second.appearance.outerBorderWidth = 0;
  const andromeda::ResolvedLayoutConfig secondLayout = andromeda::resolveGlobalLayout(second);

  CHECK_EQ(secondLayout.totalGap, 6);
  CHECK_EQ(secondLayout.edgePad, 5);
  CHECK(firstLayout != secondLayout);
}

ANDROMEDA_TEST(workspaceOverridesApplyGlobalThenOutputSpecificRules) {
  Config config;
  config.layout.gap = 8;
  config.layout.struts = {.left = 1, .right = 2, .top = 3, .bottom = 4};
  config.appearance.borderWidth = 2;
  config.layout.master.position = andromeda::MasterPosition::Right;
  config.layout.master.defaultWidthFraction = 0.58;
  config.layout.master.newOnTop = false;
  config.layout.dwindle.preserveSplit = true;

  WorkspaceConfig global;
  global.name = "dev";
  global.layout.gap = 12;
  global.layout.struts.left = 10;
  global.layout.struts.top = 30;
  global.layout.scrolling.defaultExtentFraction = 0.6;
  global.layout.master.defaultWidthFraction = 0.6;
  global.layout.master.newOnTop = true;
  global.layout.dwindle.preserveSplit = false;
  config.workspaceRules.push_back(std::move(global));

  WorkspaceConfig dpOne;
  dpOne.name = "dev";
  dpOne.output = "DP-1";
  dpOne.layout.gap = 20;
  dpOne.layout.struts.right = 20;
  dpOne.layout.mode = LayoutMode::Dwindle;
  dpOne.layout.master.position = andromeda::MasterPosition::Left;
  dpOne.layout.master.defaultWidthFraction = 0.7;
  dpOne.layout.master.newOnTop = false;
  dpOne.layout.dwindle.preserveSplit = true;
  config.workspaceRules.push_back(std::move(dpOne));

  WorkspaceConfig dpTwo;
  dpTwo.name = "dev";
  dpTwo.output = "DP-2";
  dpTwo.layout.gap = 30;
  config.workspaceRules.push_back(std::move(dpTwo));

  const auto onDpOne = andromeda::resolveWorkspaceLayout(config, identity("DP-1"), "dev", 0);
  CHECK(onDpOne.mode == LayoutMode::Dwindle);
  CHECK_EQ(onDpOne.gap, 20);
  CHECK_EQ(onDpOne.totalGap, 24);
  CHECK_EQ(onDpOne.edgePad, 22);
  CHECK_EQ(onDpOne.struts.left, 10);
  CHECK_EQ(onDpOne.struts.right, 20);
  CHECK_EQ(onDpOne.struts.top, 30);
  CHECK_EQ(onDpOne.struts.bottom, 4);
  CHECK(onDpOne.scrolling.defaultExtentFraction.has_value());
  CHECK_EQ(*onDpOne.scrolling.defaultExtentFraction, 0.6);
  CHECK(onDpOne.master.position == andromeda::MasterPosition::Left);
  CHECK_EQ(onDpOne.master.defaultWidthFraction, 0.7);
  CHECK(!onDpOne.master.newOnTop);
  CHECK(onDpOne.dwindle.preserveSplit);

  const auto onDpTwo = andromeda::resolveWorkspaceLayout(config, identity("DP-2"), "dev", 0);
  CHECK(onDpTwo.mode == LayoutMode::Scrolling);
  CHECK_EQ(onDpTwo.gap, 30);
  CHECK_EQ(onDpTwo.struts.left, 10);
  CHECK_EQ(onDpTwo.struts.right, 2);
  CHECK_EQ(onDpTwo.struts.top, 30);
  CHECK_EQ(onDpTwo.struts.bottom, 4);
  CHECK(onDpTwo.scrolling.defaultExtentFraction.has_value());
  CHECK_EQ(*onDpTwo.scrolling.defaultExtentFraction, 0.6);
  CHECK(onDpTwo.master.position == andromeda::MasterPosition::Right);
  CHECK_EQ(onDpTwo.master.defaultWidthFraction, 0.6);
  CHECK(onDpTwo.master.newOnTop);
  CHECK(!onDpTwo.dwindle.preserveSplit);

  const auto elsewhere = andromeda::resolveWorkspaceLayout(config, identity("HDMI-A-1"), "dev", 0);
  CHECK_EQ(elsewhere.gap, 12);
  CHECK_EQ(elsewhere.struts.left, 10);
  CHECK_EQ(elsewhere.struts.right, 2);
  CHECK_EQ(elsewhere.struts.top, 30);
  CHECK_EQ(elsewhere.struts.bottom, 4);
  CHECK(elsewhere.scrolling.defaultExtentFraction.has_value());
  CHECK_EQ(*elsewhere.scrolling.defaultExtentFraction, 0.6);
  CHECK(elsewhere.master.position == andromeda::MasterPosition::Right);
  CHECK_EQ(elsewhere.master.defaultWidthFraction, 0.6);
  CHECK(elsewhere.master.newOnTop);
  CHECK(!elsewhere.dwindle.preserveSplit);
}

ANDROMEDA_TEST(outputScrollingDefaultPrecedesWorkspaceRulesAndFollowsGlobalLayout) {
  Config config;
  config.layout.scrolling.defaultExtentFraction = 0.2;

  OutputRule output;
  output.name = "DP-1";
  output.layout.scrolling.defaultExtentFraction = 0.3;
  config.outputs.push_back(std::move(output));

  WorkspaceConfig globalWorkspace;
  globalWorkspace.name = "dev";
  globalWorkspace.layout.scrolling.defaultExtentFraction = 0.4;
  config.workspaceRules.push_back(std::move(globalWorkspace));

  WorkspaceConfig outputWorkspace;
  outputWorkspace.name = "dev";
  outputWorkspace.output = "DP-1";
  outputWorkspace.layout.scrolling.defaultExtentFraction = 0.5;
  config.workspaceRules.push_back(std::move(outputWorkspace));

  const auto outputSpecific = andromeda::resolveWorkspaceLayout(config, identity("DP-1"), "dev", 0);
  CHECK_EQ(*outputSpecific.scrolling.defaultExtentFraction, 0.5);

  const auto outputDefault = andromeda::resolveWorkspaceLayout(config, identity("DP-1"), "chat", 1);
  CHECK_EQ(*outputDefault.scrolling.defaultExtentFraction, 0.3);

  const auto workspaceDefault = andromeda::resolveWorkspaceLayout(config, identity("DP-2"), "dev", 0);
  CHECK_EQ(*workspaceDefault.scrolling.defaultExtentFraction, 0.4);

  const auto globalDefault = andromeda::resolveWorkspaceLayout(config, identity("DP-2"), "chat", 1);
  CHECK_EQ(*globalDefault.scrolling.defaultExtentFraction, 0.2);
}

ANDROMEDA_TEST(outputSpecificWorkspaceRulesBeatLaterGlobalRules) {
  Config config;

  WorkspaceConfig specific;
  specific.name = "dev";
  specific.output = "microstep msi g2712f cd6t084401192";
  specific.layout.gap = 20;
  config.workspaceRules.push_back(std::move(specific));

  WorkspaceConfig global;
  global.name = "dev";
  global.layout.gap = 12;
  config.workspaceRules.push_back(std::move(global));

  constexpr OutputIdentity monitor = identity("HDMI-A-1", "Microstep", "MSI G2712F", "CD6T084401192");
  const auto onMonitor = andromeda::resolveWorkspaceLayout(config, monitor, "dev", 0);
  CHECK_EQ(onMonitor.gap, 20);

  const auto elsewhere = andromeda::resolveWorkspaceLayout(config, identity("DP-1"), "dev", 0);
  CHECK_EQ(elsewhere.gap, 12);
}

ANDROMEDA_TEST(omittedScrollingDefaultWidthRemainsUnset) {
  Config config;

  const auto global = andromeda::resolveGlobalLayout(config);
  CHECK(!global.scrolling.defaultExtentFraction.has_value());

  const auto workspace = andromeda::resolveWorkspaceLayout(config, identity("DP-1"), "dev", 0);
  CHECK(!workspace.scrolling.defaultExtentFraction.has_value());
}

ANDROMEDA_TEST(workspaceInventoryResolvesStaticAndDynamicOutputs) {
  Config config;
  OutputRule fixed;
  fixed.name = "DP-1";
  fixed.workspaces = std::vector<std::string>{"main", "web"};
  config.outputs.push_back(std::move(fixed));

  WorkspaceConfig web;
  web.name = "web";
  web.output = "DP-1";
  web.layout.gap = 24;
  config.workspaceRules.push_back(std::move(web));

  OutputRule counted;
  counted.name = "DP-3";
  counted.workspaces = size_t{3};
  config.outputs.push_back(std::move(counted));

  WorkspaceConfig countedIndex;
  countedIndex.index = 2;
  countedIndex.output = "DP-3";
  countedIndex.layout.gap = 31;
  CHECK(andromeda::workspaceRuleTargetExists(config, countedIndex));
  config.workspaceRules.push_back(std::move(countedIndex));

  WorkspaceConfig countedName;
  countedName.name = "2";
  countedName.output = "DP-3";
  countedName.layout.mode = LayoutMode::Master;
  CHECK(!andromeda::workspaceRuleTargetExists(config, countedName));
  config.workspaceRules.push_back(std::move(countedName));

  const auto staticSet = andromeda::resolveWorkspacesForOutput(config, identity("DP-1"));
  CHECK(!staticSet.dynamic);
  CHECK_EQ(staticSet.workspaces.size(), size_t{2});
  CHECK_EQ(staticSet.workspaces[0].name, std::string{"main"});
  CHECK(staticSet.workspaces[0].named);
  CHECK_EQ(staticSet.workspaces[1].name, std::string{"web"});
  CHECK(staticSet.workspaces[1].named);
  CHECK_EQ(staticSet.workspaces[1].layout.gap, 24);

  const auto countedSet = andromeda::resolveWorkspacesForOutput(config, identity("DP-3"));
  CHECK(!countedSet.dynamic);
  CHECK_EQ(countedSet.workspaces.size(), size_t{3});
  if (countedSet.workspaces.size() == 3) {
    CHECK_EQ(countedSet.workspaces[0].name, std::string{"1"});
    CHECK(!countedSet.workspaces[0].named);
    CHECK_EQ(countedSet.workspaces[1].name, std::string{"2"});
    CHECK(!countedSet.workspaces[1].named);
    CHECK_EQ(countedSet.workspaces[1].layout.gap, 31);
    CHECK(countedSet.workspaces[1].layout.mode == LayoutMode::Scrolling);
    CHECK_EQ(countedSet.workspaces[2].name, std::string{"3"});
    CHECK(!countedSet.workspaces[2].named);
  }

  const auto dynamicSet = andromeda::resolveWorkspacesForOutput(config, identity("DP-2"));
  CHECK(dynamicSet.dynamic);
  CHECK_EQ(dynamicSet.workspaces.size(), size_t{1});
  CHECK_EQ(dynamicSet.workspaces[0].name, std::string{"1"});
  CHECK(!dynamicSet.workspaces[0].named);

  config.workspaces.emptyAbove = true;
  const auto dynamicSetWithEmptyAbove = andromeda::resolveWorkspacesForOutput(config, identity("DP-2"));
  CHECK(dynamicSetWithEmptyAbove.dynamic);
  CHECK_EQ(dynamicSetWithEmptyAbove.workspaces.size(), size_t{2});
  if (dynamicSetWithEmptyAbove.workspaces.size() == 2) {
    CHECK_EQ(dynamicSetWithEmptyAbove.workspaces[0].name, std::string{"1"});
    CHECK(!dynamicSetWithEmptyAbove.workspaces[0].named);
    CHECK_EQ(dynamicSetWithEmptyAbove.workspaces[1].name, std::string{"2"});
    CHECK(!dynamicSetWithEmptyAbove.workspaces[1].named);
  }
}

ANDROMEDA_TEST(dynamicWorkspaceNamesMaterializeWithScopeAndRemainDistinctFromNumericLabels) {
  Config config;
  config.workspaces.emptyAbove = true;

  WorkspaceConfig numericName;
  numericName.name = "1";
  numericName.layout.mode = LayoutMode::Master;
  config.workspaceRules.push_back(std::move(numericName));

  WorkspaceConfig global;
  global.name = "GLOBAL";
  config.workspaceRules.push_back(std::move(global));

  WorkspaceConfig globalLeftOverride;
  globalLeftOverride.name = "GLOBAL";
  globalLeftOverride.output = "DP-1";
  globalLeftOverride.layout.mode = LayoutMode::Dwindle;
  config.workspaceRules.push_back(std::move(globalLeftOverride));

  WorkspaceConfig leftOnly;
  leftOnly.name = "LEFT";
  leftOnly.output = "DP-1";
  leftOnly.layout.mode = LayoutMode::Dwindle;
  config.workspaceRules.push_back(std::move(leftOnly));

  const auto left = andromeda::resolveWorkspacesForOutput(config, identity("DP-1"));
  CHECK(left.dynamic);
  CHECK_EQ(left.workspaces.size(), size_t{5});
  if (left.workspaces.size() == 5) {
    CHECK_EQ(left.workspaces[0].name, std::string{"1"});
    CHECK(!left.workspaces[0].named);
    CHECK(left.workspaces[0].layout.mode == LayoutMode::Scrolling);

    CHECK_EQ(left.workspaces[1].name, std::string{"1"});
    CHECK(left.workspaces[1].named);
    CHECK(left.workspaces[1].layout.mode == LayoutMode::Master);

    CHECK_EQ(left.workspaces[2].name, std::string{"GLOBAL"});
    CHECK(left.workspaces[2].named);
    CHECK(left.workspaces[2].layout.mode == LayoutMode::Dwindle);
    CHECK_EQ(left.workspaces[3].name, std::string{"LEFT"});
    CHECK(left.workspaces[3].named);
    CHECK(left.workspaces[3].layout.mode == LayoutMode::Dwindle);

    CHECK_EQ(left.workspaces[4].name, std::string{"5"});
    CHECK(!left.workspaces[4].named);
  }

  const auto right = andromeda::resolveWorkspacesForOutput(config, identity("DP-2"));
  CHECK(right.dynamic);
  CHECK_EQ(right.workspaces.size(), size_t{4});
  if (right.workspaces.size() == 4) {
    CHECK_EQ(right.workspaces[0].name, std::string{"1"});
    CHECK(!right.workspaces[0].named);
    CHECK(right.workspaces[0].layout.mode == LayoutMode::Scrolling);

    CHECK_EQ(right.workspaces[1].name, std::string{"1"});
    CHECK(right.workspaces[1].named);
    CHECK(right.workspaces[1].layout.mode == LayoutMode::Master);

    CHECK_EQ(right.workspaces[2].name, std::string{"GLOBAL"});
    CHECK(right.workspaces[2].named);
    CHECK(right.workspaces[2].layout.mode == LayoutMode::Scrolling);
    CHECK_EQ(right.workspaces[3].name, std::string{"4"});
    CHECK(!right.workspaces[3].named);
  }
  CHECK(!std::ranges::any_of(right.workspaces, [](const auto& workspace) { return workspace.name == "LEFT"; }));

  const auto unnamedNumericLabel = andromeda::resolveUnnamedWorkspaceLayout(config, identity("DP-1"), 0);
  const auto explicitNumericName = andromeda::resolveWorkspaceLayout(config, identity("DP-1"), "1", 0);
  CHECK(unnamedNumericLabel.mode == LayoutMode::Scrolling);
  CHECK(explicitNumericName.mode == LayoutMode::Master);
}

ANDROMEDA_TEST(dynamicWorkspaceResolutionReportsNamesOmittedAcrossOutputAliases) {
  Config config;
  for (size_t index = 0; index < 63; ++index) {
    WorkspaceConfig named;
    named.name = "connector-" + std::to_string(index);
    named.output = "HDMI-A-1";
    config.workspaceRules.push_back(std::move(named));
  }
  WorkspaceConfig descriptor;
  descriptor.name = "descriptor";
  descriptor.output = "Microstep MSI G2712F CD6T084401192";
  config.workspaceRules.push_back(std::move(descriptor));

  constexpr OutputIdentity monitor = identity("HDMI-A-1", "Microstep", "MSI G2712F", "CD6T084401192");
  const auto resolved = andromeda::resolveWorkspacesForOutput(config, monitor);
  CHECK(resolved.dynamic);
  CHECK_EQ(resolved.workspaces.size(), andromeda::kMaxWorkspaces);
  CHECK_EQ(resolved.omittedNamed, size_t{1});
  CHECK_EQ(
      std::ranges::count_if(resolved.workspaces, [](const auto& workspace) { return workspace.named; }),
      static_cast<std::ptrdiff_t>(andromeda::kMaxWorkspaces - 1)
  );
  CHECK_EQ(resolved.workspaces.back().name, std::string{"64"});
  CHECK(!resolved.workspaces.back().named);
  CHECK(!std::ranges::any_of(resolved.workspaces, [](const auto& workspace) {
    return workspace.name == "descriptor";
  }));
}

ANDROMEDA_TEST(dynamicWorkspaceMinimumIsPerOutput) {
  Config config;
  OutputRule padded;
  padded.name = "DP-1";
  padded.minWorkspaces = 3;
  config.outputs.push_back(std::move(padded));

  CHECK_EQ(andromeda::resolveDynamicWorkspaceMinimum(config, identity("DP-1")), size_t{3});
  CHECK_EQ(andromeda::resolveDynamicWorkspaceMinimum(config, identity("DP-2")), size_t{1});

  const auto paddedSet = andromeda::resolveWorkspacesForOutput(config, identity("DP-1"));
  CHECK(paddedSet.dynamic);
  CHECK_EQ(paddedSet.workspaces.size(), size_t{3});
  if (paddedSet.workspaces.size() == 3) {
    CHECK_EQ(paddedSet.workspaces[0].name, std::string{"1"});
    CHECK_EQ(paddedSet.workspaces[2].name, std::string{"3"});
  }

  const auto bareSet = andromeda::resolveWorkspacesForOutput(config, identity("DP-2"));
  CHECK(bareSet.dynamic);
  CHECK_EQ(bareSet.workspaces.size(), size_t{1});

  // The leading empty is an extra entry only where it exceeds the minimum.
  config.workspaces.emptyAbove = true;
  CHECK_EQ(andromeda::resolveWorkspacesForOutput(config, identity("DP-1")).workspaces.size(), size_t{3});
  CHECK_EQ(andromeda::resolveWorkspacesForOutput(config, identity("DP-2")).workspaces.size(), size_t{2});
}
ANDROMEDA_TEST(workspaceRulesMatchConnectorAndDescriptorWithoutOutputSection) {
  Config config;

  WorkspaceConfig connector;
  connector.index = 1;
  connector.output = "hdmi-a-1";
  connector.layout.gap = 18;
  config.workspaceRules.push_back(std::move(connector));

  WorkspaceConfig descriptor;
  descriptor.index = 1;
  descriptor.output = "microstep msi g2712f cd6t084401192";
  descriptor.layout.mode = LayoutMode::Dwindle;
  config.workspaceRules.push_back(std::move(descriptor));

  constexpr OutputIdentity monitor = identity("HDMI-A-1", "Microstep", "MSI G2712F", "CD6T084401192");
  const auto resolved = andromeda::resolveWorkspaceLayout(config, monitor, "1", 0);
  CHECK_EQ(resolved.gap, 18);
  CHECK(resolved.mode == LayoutMode::Dwindle);
}

ANDROMEDA_TEST(descriptorOutputRuleOverridesConnectorFallback) {
  Config config;
  config.layout.scrolling.defaultExtentFraction = 0.5;

  OutputRule connector;
  connector.name = "HDMI-A-1";
  connector.workspaces = std::vector<std::string>{"fallback"};
  connector.layout.scrolling.defaultExtentFraction = 0.25;
  config.outputs.push_back(std::move(connector));

  OutputRule descriptor;
  descriptor.name = "Microstep MSI G2712F CD6T084401192";
  descriptor.workspaces = std::vector<std::string>{"specific"};
  descriptor.layout.scrolling.defaultExtentFraction = 0.75;
  config.outputs.push_back(std::move(descriptor));

  constexpr OutputIdentity monitor = identity("HDMI-A-1", "Microstep", "MSI G2712F", "CD6T084401192");
  const OutputRule* selected = andromeda::findOutputRule(config, monitor);
  CHECK(selected != nullptr);
  if (selected != nullptr) {
    CHECK_EQ(selected->name, std::string{"Microstep MSI G2712F CD6T084401192"});
    CHECK_EQ(*selected->layout.scrolling.defaultExtentFraction, 0.75);
  }

  const auto resolved = andromeda::resolveWorkspacesForOutput(config, monitor);
  CHECK(!resolved.dynamic);
  CHECK_EQ(resolved.workspaces.size(), size_t{1});
  if (resolved.workspaces.size() == 1) {
    CHECK_EQ(resolved.workspaces[0].name, std::string{"specific"});
    CHECK_EQ(*resolved.workspaces[0].layout.scrolling.defaultExtentFraction, 0.75);
  }
}

// Scrolling runs perpendicular to the workspace axis, and a descriptor rule that
// omits the axis does not inherit the connector rule's.
ANDROMEDA_TEST(workspaceAxisSelectsTheStripDirectionPerOutput) {
  Config config;

  OutputRule connector;
  connector.name = "HDMI-A-1";
  connector.workspaceAxis = andromeda::WorkspaceAxis::Horizontal;
  config.outputs.push_back(std::move(connector));

  OutputRule descriptor;
  descriptor.name = "Microstep MSI G2712F CD6T084401192";
  config.outputs.push_back(std::move(descriptor));

  OutputRule horizontal;
  horizontal.name = "DP-1";
  horizontal.workspaceAxis = andromeda::WorkspaceAxis::Horizontal;
  config.outputs.push_back(std::move(horizontal));

  constexpr OutputIdentity monitor = identity("HDMI-A-1", "Microstep", "MSI G2712F", "CD6T084401192");
  CHECK(andromeda::resolveWorkspaceAxis(config, monitor) == andromeda::WorkspaceAxis::Vertical);
  CHECK(
      andromeda::resolveWorkspaceLayout(config, monitor, "1", 0).scrolling.direction
      == andromeda::ScrollingDirection::Horizontal
  );

  CHECK(andromeda::resolveWorkspaceAxis(config, identity("DP-1")) == andromeda::WorkspaceAxis::Horizontal);
  CHECK(
      andromeda::resolveWorkspaceLayout(config, identity("DP-1"), "1", 0).scrolling.direction
      == andromeda::ScrollingDirection::Vertical
  );

  CHECK(andromeda::resolveWorkspaceAxis(config, identity("DP-9")) == andromeda::WorkspaceAxis::Vertical);
  CHECK(
      andromeda::resolveWorkspaceLayout(config, identity("DP-9"), "1", 0).scrolling.direction
      == andromeda::ScrollingDirection::Horizontal
  );
}

// A workspace rule cannot override the resolved strip direction.
ANDROMEDA_TEST(workspaceRulesCannotOverrideTheResolvedStripDirection) {
  Config config;

  OutputRule horizontal;
  horizontal.name = "DP-1";
  horizontal.workspaceAxis = andromeda::WorkspaceAxis::Horizontal;
  config.outputs.push_back(std::move(horizontal));

  WorkspaceConfig rule;
  rule.name = "dev";
  rule.layout.scrolling.centerFocused = andromeda::CenterFocusedColumn::Always;
  config.workspaceRules.push_back(std::move(rule));

  const auto resolved = andromeda::resolveWorkspaceLayout(config, identity("DP-1"), "dev", 0);
  CHECK(resolved.scrolling.centerFocused == andromeda::CenterFocusedColumn::Always);
  CHECK(resolved.scrolling.direction == andromeda::ScrollingDirection::Vertical);
}

ANDROMEDA_TEST(fixedWorkspaceTargetSelectsItsUniqueOutput) {
  Config config;

  OutputRule primary;
  primary.name = "DP-1";
  primary.workspaces = std::vector<std::string>{"1", "2", "3", "COMMON"};
  config.outputs.push_back(std::move(primary));

  OutputRule chat;
  chat.name = "HDMI-A-1";
  chat.workspaces = std::vector<std::string>{"CHAT", "COMMON"};
  config.outputs.push_back(std::move(chat));

  OutputRule dynamic;
  dynamic.name = "DP-2";
  config.outputs.push_back(std::move(dynamic));

  CHECK_EQ(andromeda::uniqueFixedWorkspaceOwner(config, 3), config.outputs.data());
  CHECK(andromeda::uniqueFixedWorkspaceOwner(config, 0) == nullptr);
  CHECK(andromeda::uniqueFixedWorkspaceOwner(config, 4) == nullptr);
  CHECK_EQ(andromeda::uniqueFixedWorkspaceOwner(config, "1"), config.outputs.data());
  CHECK_EQ(andromeda::uniqueFixedWorkspaceOwner(config, "CHAT"), config.outputs.data() + 1);
  CHECK(andromeda::uniqueFixedWorkspaceOwner(config, "COMMON") == nullptr);
  CHECK(andromeda::uniqueFixedWorkspaceOwner(config, "missing") == nullptr);
}

ANDROMEDA_TEST(windowRulesMergeMatchingFieldsInOrder) {
  Config config;

  WindowRule app;
  app.appIdPattern = "^foot$";
  app.appIdRegex = std::regex(app.appIdPattern);
  app.opacity = 0.5;
  app.blur = true;
  app.defaultFocused = false;
  app.defaultPinned = true;
  app.defaultScrollingColumn = "browser-stack";
  app.defaultScrollingColumnOrder = 20;
  app.focusOnActivate = false;
  app.vrr = VrrMode::Disabled;
  app.allowTearing = false;
  app.hdr = andromeda::HdrMode::Off;
  app.defaultPosition = andromeda::WindowPosition{
      .x = 12,
      .y = 24,
      .anchor = andromeda::WindowPositionAnchor::TopRight,
  };
  config.windowRules.push_back(std::move(app));

  WindowRule title;
  title.titlePattern = "shell";
  title.titleRegex = std::regex(title.titlePattern);
  title.opacity = 0.8;
  title.focusOnActivate = true;
  title.vrr = VrrMode::Always;
  title.allowTearing = true;
  title.hdr = andromeda::HdrMode::On;
  title.defaultPinned = false;
  title.defaultScrollingColumn = "terminals";
  title.defaultScrollingColumnOrder = 10;
  config.windowRules.push_back(std::move(title));

  WindowRule unfocused;
  unfocused.matchFocused = false;
  unfocused.defaultFloating = true;
  config.windowRules.push_back(std::move(unfocused));

  const auto resolved =
      andromeda::resolveWindowRules(config, "foot", "project shell", std::nullopt, ContentType::None, {}, 0);
  CHECK(resolved.opacity && *resolved.opacity == 0.8);
  CHECK(resolved.blur && *resolved.blur);
  CHECK(resolved.defaultFloating && *resolved.defaultFloating);
  CHECK(resolved.defaultPosition.has_value());
  CHECK_EQ(resolved.defaultPosition->x, 12);
  CHECK_EQ(resolved.defaultPosition->y, 24);
  CHECK(resolved.defaultPosition->anchor == andromeda::WindowPositionAnchor::TopRight);
  CHECK(resolved.defaultFocused && !*resolved.defaultFocused);
  CHECK(resolved.defaultPinned && !*resolved.defaultPinned);
  CHECK(resolved.defaultScrollingColumn && *resolved.defaultScrollingColumn == "terminals");
  CHECK(resolved.defaultScrollingColumnOrder && *resolved.defaultScrollingColumnOrder == 10);
  CHECK(resolved.focusOnActivate && *resolved.focusOnActivate);
  CHECK(resolved.vrr == VrrMode::Always);
  CHECK(resolved.allowTearing && *resolved.allowTearing);
  CHECK(resolved.hdr == andromeda::HdrMode::On);

  const auto appOnly = andromeda::resolveWindowRules(config, "foot", "editor", std::nullopt, ContentType::None, {}, 0);
  CHECK(appOnly.defaultPinned && *appOnly.defaultPinned);
  CHECK(appOnly.defaultScrollingColumn && *appOnly.defaultScrollingColumn == "browser-stack");
  CHECK(appOnly.defaultScrollingColumnOrder && *appOnly.defaultScrollingColumnOrder == 20);
  CHECK(appOnly.vrr == VrrMode::Disabled);
  CHECK(appOnly.allowTearing && !*appOnly.allowTearing);
  CHECK(appOnly.hdr == andromeda::HdrMode::Off);

  const auto focused = andromeda::resolveWindowRules(
      config, "foot", "project shell", std::nullopt, ContentType::None, {.focused = true}, 0
  );
  CHECK(focused.opacity && *focused.opacity == 0.8);
  CHECK(!focused.defaultFloating);
  CHECK(andromeda::anyWindowRuleHasTitlePattern(config));
}

// Decoration overrides merge per field like every other dynamic effect: the last
// matching rule that names a key wins it, and a key no rule sets stays unset so
// the window keeps the global [appearance] value.
ANDROMEDA_TEST(windowRuleDecorationOverridesMergePerField) {

  Config config;

  WindowRule app;
  app.appIdPattern = "^foot$";
  app.appIdRegex = std::regex(app.appIdPattern);
  app.borderWidth = 0;
  app.shadow = false;
  app.outerBorderWidth = 3;
  config.windowRules.push_back(std::move(app));

  WindowRule title;
  title.titlePattern = "editor";
  title.titleRegex = std::regex(title.titlePattern);
  title.cornerRadius = 0;
  title.shadow = true;
  config.windowRules.push_back(std::move(title));

  const auto both = andromeda::resolveWindowRules(config, "foot", "editor", std::nullopt, ContentType::None, {}, 0);
  CHECK(both.borderWidth && *both.borderWidth == 0);
  CHECK(both.outerBorderWidth && *both.outerBorderWidth == 3);
  CHECK(both.cornerRadius && *both.cornerRadius == 0);
  CHECK(both.shadow && *both.shadow);

  const auto appOnly = andromeda::resolveWindowRules(config, "foot", "docs", std::nullopt, ContentType::None, {}, 0);
  CHECK(appOnly.borderWidth && *appOnly.borderWidth == 0);
  CHECK(appOnly.shadow && !*appOnly.shadow);
  CHECK(!appOnly.cornerRadius);
}

ANDROMEDA_TEST(windowRuleEffectOverridesUseLastWriterWins) {
  andromeda::Config config;
  andromeda::WindowRule first;
  first.appIdPattern = "foot";
  first.appIdRegex = std::regex("foot");
  first.borderEffect = "pulse";
  first.windowEffect = "lines";
  andromeda::WindowRule second = first;
  second.borderEffect = "off";
  second.windowEffect.reset();
  config.windowRules = {first, second};
  const auto resolved =
      andromeda::resolveWindowRules(config, "foot", std::nullopt, std::nullopt, ContentType::None, {}, 0);
  CHECK(resolved.borderEffect == "off");
  CHECK(resolved.windowEffect == "lines");
}

ANDROMEDA_TEST(windowRulesMergeWorkspaceTargetsAcrossSelectorKinds) {
  Config config;

  WindowRule app;
  app.appIdPattern = "^foot$";
  app.appIdRegex = std::regex(app.appIdPattern);
  app.defaultWorkspace = andromeda::WorkspaceReference{andromeda::WorkspaceIndex{2}};
  config.windowRules.push_back(std::move(app));

  WindowRule title;
  title.titlePattern = "chat";
  title.titleRegex = std::regex(title.titlePattern);
  title.defaultWorkspace = andromeda::WorkspaceReference{andromeda::WorkspaceName{"2"}};
  config.windowRules.push_back(std::move(title));

  const auto appOnly = andromeda::resolveWindowRules(config, "foot", "editor", std::nullopt, ContentType::None, {}, 0);
  const auto* position =
      appOnly.defaultWorkspace ? std::get_if<andromeda::WorkspaceIndex>(&*appOnly.defaultWorkspace) : nullptr;
  CHECK(position != nullptr);
  CHECK(position != nullptr && position->value == 2);

  const auto merged =
      andromeda::resolveWindowRules(config, "foot", "project chat", std::nullopt, ContentType::None, {}, 0);
  const auto* name = merged.defaultWorkspace ? std::get_if<andromeda::WorkspaceName>(&*merged.defaultWorkspace) : nullptr;
  CHECK(name != nullptr);
  CHECK(name != nullptr && name->value == "2");
}

ANDROMEDA_TEST(windowRulesMergeDefaultScratchpadLastWriterWins) {
  Config config;

  WindowRule app;
  app.appIdPattern = "^foot$";
  app.appIdRegex = std::regex(app.appIdPattern);
  app.defaultScratchpad = "terminal";
  config.windowRules.push_back(std::move(app));

  WindowRule title;
  title.titlePattern = "music";
  title.titleRegex = std::regex(title.titlePattern);
  title.defaultScratchpad = "media";
  config.windowRules.push_back(std::move(title));

  const auto appOnly = andromeda::resolveWindowRules(config, "foot", "shell", std::nullopt, ContentType::None, {}, 0);
  CHECK(appOnly.defaultScratchpad == "terminal");

  const auto merged = andromeda::resolveWindowRules(config, "foot", "music", std::nullopt, ContentType::None, {}, 0);
  CHECK(merged.defaultScratchpad == "media");
}

ANDROMEDA_TEST(windowRulesMergeSizingFieldsLastWriterWins) {
  Config config;

  WindowRule first;
  first.appIdPattern = "^utility$";
  first.appIdRegex = std::regex(first.appIdPattern);
  first.defaultFloating = true;
  first.defaultFloatingWidthPx = 800;
  first.defaultFloatingWidth = 0.5;
  first.defaultFloatingHeight = 0.5;
  first.defaultScrollingExtent = 0.5;
  first.defaultScrollingExtentPx = 800;
  config.windowRules.push_back(std::move(first));

  const auto sameRule =
      andromeda::resolveWindowRules(config, "utility", std::nullopt, std::nullopt, ContentType::None, {}, 0);
  CHECK(sameRule.defaultFloatingWidthPx && *sameRule.defaultFloatingWidthPx == 800);
  CHECK(sameRule.defaultFloatingWidth && *sameRule.defaultFloatingWidth == 0.5);
  CHECK(sameRule.defaultScrollingExtentPx && *sameRule.defaultScrollingExtentPx == 800);
  CHECK(sameRule.defaultScrollingExtent && *sameRule.defaultScrollingExtent == 0.5);

  WindowRule second;
  second.appIdPattern = "^utility$";
  second.appIdRegex = std::regex(second.appIdPattern);
  second.defaultScrollingExtent = 0.75;
  second.defaultFloatingWidth = 0.6;
  second.defaultFloatingHeight = 0.75;
  config.windowRules.push_back(std::move(second));

  const auto resolved =
      andromeda::resolveWindowRules(config, "utility", std::nullopt, std::nullopt, ContentType::None, {}, 0);
  CHECK(resolved.defaultFloating && *resolved.defaultFloating);
  CHECK(!resolved.defaultFloatingWidthPx);
  CHECK(resolved.defaultFloatingWidth && *resolved.defaultFloatingWidth == 0.6);
  CHECK(resolved.defaultFloatingHeight && *resolved.defaultFloatingHeight == 0.75);
  // A later rule selecting another unit replaces the earlier unit for that axis or extent.
  CHECK(resolved.defaultScrollingExtent && *resolved.defaultScrollingExtent == 0.75);
  CHECK(!resolved.defaultScrollingExtentPx);
}

ANDROMEDA_TEST(windowRulesMergeBorderColorsLastWriterWins) {
  Config config;

  WindowRule app;
  app.appIdPattern = "^foot$";
  app.appIdRegex = std::regex(app.appIdPattern);
  app.borderColorFocused = std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F};
  config.windowRules.push_back(std::move(app));

  WindowRule title;
  title.titlePattern = "shell";
  title.titleRegex = std::regex(title.titlePattern);
  title.borderColorFocused = std::array<float, 4>{0.0F, 1.0F, 0.0F, 1.0F};
  title.borderColorOuter = std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F};
  config.windowRules.push_back(std::move(title));

  const auto appOnly = andromeda::resolveWindowRules(config, "foot", "editor", std::nullopt, ContentType::None, {}, 0);
  CHECK(appOnly.borderColorFocused && (*appOnly.borderColorFocused)[0] == 1.0F);
  CHECK(!appOnly.borderColorUnfocused);
  CHECK(!appOnly.borderColorOuter);

  // Each key is independent: the later rule replaces only the keys it sets.
  const auto merged =
      andromeda::resolveWindowRules(config, "foot", "project shell", std::nullopt, ContentType::None, {}, 0);
  CHECK(merged.borderColorFocused && (*merged.borderColorFocused)[1] == 1.0F);
  CHECK(!merged.borderColorUnfocused);
  CHECK(merged.borderColorOuter && (*merged.borderColorOuter)[2] == 0.3F);
}

ANDROMEDA_TEST(windowRulesMatchContentTypesAndComposeSelectors) {
  Config config;

  WindowRule photo;
  photo.matchContentType = ContentType::Photo;
  photo.opacity = 0.25;
  config.windowRules.push_back(std::move(photo));

  WindowRule game;
  game.appIdPattern = "^runner$";
  game.appIdRegex = std::regex(game.appIdPattern);
  game.titlePattern = "playing";
  game.titleRegex = std::regex(game.titlePattern);
  game.matchContentType = ContentType::Game;
  game.matchFocused = false;
  game.matchAtStartup = true;
  game.opacity = 0.75;
  config.windowRules.push_back(std::move(game));

  WindowRule afterStartup;
  afterStartup.matchContentType = ContentType::Game;
  afterStartup.matchAtStartup = false;
  afterStartup.defaultFloating = true;
  config.windowRules.push_back(std::move(afterStartup));

  WindowRule none;
  none.matchContentType = ContentType::None;
  none.defaultFloating = true;
  config.windowRules.push_back(std::move(none));

  const auto matchingGame =
      andromeda::resolveWindowRules(config, "runner", "now playing", std::nullopt, ContentType::Game, {}, 0);
  CHECK(matchingGame.opacity && *matchingGame.opacity == 0.75);
  CHECK(!matchingGame.defaultFloating);

  const auto wrongApp =
      andromeda::resolveWindowRules(config, "launcher", "now playing", std::nullopt, ContentType::Game, {}, 0);
  CHECK(!wrongApp.opacity);
  const auto wrongTitle =
      andromeda::resolveWindowRules(config, "runner", "paused", std::nullopt, ContentType::Game, {}, 0);
  CHECK(!wrongTitle.opacity);
  const auto wrongFocus = andromeda::resolveWindowRules(
      config, "runner", "now playing", std::nullopt, ContentType::Game, {.focused = true}, 0
  );
  CHECK(!wrongFocus.opacity);
  const auto afterStartupRule = andromeda::resolveWindowRules(
      config, "runner", "now playing", std::nullopt, ContentType::Game, {}, andromeda::kStartupWindowRuleDurationMs
  );
  CHECK(!afterStartupRule.opacity);
  CHECK(afterStartupRule.defaultFloating && *afterStartupRule.defaultFloating);

  const auto matchingPhoto =
      andromeda::resolveWindowRules(config, "viewer", "photo", std::nullopt, ContentType::Photo, {}, 0);
  CHECK(matchingPhoto.opacity && *matchingPhoto.opacity == 0.25);

  const auto matchingNone =
      andromeda::resolveWindowRules(config, "terminal", "shell", std::nullopt, ContentType::None, {}, 0);
  CHECK(matchingNone.defaultFloating && *matchingNone.defaultFloating);

  const auto video = andromeda::resolveWindowRules(config, "viewer", "video", std::nullopt, ContentType::Video, {}, 0);
  CHECK(!video.opacity);
  CHECK(!video.defaultFloating);
}

ANDROMEDA_TEST(windowRulesMatchWindowStateSelectors) {
  Config config;

  WindowRule floatingRule;
  floatingRule.matchFloating = true;
  floatingRule.matchScratchpad = false;
  floatingRule.opacity = 0.4;
  config.windowRules.push_back(std::move(floatingRule));

  WindowRule pinnedRule;
  pinnedRule.matchPinned = true;
  pinnedRule.blur = false;
  config.windowRules.push_back(std::move(pinnedRule));

  WindowRule scratchpadRule;
  scratchpadRule.matchScratchpad = true;
  scratchpadRule.opacity = 0.9;
  config.windowRules.push_back(std::move(scratchpadRule));

  const auto tiled = andromeda::resolveWindowRules(config, "foot", "shell", std::nullopt, ContentType::None, {}, 0);
  CHECK(!tiled.opacity);
  CHECK(!tiled.blur);

  const auto floating =
      andromeda::resolveWindowRules(config, "foot", "shell", std::nullopt, ContentType::None, {.floating = true}, 0);
  CHECK(floating.opacity && *floating.opacity == 0.4);
  CHECK(!floating.blur);

  // A pinned window is floating too, so both selectors hold at once.
  const auto pinned = andromeda::resolveWindowRules(
      config, "foot", "shell", std::nullopt, ContentType::None, {.floating = true, .pinned = true}, 0
  );
  CHECK(pinned.opacity && *pinned.opacity == 0.4);
  CHECK(pinned.blur && !*pinned.blur);

  // is_scratchpad = false excludes the floating rule; the scratchpad rule wins the opacity.
  const auto scratchpad = andromeda::resolveWindowRules(
      config, "foot", "shell", std::nullopt, ContentType::None, {.floating = true, .scratchpad = true}, 0
  );
  CHECK(scratchpad.opacity && *scratchpad.opacity == 0.9);
}

ANDROMEDA_TEST(windowRulesMatchXdgTagsAndComposeSelectors) {
  Config config;

  WindowRule anyGameTag;
  anyGameTag.xdgTagPattern = "^game-";
  anyGameTag.xdgTagRegex = std::regex(anyGameTag.xdgTagPattern);
  anyGameTag.opacity = 0.25;
  config.windowRules.push_back(std::move(anyGameTag));

  WindowRule launcher;
  launcher.xdgTagPattern = "^game-launcher$";
  launcher.xdgTagRegex = std::regex(launcher.xdgTagPattern);
  launcher.opacity = 0.9;
  launcher.defaultFloating = true;
  config.windowRules.push_back(std::move(launcher));

  WindowRule running;
  running.appIdPattern = "^runner$";
  running.appIdRegex = std::regex(running.appIdPattern);
  running.titlePattern = "playing";
  running.titleRegex = std::regex(running.titlePattern);
  running.xdgTagPattern = "^game-(running|settings)$";
  running.xdgTagRegex = std::regex(running.xdgTagPattern);
  running.matchContentType = ContentType::Game;
  running.matchFocused = false;
  running.opacity = 0.5;
  config.windowRules.push_back(std::move(running));

  const auto matchingLauncher =
      andromeda::resolveWindowRules(config, "runner", "now playing", "game-launcher", ContentType::Game, {}, 0);
  CHECK(matchingLauncher.opacity && *matchingLauncher.opacity == 0.9);
  CHECK(matchingLauncher.defaultFloating && *matchingLauncher.defaultFloating);

  const auto matchingRunning =
      andromeda::resolveWindowRules(config, "runner", "now playing", "game-running", ContentType::Game, {}, 0);
  CHECK(matchingRunning.opacity && *matchingRunning.opacity == 0.5);

  const auto matchingSecondTag =
      andromeda::resolveWindowRules(config, "runner", "now playing", "game-settings", ContentType::Game, {}, 0);
  CHECK(matchingSecondTag.opacity && *matchingSecondTag.opacity == 0.5);

  const auto wrongApp =
      andromeda::resolveWindowRules(config, "launcher", "now playing", "game-running", ContentType::Game, {}, 0);
  CHECK(wrongApp.opacity && *wrongApp.opacity == 0.25);
  const auto wrongTitle =
      andromeda::resolveWindowRules(config, "runner", "paused", "game-running", ContentType::Game, {}, 0);
  CHECK(wrongTitle.opacity && *wrongTitle.opacity == 0.25);
  const auto wrongContent =
      andromeda::resolveWindowRules(config, "runner", "now playing", "game-running", ContentType::Video, {}, 0);
  CHECK(wrongContent.opacity && *wrongContent.opacity == 0.25);
  const auto wrongFocus = andromeda::resolveWindowRules(
      config, "runner", "now playing", "game-running", ContentType::Game, {.focused = true}, 0
  );
  CHECK(wrongFocus.opacity && *wrongFocus.opacity == 0.25);

  const auto missingTag =
      andromeda::resolveWindowRules(config, "runner", "now playing", std::nullopt, ContentType::Game, {}, 0);
  CHECK(!missingTag.opacity);
  CHECK(!missingTag.defaultFloating);
  const auto unknownTag =
      andromeda::resolveWindowRules(config, "runner", "now playing", "browser", ContentType::Game, {}, 0);
  CHECK(!unknownTag.opacity);
  CHECK(!unknownTag.defaultFloating);
}

ANDROMEDA_TEST(windowRulesMatchEmptyIdentityOnlyWhenTheClientSetIt) {
  Config config;

  WindowRule blankTitle;
  blankTitle.titlePattern = "^$";
  blankTitle.titleRegex = std::regex(blankTitle.titlePattern);
  blankTitle.defaultFloating = true;
  config.windowRules.push_back(std::move(blankTitle));

  WindowRule blankAppId;
  blankAppId.appIdPattern = "^$";
  blankAppId.appIdRegex = std::regex(blankAppId.appIdPattern);
  blankAppId.opacity = 0.5;
  config.windowRules.push_back(std::move(blankAppId));

  WindowRule blankTag;
  blankTag.xdgTagPattern = "^$";
  blankTag.xdgTagRegex = std::regex(blankTag.xdgTagPattern);
  blankTag.defaultMaximize = true;
  config.windowRules.push_back(std::move(blankTag));

  // A window whose title, app ID, and tag are all set to the empty string.
  const auto allEmpty = andromeda::resolveWindowRules(config, "", "", "", ContentType::None, {}, 0);
  CHECK(allEmpty.defaultFloating && *allEmpty.defaultFloating);
  CHECK(allEmpty.opacity && *allEmpty.opacity == 0.5);
  CHECK(allEmpty.defaultMaximize && *allEmpty.defaultMaximize);

  // A window that never sent any of them matches nothing: an unset value is not an empty one.
  const auto allUnset =
      andromeda::resolveWindowRules(config, std::nullopt, std::nullopt, std::nullopt, ContentType::None, {}, 0);
  CHECK(!allUnset.defaultFloating);
  CHECK(!allUnset.opacity);
  CHECK(!allUnset.defaultMaximize);

  // Selectors are independent: an empty title matches while a non-empty app ID and an unset tag do not.
  const auto emptyTitleOnly =
      andromeda::resolveWindowRules(config, "firefox", "", std::nullopt, ContentType::None, {}, 0);
  CHECK(emptyTitleOnly.defaultFloating && *emptyTitleOnly.defaultFloating);
  CHECK(!emptyTitleOnly.opacity);
  CHECK(!emptyTitleOnly.defaultMaximize);
}

ANDROMEDA_TEST(windowRulesMatchIsAlone) {
  Config config;

  WindowRule aloneOnly;
  aloneOnly.appIdPattern = "^player$";
  aloneOnly.appIdRegex = std::regex(aloneOnly.appIdPattern);
  aloneOnly.matchAlone = true;
  aloneOnly.defaultMaximize = true;
  config.windowRules.push_back(std::move(aloneOnly));

  WindowRule notAloneOnly;
  notAloneOnly.appIdPattern = "^player$";
  notAloneOnly.appIdRegex = std::regex(notAloneOnly.appIdPattern);
  notAloneOnly.matchAlone = false;
  notAloneOnly.defaultFloating = true;
  config.windowRules.push_back(std::move(notAloneOnly));

  // Alone windows take the is_alone=true rule and skip the is_alone=false one.
  const auto alone =
      andromeda::resolveWindowRules(config, "player", std::nullopt, std::nullopt, ContentType::None, {.alone = true}, 0);
  CHECK(alone.defaultMaximize && *alone.defaultMaximize);
  CHECK(!alone.defaultFloating);

  // Windows that are not alone skip the is_alone=true rule and take the is_alone=false one.
  const auto notAlone =
      andromeda::resolveWindowRules(config, "player", std::nullopt, std::nullopt, ContentType::None, {}, 0);
  CHECK(notAlone.defaultFloating && *notAlone.defaultFloating);
  CHECK(!notAlone.defaultMaximize);

  // A rule without the selector matches regardless of being alone.
  WindowRule anyState;
  anyState.appIdPattern = "^worker$";
  anyState.appIdRegex = std::regex(anyState.appIdPattern);
  anyState.defaultPinned = true;
  config.windowRules.push_back(std::move(anyState));

  const auto anyStateAlone =
      andromeda::resolveWindowRules(config, "worker", std::nullopt, std::nullopt, ContentType::None, {.alone = true}, 0);
  CHECK(anyStateAlone.defaultPinned && *anyStateAlone.defaultPinned);
  const auto anyStateNotAlone =
      andromeda::resolveWindowRules(config, "worker", std::nullopt, std::nullopt, ContentType::None, {}, 0);
  CHECK(anyStateNotAlone.defaultPinned && *anyStateNotAlone.defaultPinned);
}

ANDROMEDA_TEST(windowVrrRuleOverridesTheOutputPolicy) {
  CHECK(andromeda::effectiveVrrEnabled(VrrMode::Disabled, false, VrrMode::Always, false));
  CHECK(!andromeda::effectiveVrrEnabled(VrrMode::Always, false, VrrMode::Disabled, false));
  CHECK(!andromeda::effectiveVrrEnabled(VrrMode::Disabled, false, VrrMode::Fullscreen, false));
  CHECK(andromeda::effectiveVrrEnabled(VrrMode::Disabled, false, VrrMode::Fullscreen, true));
  CHECK(andromeda::effectiveVrrEnabled(VrrMode::Fullscreen, true, std::nullopt, false));
}

ANDROMEDA_TEST(tearingRequiresTheOutputGateAndUsesTheWindowOverride) {
  CHECK(!andromeda::tearingEnabled(false, std::nullopt, true));
  CHECK(!andromeda::tearingEnabled(false, true, true));
  CHECK(andromeda::tearingEnabled(true, std::nullopt, true));
  CHECK(!andromeda::tearingEnabled(true, std::nullopt, false));
  CHECK(andromeda::tearingEnabled(true, true, false));
  CHECK(!andromeda::tearingEnabled(true, false, true));
}

ANDROMEDA_TEST(layerRulesMergeMatchingFieldsInOrder) {
  Config config;

  LayerRule first;
  first.namespacePattern = "^panel$";
  first.namespaceRegex = std::regex(first.namespacePattern);
  first.blur = true;
  first.ignoreAlpha = 0.2;
  config.layerRules.push_back(std::move(first));

  LayerRule second;
  second.namespacePattern = "^panel$";
  second.namespaceRegex = std::regex(second.namespacePattern);
  second.ignoreAlpha = 0.75;
  second.optimized = true;
  config.layerRules.push_back(std::move(second));

  const auto resolved = andromeda::resolveLayerRules(config, "panel");
  CHECK(resolved.blur && *resolved.blur);
  CHECK(resolved.ignoreAlpha && *resolved.ignoreAlpha == 0.75);
  CHECK(resolved.optimized && *resolved.optimized);

  const auto unmatched = andromeda::resolveLayerRules(config, "wallpaper");
  CHECK(!unmatched.blur);
  CHECK(!unmatched.ignoreAlpha);
  CHECK(!unmatched.optimized);

  LayerRule blank;
  blank.namespacePattern = "^$";
  blank.namespaceRegex = std::regex(blank.namespacePattern);
  blank.blur = true;
  config.layerRules.push_back(std::move(blank));

  // Layer surfaces name themselves at creation, so an empty namespace is a value a rule can match.
  const auto blankNamespace = andromeda::resolveLayerRules(config, "");
  CHECK(blankNamespace.blur && *blankNamespace.blur);
  CHECK(!andromeda::resolveLayerRules(config, std::nullopt).blur);
}

ANDROMEDA_TEST(securityContextRulesGrantGlobalsByMetadata) {
  Config config;

  SecurityContextRule scoped;
  scoped.sandboxEnginePattern = "^org\\.flatpak$";
  scoped.sandboxEngineRegex = std::regex(scoped.sandboxEnginePattern);
  scoped.appIdPattern = "^org\\.example\\.Bar$";
  scoped.appIdRegex = std::regex(scoped.appIdPattern);
  scoped.allowGlobals = {"zwlr_layer_shell_v1"};
  config.securityContextRules.push_back(std::move(scoped));

  SecurityContextRule wildcard;
  wildcard.allowGlobals = {"ext_idle_notifier_v1"};
  config.securityContextRules.push_back(std::move(wildcard));

  SecurityContextRule partial;
  partial.appIdPattern = "example";
  partial.appIdRegex = std::regex(partial.appIdPattern);
  partial.allowGlobals = {"zwlr_screencopy_manager_v1"};
  config.securityContextRules.push_back(std::move(partial));

  const auto grants = [&](const char* engine, const char* appId, std::string_view global) {
    const std::vector<std::string> globals = andromeda::securityContextRuleGlobals(config, engine, appId);
    return std::ranges::find(globals, global) != globals.end();
  };

  CHECK(grants("org.flatpak", "org.example.Bar", "zwlr_layer_shell_v1"));
  // Every specified match key must hold.
  CHECK(!grants("org.flatpak", "org.example.Other", "zwlr_layer_shell_v1"));
  CHECK(!grants("waypak", "org.example.Bar", "zwlr_layer_shell_v1"));
  // Absent metadata never satisfies a pattern.
  CHECK(!grants(nullptr, "org.example.Bar", "zwlr_layer_shell_v1"));
  // A rule without match keys applies to every client, but only for its own globals.
  CHECK(grants(nullptr, nullptr, "ext_idle_notifier_v1"));
  CHECK_EQ(andromeda::securityContextRuleGlobals(config, nullptr, nullptr).size(), size_t{1});
  // The whole value must match; a substring is not enough.
  CHECK(!grants("org.flatpak", "org.example.Bar", "zwlr_screencopy_manager_v1"));
  CHECK(grants(nullptr, "example", "zwlr_screencopy_manager_v1"));
  // Matching rules pool their globals.
  CHECK_EQ(andromeda::securityContextRuleGlobals(config, "org.flatpak", "org.example.Bar").size(), size_t{2});
}

int main() { return RUN_TESTS(); }
