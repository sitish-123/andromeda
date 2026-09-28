#include "output/enable_overrides.h"

#include <algorithm>

namespace umbriel {

  namespace {

    std::string descriptorFor(const OutputIdentity& identity) {
      if (identity.make.empty() && identity.model.empty() && identity.serial.empty()) {
        return {};
      }
      return outputDescriptor(identity);
    }

  } // namespace

  void OutputEnableOverrides::remember(const OutputIdentity& identity, bool enabled) {
    const std::string descriptor = descriptorFor(identity);
    const auto exact = std::ranges::find_if(m_entries, [&](const Entry& entry) {
      return outputNamesEqual(entry.connector, identity.connector) && outputNamesEqual(entry.descriptor, descriptor);
    });
    if (exact != m_entries.end()) {
      exact->enabled = enabled;
      return;
    }

    m_entries.push_back({
        .connector = std::string(identity.connector),
        .descriptor = descriptor,
        .enabled = enabled,
    });
    if (descriptor.empty()) {
      return;
    }

    size_t matches = 0;
    for (const Entry& entry : m_entries) {
      if (outputNamesEqual(entry.descriptor, descriptor)) {
        ++matches;
      }
    }
    if (matches < 2) {
      return;
    }
    for (Entry& entry : m_entries) {
      if (outputNamesEqual(entry.descriptor, descriptor)) {
        // Once two monitors have reported the same descriptor, connector
        // migration is unsafe for the lifetime of these overrides.
        entry.descriptorAmbiguous = true;
      }
    }
  }

  std::optional<bool> OutputEnableOverrides::resolve(const OutputIdentity& identity) {
    const std::string descriptor = descriptorFor(identity);
    if (descriptor.empty()) {
      const auto connector = std::ranges::find_if(m_entries, [&](const Entry& entry) {
        return entry.descriptor.empty() && outputNamesEqual(entry.connector, identity.connector);
      });
      return connector != m_entries.end() ? std::optional(connector->enabled) : std::nullopt;
    }

    Entry* unique = nullptr;
    size_t matches = 0;
    for (Entry& entry : m_entries) {
      if (!outputNamesEqual(entry.descriptor, descriptor)) {
        continue;
      }
      ++matches;
      unique = &entry;
      if (outputNamesEqual(entry.connector, identity.connector)) {
        return entry.enabled;
      }
    }
    if (matches != 1 || unique == nullptr || unique->descriptorAmbiguous) {
      return std::nullopt;
    }

    unique->connector = identity.connector;
    return unique->enabled;
  }

} // namespace umbriel
