#pragma once

#include <cstddef>
#include <string>

namespace util {

  // Writes through a volatile pointer so the compiler cannot drop the stores.
  inline void secureWipe(std::string& value) {
    volatile char* ptr = value.empty() ? nullptr : value.data();
    for (std::size_t i = 0; i < value.size(); ++i) {
      ptr[i] = '\0';
    }
    value.clear();
  }

} // namespace util
