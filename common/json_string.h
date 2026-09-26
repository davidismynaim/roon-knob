#pragma once

// Reads the value of a JSON string the way the Now Playing/zone parsers need it: stops at the
// first UNESCAPED quote and resolves the escape sequences, so a title such as
// Down to Earth (From "WALL-E") - sent as (From \"WALL-E\") - is not cut off at the first \".

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// `after_open_quote` points just past the opening quote. Copies the unescaped text into `out`
// (always NUL-terminated, truncated to `out_len - 1` bytes) and returns a pointer just past the
// closing quote, or NULL when the string is not terminated (or out_len is 0).
const char *json_string_copy(const char *after_open_quote, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
