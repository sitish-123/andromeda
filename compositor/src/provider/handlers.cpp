#include "output/output.h"
#include "provider/provider.h"
#include "server/focus.h"
#include "server/ipc_commands.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <charconv>
#include <unordered_set>

namespace andromeda {

  namespace {

    size_t editDistance(std::string_view a, std::string_view b) {
      std::vector<size_t> row(b.size() + 1);
      for (size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
      }
      for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
          const size_t above = row[j];
          row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
          diagonal = above;
        }
      }
      return row[b.size()];
    }

    std::optional<uint32_t> idNumber(std::string_view id, std::string_view prefix) {
      if (!id.starts_with(prefix)) {
        return std::nullopt;
      }
      id.remove_prefix(prefix.size());
      uint32_t value = 0;
      const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), value);
      if (error != std::errc{} || end != id.data() + id.size()) {
        return std::nullopt;
      }
      return value;
    }

    idl::Id outputId(const Output& output) { return {std::string("out:") + output.wlr()->name}; }

  } // namespace

  std::string closestMatches(std::string_view wanted, std::vector<std::string> candidates) {
    std::ranges::stable_sort(candidates, {}, [&](const std::string& candidate) {
      return editDistance(wanted, candidate);
    });
    std::string out;
    for (size_t i = 0; i < std::min<size_t>(3, candidates.size()); ++i) {
      out += (i == 0 ? "" : ", ") + candidates[i];
    }
    return out;
  }

  idl::Id ProviderContext::windowId(const View& view) {
    const auto [entry, inserted] = m_windowIds.try_emplace(view.extForeignIdentifier(), m_nextWindowId);
    if (inserted) {
      ++m_nextWindowId;
    }
    return {"w" + std::to_string(entry->second)};
  }

  idl::Id ProviderContext::workspaceId(const Workspace& workspace) {
    const auto [entry, inserted] = m_workspaceIds.try_emplace(workspace.id(), m_nextWorkspaceId);
    if (inserted) {
      ++m_nextWorkspaceId;
    }
    return {"ws" + std::to_string(entry->second)};
  }

  idl::Window ProviderContext::windowEntry(const View& view) {
    const Workspace* workspace = view.workspace();
    const Output* output = view.currentOutput();
    return {
        .id = windowId(view),
        .app_id = view.appId() != nullptr ? view.appId() : "",
        .title = view.title() != nullptr ? view.title() : "",
        .workspace = workspace != nullptr ? std::optional(workspaceId(*workspace)) : std::nullopt,
        .output = output != nullptr ? std::optional(outputId(*output)) : std::nullopt,
        .focused = focusedWindow(*m_server) == &view,
        .floating = view.floating(),
        .fullscreen = view.scheduledFullscreen(),
        .minimized = view.minimized(),
    };
  }

  idl::Workspace ProviderContext::workspaceEntry(const Workspace& workspace) {
    const Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
    const Output* output = workspace.group()->output();
    const auto& views = workspace.allViews();
    return {
        .id = workspaceId(workspace),
        .index = static_cast<uint32_t>(workspace.index() + 1),
        .name = workspace.name(),
        .output = outputId(*output),
        .active = workspace.active(),
        .focused = output == preferred && workspace.active(),
        .window_count = static_cast<uint32_t>(std::ranges::count_if(views, [](const View* v) { return v->mapped(); })),
    };
  }

  std::vector<idl::Window> ProviderContext::windows() {
    std::vector<idl::Window> out;
    std::unordered_set<std::string_view> live;
    for (const auto& view : m_server->views()) {
      if (view->extForeignIdentifier() == nullptr) {
        continue;
      }
      live.insert(view->extForeignIdentifier());
      if (view->mapped()) {
        out.push_back(windowEntry(*view));
      }
    }
    std::erase_if(m_windowIds, [&](const auto& entry) { return !live.contains(entry.first); });
    return out;
  }

  std::vector<idl::Workspace> ProviderContext::workspaces() {
    std::vector<idl::Workspace> out;
    std::unordered_set<std::string_view> live;
    for (const auto& output : m_server->outputs()) {
      const WorkspaceGroup* group = output->workspaceGroup();
      for (size_t index = 0; group != nullptr && index < group->workspaceCount(); ++index) {
        if (const Workspace* workspace = group->workspaceAt(index)) {
          live.insert(workspace->id());
          out.push_back(workspaceEntry(*workspace));
        }
      }
    }
    std::erase_if(m_workspaceIds, [&](const auto& entry) { return !live.contains(entry.first); });
    return out;
  }

  std::vector<idl::Output> ProviderContext::outputs() {
    std::vector<idl::Output> out;
    const Output* preferred = m_server->outputFromWlr(m_server->preferredOutput());
    for (const auto& output : m_server->outputs()) {
      const wlr_box box = output->layoutBox();
      const WorkspaceGroup* group = output->workspaceGroup();
      const Workspace* active = group != nullptr ? group->active() : nullptr;
      out.push_back({
          .id = outputId(*output),
          .name = output->wlr()->name,
          .x = box.x,
          .y = box.y,
          .width = static_cast<uint32_t>(std::max(box.width, 0)),
          .height = static_cast<uint32_t>(std::max(box.height, 0)),
          .scale = output->wlr()->scale,
          .enabled = output->desktopEnabled(),
          .focused = output.get() == preferred,
          .active_workspace = active != nullptr ? std::optional(workspaceId(*active)) : std::nullopt,
      });
    }
    return out;
  }

  std::expected<View*, idl::Error> ProviderContext::resolveWindow(const idl::Id& id, std::string_view example) {
    std::vector<std::string> known;
    for (const idl::Window& window : windows()) {
      known.push_back(window.id.value);
    }
    if (const auto number = idNumber(id.value, "w")) {
      for (const auto& [foreign, serial] : m_windowIds) {
        if (serial != *number) {
          continue;
        }
        for (const auto& view : m_server->views()) {
          const char* identifier = view->extForeignIdentifier();
          if (view->mapped() && identifier != nullptr && foreign == identifier) {
            return view.get();
          }
        }
      }
    }
    return std::unexpected(idl::Error{
        idl::ErrorCode::NotFound, "no window " + id.value,
        known.empty() ? "no windows are open" : "closest ids: " + closestMatches(id.value, std::move(known)),
        std::string(example)
    });
  }

  std::expected<void, idl::Error>
  ProviderContext::runBound(std::string_view binds, std::string_view arg, std::string_view example) {
    std::string action(binds);
    if (!arg.empty()) {
      action += ':';
      action += arg;
    }
    const nlohmann::json reply = IpcCommands::msg(*m_server, action);
    if (const auto error = reply.find("err"); error != reply.end()) {
      const std::string message = error->is_string() ? error->get<std::string>() : error->dump();
      return std::unexpected(idl::Error{
          message.starts_with("unknown") ? idl::ErrorCode::NotFound : idl::ErrorCode::Conflict, message,
          "check the current state with `andromeda state`", std::string(example)
      });
    }
    return {};
  }

  namespace idl {

    namespace {
      Error zeroIndex(std::string_view example) {
        return {ErrorCode::InvalidArgument, "index is 1-based", "use 1 for the first workspace", std::string(example)};
      }
    } // namespace

    std::expected<window_list::Result, Error> handle(Context& context, const window_list::Args& /*args*/) {
      return context.windows();
    }

    std::expected<window_focus::Result, Error> handle(Context& context, const window_focus::Args& args) {
      const auto view = context.resolveWindow(args.id, window_focus::kExample);
      if (!view) {
        return std::unexpected(view.error());
      }
      if (auto ran = context.runBound(window_focus::kBinds, (*view)->extForeignIdentifier(), window_focus::kExample);
          !ran) {
        return std::unexpected(ran.error());
      }
      return context.windowEntry(**view);
    }

    std::expected<window_close::Result, Error> handle(Context& context, const window_close::Args& args) {
      const auto view = context.resolveWindow(args.id, window_close::kExample);
      if (!view) {
        return std::unexpected(view.error());
      }
      Window entry = context.windowEntry(**view);
      if (auto ran = context.runBound(window_close::kBinds, (*view)->extForeignIdentifier(), window_close::kExample);
          !ran) {
        return std::unexpected(ran.error());
      }
      return entry;
    }

    std::expected<window_move_to_workspace::Result, Error>
    handle(Context& context, const window_move_to_workspace::Args& args) {
      constexpr std::string_view example = window_move_to_workspace::kExample;
      if (args.index == 0) {
        return std::unexpected(zeroIndex(example));
      }
      andromeda::View* view = focusedWindow(context.server());
      if (args.id.has_value()) {
        const auto resolved = context.resolveWindow(*args.id, example);
        if (!resolved) {
          return std::unexpected(resolved.error());
        }
        view = *resolved;
        // The bound action moves the focused window.
        if (auto ran = context.runBound(window_focus::kBinds, view->extForeignIdentifier(), example); !ran) {
          return std::unexpected(ran.error());
        }
      }
      if (view == nullptr) {
        return std::unexpected(Error{ErrorCode::NotFound, "no focused window", "pass --id", std::string(example)});
      }
      if (auto ran = context.runBound(window_move_to_workspace::kBinds, std::to_string(args.index), example); !ran) {
        return std::unexpected(ran.error());
      }
      return context.windowEntry(*view);
    }

    std::expected<workspace_list::Result, Error> handle(Context& context, const workspace_list::Args& /*args*/) {
      return context.workspaces();
    }

    std::expected<workspace_switch::Result, Error> handle(Context& context, const workspace_switch::Args& args) {
      if (args.index == 0) {
        return std::unexpected(zeroIndex(workspace_switch::kExample));
      }
      if (auto ran = context.runBound(workspace_switch::kBinds, std::to_string(args.index), workspace_switch::kExample);
          !ran) {
        return std::unexpected(ran.error());
      }
      Server& server = context.server();
      const andromeda::Output* output = server.outputFromWlr(server.preferredOutput());
      const WorkspaceGroup* group = output != nullptr ? output->workspaceGroup() : nullptr;
      if (group == nullptr || group->active() == nullptr) {
        return std::unexpected(Error{
            ErrorCode::Unavailable, "no output shows a workspace", "connect or enable an output",
            std::string(workspace_switch::kExample)
        });
      }
      return context.workspaceEntry(*group->active());
    }

    std::expected<output_list::Result, Error> handle(Context& context, const output_list::Args& /*args*/) {
      return context.outputs();
    }

  } // namespace idl

} // namespace andromeda
