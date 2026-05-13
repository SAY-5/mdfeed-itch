#include <cstddef>
#include <cstdint>

#include "core/parser.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    std::size_t off = 0;
    while (off < size) {
        auto pr = mdfeed::itch::decode_frame(std::span<const std::uint8_t>(data + off, size - off));
        if (pr.consumed == 0) break;
        off += pr.consumed;
    }
    return 0;
}
