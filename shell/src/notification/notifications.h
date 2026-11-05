#pragma once

#include "dbus/service_notice.h"
#include "notification/notification_manager.h"

#include <string>
#include <utility>

namespace notify {

  inline NotificationManager*& instancePtr() {
    static NotificationManager* ptr = nullptr;
    return ptr;
  }

  inline void setInstance(NotificationManager* manager) {
    instancePtr() = manager;
    service_notice::setSink(
        manager == nullptr ? service_notice::Sink{}
                           : [manager](service_notice::Level level, std::string app, std::string title, std::string body) {
                               manager->addInternal(
                                   std::move(app), std::move(title), std::move(body),
                                   level == service_notice::Level::Error ? Urgency::Critical : Urgency::Normal
                               );
                             }
    );
  }

  inline NotificationManager* instance() { return instancePtr(); }

  inline void error(std::string app, std::string title, std::string body) {
    if (auto* m = instancePtr()) {
      m->addInternal(std::move(app), std::move(title), std::move(body), Urgency::Critical);
    }
  }

  inline void info(std::string app, std::string title, std::string body) {
    if (auto* m = instancePtr()) {
      m->addInternal(std::move(app), std::move(title), std::move(body));
    }
  }

} // namespace notify
