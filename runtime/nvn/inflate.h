// zlib streams inflated in memory (miniz's tinfl, third_party/miniz)
#pragma once
#include <stddef.h>
#include <stdint.h>

// The zlib stream (RFC 1950) at src, n bytes or fewer (bytes after its end are left alone): its data, malloc'd,
// *out its size. NULL if it isn't one, or is cut off.
uint8_t *rt_inflate(const void *src, size_t n, size_t *out);
