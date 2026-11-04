// The service host starts modules after their needs, stops them in reverse, and reloads only affected modules.

#include "app/service_host.h"
#include "config/config_types.h"
#include "tests/test_check.h"

#include <memory>
#include <string>
#include <vector>

namespace {

  class FakeModule : public ServiceModule {
  public:
    FakeModule(std::string name, std::vector<std::string_view> needs, bool ConfigChangeSet::*section,
               std::vector<std::string>& log)
        : m_name(std::move(name)), m_needs(std::move(needs)), m_section(section), m_log(log) {}

    [[nodiscard]] std::string_view name() const override { return m_name; }
    [[nodiscard]] std::vector<std::string_view> needs() const override { return m_needs; }
    [[nodiscard]] bool reads(const ConfigChangeSet& change) const override { return change.*m_section; }
    void start(const Config& /*config*/) override { m_log.push_back("start " + m_name); }
    void stop() override { m_log.push_back("stop " + m_name); }
    void reload(const Config& /*config*/) override { m_log.push_back("reload " + m_name); }

  private:
    std::string m_name;
    std::vector<std::string_view> m_needs;
    bool ConfigChangeSet::*m_section;
    std::vector<std::string>& m_log;
  };

  ConfigChangeSet only(bool ConfigChangeSet::*section) {
    ConfigChangeSet change;
    for (bool* flag : {&change.bars, &change.audio, &change.bluetooth, &change.theme}) {
      *flag = false;
    }
    change.*section = true;
    return change;
  }

} // namespace

int main() {
  const Config config;
  std::vector<std::string> log;
  {
    ServiceHost host;
    host.add(std::make_unique<FakeModule>("panels", std::vector<std::string_view>{"audio", "bluetooth"},
                                          &ConfigChangeSet::bars, log));
    host.add(std::make_unique<FakeModule>("audio", std::vector<std::string_view>{"theme"}, &ConfigChangeSet::audio,
                                          log));
    host.add(std::make_unique<FakeModule>("bluetooth", std::vector<std::string_view>{}, &ConfigChangeSet::bluetooth,
                                          log));
    host.add(std::make_unique<FakeModule>("theme", std::vector<std::string_view>{}, &ConfigChangeSet::theme, log));
    TEST_CHECK(host.start(config));
    TEST_CHECK((log == std::vector<std::string>{"start bluetooth", "start theme", "start audio", "start panels"}));

    log.clear();
    host.add(std::make_unique<FakeModule>("mpris", std::vector<std::string_view>{"session"}, &ConfigChangeSet::bars,
                                          log));
    host.add(std::make_unique<FakeModule>("session", std::vector<std::string_view>{"theme"}, &ConfigChangeSet::bars,
                                          log));
    TEST_CHECK(host.start(config));
    TEST_CHECK((log == std::vector<std::string>{"start session", "start mpris"}));

    log.clear();
    host.reload(config, only(&ConfigChangeSet::audio));
    TEST_CHECK((log == std::vector<std::string>{"reload audio"}));

    log.clear();
    ConfigChangeSet two = only(&ConfigChangeSet::bars);
    two.theme = true;
    host.reload(config, two);
    TEST_CHECK((log == std::vector<std::string>{"reload theme", "reload panels", "reload session", "reload mpris"}));
    log.clear();
  }
  TEST_CHECK((log == std::vector<std::string>{"stop mpris", "stop session", "stop panels", "stop audio", "stop theme",
                                              "stop bluetooth"}));

  log.clear();
  {
    ServiceHost host;
    host.add(std::make_unique<FakeModule>("a", std::vector<std::string_view>{"b"}, &ConfigChangeSet::bars, log));
    host.add(std::make_unique<FakeModule>("b", std::vector<std::string_view>{"a"}, &ConfigChangeSet::bars, log));
    host.add(std::make_unique<FakeModule>("c", std::vector<std::string_view>{}, &ConfigChangeSet::bars, log));
    TEST_CHECK(!host.start(config));
    TEST_CHECK(host.error().find("cycle") != std::string::npos);
  }
  {
    ServiceHost host;
    host.add(std::make_unique<FakeModule>("a", std::vector<std::string_view>{"gone"}, &ConfigChangeSet::bars, log));
    TEST_CHECK(!host.start(config));
    TEST_CHECK(host.error().find("gone") != std::string::npos);
  }
  TEST_CHECK(log.empty());
  return 0;
}
