#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

// What the launcher shows: plain search, the provider overview ("/"), the Files scope, or one provider's view.
namespace launcher_view {

  struct Default {};
  struct ProviderOverview {};
  struct Files {};
  template <typename Return> struct Browser {
    std::string providerViewId;
    // The view to restore when the user leaves this provider view again.
    std::optional<Return> returnView;
  };

  template <typename Return> using View = std::variant<Default, ProviderOverview, Files, Browser<Return>>;

  // ProviderOverview comes only from a plain search; a provider view never opens over the Files scope.
  template <typename Return> [[nodiscard]] bool canEnter(const View<Return>& from, const View<Return>& to) {
    return std::visit(
        [](const auto& f, const auto& t) {
          using From = std::decay_t<decltype(f)>;
          using To = std::decay_t<decltype(t)>;
          if constexpr (std::is_same_v<To, Default>) {
            return true;
          } else if constexpr (std::is_same_v<To, ProviderOverview>) {
            return std::is_same_v<From, Default>;
          } else if constexpr (std::is_same_v<To, Files>) {
            return true;
          } else {
            return !std::is_same_v<From, Files>;
          }
        },
        from, to
    );
  }

  template <typename Return> class State {
  public:
    using ViewType = View<Return>;

    bool set(ViewType next) {
      if (!canEnter(m_view, next)) {
        return false;
      }
      m_view = std::move(next);
      return true;
    }

    [[nodiscard]] const ViewType& view() const noexcept { return m_view; }

    template <typename T> [[nodiscard]] bool is() const noexcept { return std::holds_alternative<T>(m_view); }

    [[nodiscard]] std::string_view providerViewId() const noexcept {
      const auto* browser = std::get_if<Browser<Return>>(&m_view);
      return browser == nullptr ? std::string_view() : std::string_view(browser->providerViewId);
    }

    // Takes the return view out of a provider view; the provider view stays open.
    std::optional<Return> takeReturnView() {
      auto* browser = std::get_if<Browser<Return>>(&m_view);
      if (browser == nullptr) {
        return std::nullopt;
      }
      return std::exchange(browser->returnView, std::nullopt);
    }

  private:
    ViewType m_view = Default{};
  };

} // namespace launcher_view
