#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class AsyncTextureCache;
class Flex;
class Node;
class Renderer;
class RovingListNavHost;
class ScrollView;
struct ScrollViewState;

namespace settings {
  struct SettingsPage;

  struct SettingsSidebarContext {
    // Pages to list, in IA order.
    const std::vector<const SettingsPage*>& pages;
    const std::vector<std::string>& availableBars;
    float scale = 1.0F;
    bool globalSearchActive = false;
    const std::string& selectedPage;
    const std::string& selectedBarName;
    std::string& creatingBarName;

    // The search field sits above the account card; the window owns it because search drives the content.
    std::unique_ptr<Node> searchField;
    std::string accountName;
    std::string accountAvatarPath;
    Renderer* renderer = nullptr;
    AsyncTextureCache* avatarTextures = nullptr;

    ScrollViewState& sidebarScrollState;

    std::function<void()> clearTransientState;
    std::function<void()> requestRebuild;
    std::function<void(std::string page, std::string bar)> navigate;
    std::function<void()> openAccount;
    std::function<void(std::string)> createBar;
    std::function<void(const Node*)> scrollSidebarNodeIntoView;
    RovingListNavHost** outNav = nullptr;
    ScrollView** outScroll = nullptr;
  };

  [[nodiscard]] std::unique_ptr<Flex> buildSettingsSidebar(SettingsSidebarContext ctx);

} // namespace settings
