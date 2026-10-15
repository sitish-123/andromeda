#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/tab.h"

#include <cstdint>
#include <optional>
#include <string>

class Flex;
class Label;
class LinkService;
class QuickShareService;
class ScrollView;

class DevicesTab : public Tab {
public:
  DevicesTab(LinkService* link, QuickShareService* quickShare);

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
  [[nodiscard]] std::unique_ptr<Flex> makeTexts(const std::string& id, float scale);
  void sendDraft();
  void pickAndSend(const std::string& id);
  void finishPendingSend();

  LinkService* m_link = nullptr;
  QuickShareService* m_quickShare = nullptr;
  // A file picked to send, waiting for the user to choose a Quick Share or LocalSend device.
  std::string m_pendingSend;
  Flex* m_rootLayout = nullptr;
  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;
  Label* m_countdown = nullptr;
  Timer m_countdownTimer;
  bool m_active = false;
  std::string m_lastStructureKey;
  // The phone whose texts are open, the conversation shown (none for the list), and the text being written.
  std::string m_textsDevice;
  std::optional<std::uint64_t> m_textsThread;
  std::string m_textsTo;
  std::string m_draft;
  std::string m_textsError;
  bool m_sending = false;
  float m_lastListWidth = -1.0F;
};
