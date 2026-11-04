#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

// libqalculate behind a module (noctalia-calc.so) that the shell loads with dlopen, so its library tree stays off
// the start path. Only one instance may exist at a time: libqalculate keeps a global Calculator pointer.
class CalcEngine {
public:
  virtual ~CalcEngine() = default;

  virtual void loadExchangeRates() = 0;
  [[nodiscard]] virtual bool canFetchExchangeRates() = 0;
  // (url, file) pairs.
  [[nodiscard]] virtual std::vector<std::pair<std::string, std::string>> exchangeRateSources() = 0;
  // The printed result of a localized expression, or nullopt on an error.
  [[nodiscard]] virtual std::optional<std::string> evaluate(const std::string& localized, std::string& unlocalized) = 0;
};

// Loads global and user definitions, with the functions that run programs or touch files disabled. The version
// suffix changes with this interface, so the shell rejects a stale installed module instead of calling a wrong slot.
extern "C" CalcEngine* noctalia_calc_engine_create_v1();
inline constexpr const char* kCalcEngineCreateSymbol = "noctalia_calc_engine_create_v1";
