#include "app/service_host.h"

#include <algorithm>

ServiceHost::~ServiceHost() { stop(); }

void ServiceHost::add(std::unique_ptr<ServiceModule> module) { m_modules.push_back(std::move(module)); }

bool ServiceHost::computeOrder() {
  m_order.resize(m_started);
  const auto placed = [this](std::string_view name) {
    return std::ranges::any_of(m_order, [name](const ServiceModule* module) { return module->name() == name; });
  };
  for (const auto& module : m_modules) {
    for (std::string_view need : module->needs()) {
      if (!std::ranges::any_of(m_modules, [need](const auto& other) { return other->name() == need; })) {
        m_error = std::string(module->name()) + " needs missing module " + std::string(need);
        m_order.resize(m_started);
        return false;
      }
    }
  }
  // The first module in registration order whose needs are all placed goes next.
  while (m_order.size() < m_modules.size()) {
    const auto next = std::ranges::find_if(m_modules, [&](const auto& module) {
      return !placed(module->name()) && std::ranges::all_of(module->needs(), placed);
    });
    if (next == m_modules.end()) {
      const auto stuck =
          std::ranges::find_if(m_modules, [&](const auto& module) { return !placed(module->name()); });
      m_error = std::string((*stuck)->name()) + " is in or needs a dependency cycle";
      m_order.resize(m_started);
      return false;
    }
    m_order.push_back(next->get());
  }
  return true;
}

bool ServiceHost::start(const Config& config) {
  m_error.clear();
  if (!computeOrder()) {
    return false;
  }
  while (m_started < m_order.size()) {
    m_order[m_started++]->start(config);
  }
  return true;
}

void ServiceHost::stop() {
  while (m_started > 0) {
    m_order[--m_started]->stop();
  }
}

void ServiceHost::reload(const Config& config, const ConfigChangeSet& change) {
  for (std::size_t i = 0; i < m_started; ++i) {
    if (m_order[i]->reads(change)) {
      m_order[i]->reload(config);
    }
  }
}
