#include "input/key_pipeline.h"

namespace andromeda {

  KeyResult KeyPipeline::runFrom(KeyStage first, KeyEvent& event) const {
    for (size_t slot = static_cast<size_t>(first); slot < m_stages.size(); ++slot) {
      if (!m_stages[slot]) {
        continue;
      }
      if (m_stages[slot](event) == KeyResult::Consume) {
        return KeyResult::Consume;
      }
    }
    return KeyResult::Pass;
  }

} // namespace andromeda
