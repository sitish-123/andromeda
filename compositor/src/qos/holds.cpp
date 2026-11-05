#include "qos/holds.h"

#include <algorithm>
#include <format>
#include <utility>

namespace andromeda::qos {

  std::optional<std::string> Holds::add(Hold hold, bool known) {
    if (!known) {
      return std::nullopt;
    }
    hold.id = std::format("apps-{}", m_next++);
    std::string id = hold.id;
    m_holds.emplace(id, std::move(hold));
    return id;
  }

  std::optional<std::string> Holds::holdTier(const std::string& owner, const std::string& unit, Tier tier,
                                             std::string reason, bool known) {
    return add({.id = {}, .owner = owner, .unit = unit, .tier = tier, .reason = std::move(reason)}, known);
  }

  std::optional<std::string> Holds::holdFreeze(const std::string& owner, const std::string& unit, std::string reason,
                                               bool known) {
    return add({.id = {}, .owner = owner, .unit = unit, .tier = std::nullopt, .reason = std::move(reason)}, known);
  }

  bool Holds::release(const std::string& id, const std::string& owner) {
    const auto it = m_holds.find(id);
    if (it == m_holds.end() || it->second.owner != owner) {
      return false;
    }
    m_holds.erase(it);
    return true;
  }

  bool Holds::releaseOwner(const std::string& owner) {
    return std::erase_if(m_holds, [&](const auto& entry) { return entry.second.owner == owner; }) > 0;
  }

  void Holds::forget(const std::string& unit) {
    std::erase_if(m_holds, [&](const auto& entry) { return entry.second.unit == unit; });
  }

  Decision Holds::apply(const std::string& unit, Decision policy, bool exempt) const {
    if (exempt) {
      return policy;
    }
    for (const auto& [id, hold] : m_holds) {
      if (hold.unit != unit) {
        continue;
      }
      if (hold.tier) {
        policy.tier = std::max(policy.tier, *hold.tier);
      } else {
        policy.frozen = true;
      }
    }
    return policy;
  }

  std::vector<Holds::Hold> Holds::list() const {
    std::vector<Hold> out;
    out.reserve(m_holds.size());
    for (const auto& [id, hold] : m_holds) {
      out.push_back(hold);
    }
    return out;
  }

  Registry& registry() {
    static Registry shared;
    return shared;
  }

} // namespace andromeda::qos
