#include "dbus/service_notice.h"

namespace service_notice {

  namespace {
    Sink& sink() {
      static Sink instance;
      return instance;
    }
  } // namespace

  void setSink(Sink next) { sink() = std::move(next); }

  void post(Level level, std::string app, std::string title, std::string body) {
    if (sink()) {
      sink()(level, std::move(app), std::move(title), std::move(body));
    }
  }

} // namespace service_notice
