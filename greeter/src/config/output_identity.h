#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool greeter_output_stable_identifier(
    const char* connector, const char* make, const char* model, const char* serial, const char* description, char* out,
    size_t out_size
);

bool greeter_output_identifier_matches(
    const char* connector, const char* make, const char* model, const char* serial, const char* description,
    const char* identifier
);

bool greeter_output_name_is_internal(const char* connector);

#ifdef __cplusplus
}
#endif
