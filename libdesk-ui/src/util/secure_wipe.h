#pragma once

#include <cstddef>
#include <string>
#include <string.h>

namespace util {

  // explicit_bzero: the compiler cannot drop the stores.
  inline void secureWipe(void* data, std::size_t len) { explicit_bzero(data, len); }

  // Wipes the whole buffer, also the bytes between size() and capacity().
  inline void secureWipe(std::string& value) {
    value.resize(value.capacity());
    explicit_bzero(value.data(), value.size());
    value.clear();
  }

} // namespace util
