#include "server/desktop_output_manager.h"

#include "desktop-unstable-v1-protocol.h"
#include "output/output.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <string>

namespace umbriel {

  namespace {

    constexpr uint32_t kVersion = 1;

    DesktopOutputManager* managerFrom(wl_resource* resource) {
      return static_cast<DesktopOutputManager*>(wl_resource_get_user_data(resource));
    }

    Output* outputNamed(const Server& server, std::string_view connector) {
      for (const auto& output : server.outputs()) {
        if (connector == output->wlr()->name) {
          return output.get();
        }
      }
      return nullptr;
    }

  } // namespace

  DesktopOutputManager::DesktopOutputManager(Server& server) : m_server(server) {
    m_global = wl_global_create(server.display(), &dsk_output_manager_v1_interface, kVersion, this, bind);
  }

  DesktopOutputManager::~DesktopOutputManager() {
    for (wl_resource* resource : m_resources) {
      wl_resource_set_user_data(resource, nullptr);
    }
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void DesktopOutputManager::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<DesktopOutputManager*>(data);
    wl_resource* resource = wl_resource_create(client, &dsk_output_manager_v1_interface, static_cast<int>(version), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, &kImplementation, self, handleResourceDestroyed);
    self->m_resources.push_back(resource);
    for (const auto& output : self->m_server.outputs()) {
      sendState(resource, *output);
    }
    dsk_output_manager_v1_send_done(resource);
  }

  void
  DesktopOutputManager::handleSetMirror(wl_client*, wl_resource* resource, const char* target, const char* source) {
    if (DesktopOutputManager* self = managerFrom(resource)) {
      self->apply(resource, target, source);
    }
  }

  void DesktopOutputManager::handleClearMirror(wl_client*, wl_resource* resource, const char* target) {
    if (DesktopOutputManager* self = managerFrom(resource)) {
      self->apply(resource, target, {});
    }
  }

  void DesktopOutputManager::handleDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

  void DesktopOutputManager::handleResourceDestroyed(wl_resource* resource) {
    if (DesktopOutputManager* self = managerFrom(resource)) {
      std::erase(self->m_resources, resource);
    }
  }

  void DesktopOutputManager::apply(wl_resource* resource, std::string_view target, std::string_view source) {
    const std::string targetName(target);
    Output* targetOutput = outputNamed(m_server, target);
    Output* sourceOutput = source.empty() ? nullptr : outputNamed(m_server, source);
    const char* reason = nullptr;
    if (targetOutput == nullptr || (!source.empty() && sourceOutput == nullptr)) {
      reason = "unknown output";
    } else if (sourceOutput == targetOutput) {
      reason = "an output cannot mirror itself";
    } else if (sourceOutput != nullptr && (sourceOutput->mirrorSource() != nullptr || !sourceOutput->onDesktop())) {
      reason = "the source is not on the desktop";
    } else if (sourceOutput != nullptr && std::ranges::any_of(m_server.outputs(), [&](const auto& output) {
                 return output->mirrorSource() == targetOutput;
               })) {
      reason = "the target is being mirrored";
    }
    if (reason != nullptr) {
      dsk_output_manager_v1_send_failed(resource, targetName.c_str(), reason);
      return;
    }
    m_server.setOutputMirror(*targetOutput, sourceOutput);
  }

  void DesktopOutputManager::broadcast(const Output& output) {
    for (wl_resource* resource : m_resources) {
      sendState(resource, output);
    }
  }

  void DesktopOutputManager::sendState(wl_resource* resource, const Output& output) {
    const Output* source = output.mirrorSource();
    dsk_output_manager_v1_send_mirror(resource, output.wlr()->name, source != nullptr ? source->wlr()->name : "");
  }

  const struct dsk_output_manager_v1_interface DesktopOutputManager::kImplementation = {
      .destroy = DesktopOutputManager::handleDestroy,
      .set_mirror = DesktopOutputManager::handleSetMirror,
      .clear_mirror = DesktopOutputManager::handleClearMirror,
  };

} // namespace umbriel
