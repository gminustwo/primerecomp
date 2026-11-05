// zlib streams inflated in memory, with miniz's tinfl (third_party/miniz: miniz 3.1.2's inflater, MIT): the game's
// materials, where its shader binaries are (runtime/nvn/vulkan/exec.c, the shipped shader list), and that list itself.
#include "inflate.h"

#include "../../third_party/miniz/miniz_tinfl.c"

uint8_t *rt_inflate(const void *src, size_t n, size_t *out) {
    *out = 0;
    return tinfl_decompress_mem_to_heap(src, n, out, TINFL_FLAG_PARSE_ZLIB_HEADER);
}
