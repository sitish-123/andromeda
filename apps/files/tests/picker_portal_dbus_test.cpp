// FileChooser portal contract over a private session bus (meson runs it under dbus-run-session).
#include "core/deferred_call.h"
#include "dbus/session_bus.h"
#include "picker_portal.h"

#include <atomic>
#include <chrono>
#include <map>
#include <print>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <vector>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "picker_portal_dbus_test: {}", what);
      ++g_failures;
    }
  }

  using Options = std::map<std::string, sdbus::Variant>;
  using Pattern = sdbus::Struct<std::uint32_t, std::string>;
  using FilterStruct = sdbus::Struct<std::string, std::vector<Pattern>>;

  const sdbus::ServiceName kBusName{"org.freedesktop.impl.portal.desktop.andromeda.Files"};
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.FileChooser";

  std::vector<std::uint8_t> bytes(const std::string& path) {
    std::vector<std::uint8_t> out(path.begin(), path.end());
    out.push_back(0);
    return out;
  }

  template <typename Predicate> bool pumpUntil(SessionBus& bus, Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      for (auto& callback : DeferredCall::takePending()) {
        callback();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready();
  }

  struct Reply {
    std::atomic<bool> done = false;
    std::uint32_t response = 99;
    Options results;
  };
} // namespace

int main() {
  SessionBus bus;
  std::vector<std::pair<std::uint64_t, PickerPortal::Request>> requests;
  std::vector<std::uint64_t> closed;
  PickerPortal portal(
      bus, [&](std::uint64_t id, const PickerPortal::Request& r) { requests.emplace_back(id, r); },
      [&](std::uint64_t id) { closed.push_back(id); }
  );

  auto client = sdbus::createSessionBusConnection();
  auto proxy = sdbus::createProxy(*client, kBusName, kObjectPath);
  client->enterEventLoopAsync();

  const auto call = [&](const char* method, const std::string& handle, const Options& options, Reply& reply) {
    proxy->callMethodAsync(method)
        .onInterface(kInterface)
        .withArguments(sdbus::ObjectPath{handle}, std::string{"org.example.App"}, std::string{"wayland:abc"},
                       std::string{"Pick"}, options)
        .uponReplyInvoke([&reply](std::optional<sdbus::Error> error, std::uint32_t response, Options results) {
          if (!error.has_value()) {
            reply.response = response;
            reply.results = std::move(results);
          }
          reply.done = true;
        });
  };

  // OpenFile: the options reach the window, and the window's answer reaches the caller.
  const FilterStruct images{"Images", {Pattern{0, "*.png"}}};
  const FilterStruct text{"Text", {Pattern{1, "text/plain"}}};
  Options openOptions{
      {"multiple", sdbus::Variant(true)},
      {"filters", sdbus::Variant(std::vector<FilterStruct>{images, text})},
      {"current_filter", sdbus::Variant(text)},
      {"current_file", sdbus::Variant(bytes("/home/u/docs/a.txt"))},
  };
  Reply open;
  call("OpenFile", "/org/freedesktop/portal/desktop/request/1_1/t1", openOptions, open);
  expect(pumpUntil(bus, [&] { return requests.size() == 1; }), "OpenFile reached the window");
  if (requests.size() == 1) {
    const auto& [id, r] = requests.front();
    expect(r.kind == PickerPortal::Request::Kind::Open, "kind Open");
    expect(r.appId == "org.example.App" && r.parentWindow == "wayland:abc", "app id and parent");
    expect(r.multiple, "multiple");
    expect(r.filters.size() == 2 && r.currentFilter == 1u, "filters and current filter");
    expect(r.currentName == "a.txt" && r.currentFolder == "/home/u/docs", "current_file sets name and folder");
    portal.answer(id, {.response = 0, .uris = {"file:///home/u/docs/a.txt", "file:///home/u/b.txt"}, .filter = 1});
  }
  expect(pumpUntil(bus, [&] { return open.done.load(); }), "OpenFile replied");
  expect(open.response == 0, "OpenFile response 0");
  const auto uris = open.results.contains("uris") ? open.results["uris"].get<std::vector<std::string>>()
                                                  : std::vector<std::string>{};
  expect(uris == std::vector<std::string>{"file:///home/u/docs/a.txt", "file:///home/u/b.txt"}, "uris returned");
  expect(open.results.contains("current_filter")
             && open.results["current_filter"].get<FilterStruct>().get<0>() == "Text",
         "current_filter returned");
  expect(portal.idle(), "answered call leaves no pending request");

  // SaveFile: multiple never applies; a cancel returns response 1 and no uris.
  Reply save;
  call("SaveFile", "/org/freedesktop/portal/desktop/request/1_1/t2",
       Options{{"multiple", sdbus::Variant(true)}, {"current_name", sdbus::Variant(std::string{"new.txt"})}}, save);
  expect(pumpUntil(bus, [&] { return requests.size() == 2; }), "SaveFile reached the window");
  if (requests.size() == 2) {
    const auto& [id, r] = requests.back();
    expect(r.kind == PickerPortal::Request::Kind::Save && !r.multiple, "SaveFile is single");
    expect(r.currentName == "new.txt", "current_name");
    portal.answer(id, {.response = 1, .uris = {"file:///ignored"}});
  }
  expect(pumpUntil(bus, [&] { return save.done.load(); }), "SaveFile replied");
  expect(save.response == 1 && !save.results.contains("uris"), "cancel returns no uris");

  // The caller closes its Request: the call ends with response 2 and the window hears about it.
  const std::string handle = "/org/freedesktop/portal/desktop/request/1_1/t3";
  Reply dropped;
  call("OpenFile", handle, Options{}, dropped);
  expect(pumpUntil(bus, [&] { return requests.size() == 3; }), "third request reached the window");
  auto request = sdbus::createProxy(*client, kBusName, sdbus::ObjectPath{handle});
  request->callMethodAsync("Close").onInterface("org.freedesktop.impl.portal.Request").uponReplyInvoke(
      [](std::optional<sdbus::Error>) {}
  );
  expect(pumpUntil(bus, [&] { return dropped.done.load() && closed.size() == 1; }), "Close ended the call");
  expect(dropped.response == 2, "Close returns response 2");
  expect(closed.size() == 1 && closed.front() == requests.back().first, "window told which request closed");
  expect(portal.idle(), "closed call leaves no pending request");

  client->leaveEventLoop();
  if (g_failures == 0) {
    std::println("picker_portal_dbus_test: ok");
  }
  return g_failures == 0 ? 0 : 1;
}
