#include "launcher/calc_engine.h"

#include <libqalculate/BuiltinFunctions.h>
#include <libqalculate/Calculator.h>
#include <libqalculate/Function.h>
#include <libqalculate/util.h>

namespace {

  class QalculateEngine final : public CalcEngine {
  public:
    QalculateEngine() {
      // Load any cached rates before definitions so currency units pick them up.
      m_calc.loadExchangeRates();
      m_calc.loadGlobalDefinitions();
      m_calc.loadLocalDefinitions();
      // These run programs or touch files, so a pasted expression could execute code while it is typed.
      // Must run after loadLocalDefinitions(), which re-activates any inactive non-local function.
      for (const int id : {FUNCTION_ID_COMMAND, FUNCTION_ID_PLOT, FUNCTION_ID_LOAD, FUNCTION_ID_EXPORT}) {
        if (MathFunction* function = m_calc.getFunctionById(id); function != nullptr) {
          function->setActive(false);
        }
      }
    }

    void loadExchangeRates() override { m_calc.loadExchangeRates(); }

    bool canFetchExchangeRates() override { return m_calc.canFetch(); }

    std::vector<std::pair<std::string, std::string>> exchangeRateSources() override {
      std::vector<std::pair<std::string, std::string>> sources;
      for (int i = 1;; ++i) {
        std::string url = m_calc.getExchangeRatesUrl(i);
        std::string file = m_calc.getExchangeRatesFileName(i);
        if (url.empty() || file.empty()) {
          return sources;
        }
        sources.emplace_back(std::move(url), std::move(file));
      }
    }

    std::optional<std::string> evaluate(const std::string& localized, std::string& unlocalized) override {
      // Drop any messages left over from a previous evaluation.
      m_calc.clearMessages();

      EvaluationOptions eo = default_user_evaluation_options;
      eo.parse_options.dot_as_separator = m_calc.default_dot_as_separator;
      PrintOptions po = default_print_options;
      po.use_unicode_signs = false;
      // Collapse interval arithmetic to a single rounded value instead of "interval(a, b)".
      po.interval_display = INTERVAL_DISPLAY_SIGNIFICANT_DIGITS;

      // libqalculate expects '.' decimals internally; convert locale signs first.
      unlocalized = m_calc.unlocalizeExpression(localized, eo.parse_options);
      if (unlocalized.empty()) {
        return std::nullopt;
      }

      std::string output = m_calc.calculateAndPrint(unlocalized, /*msecs=*/200, eo, po);
      for (CalculatorMessage* m = m_calc.message(); m != nullptr; m = m_calc.nextMessage()) {
        if (m->type() == MESSAGE_ERROR) {
          return std::nullopt;
        }
      }
      return output;
    }

  private:
    Calculator m_calc;
  };

} // namespace

extern "C" __attribute__((visibility("default"))) CalcEngine* noctalia_calc_engine_create_v1() {
  return new QalculateEngine();
}
