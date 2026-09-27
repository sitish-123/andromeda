#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/tab.h"

#include <string>

class Flex;
class Label;
class LinkService;
class ScrollView;

class DevicesTab : public Tab {
public:
  explicit DevicesTab(LinkService* link);

  std::unique_ptr<Flex> create() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuild(Renderer& renderer);
  void syncCountdown();
  [[nodiscard]] std::string structureKey() const;
  [[nodiscard]] std::unique_ptr<Flex> makeSettings(const std::string& id, float scale);
  [[nodiscard]] std::unique_ptr<Flex> makeNearby(float scale, float opacity);
  void pickAndSend(const std::string& id);

  LinkService* m_link = nullptr;
  Flex* m_rootLayout = nullptr;
  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;
  Label* m_countdown = nullptr;
  Timer m_countdownTimer;
  bool m_active = false;
  std::string m_lastStructureKey;
  float m_lastListWidth = -1.0F;
};
