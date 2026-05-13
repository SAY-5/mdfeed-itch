#pragma once

#include <cstdint>

namespace mdfeed::obs {

// Monotonic, raw if available, in nanoseconds.
std::uint64_t now_ns();

}  // namespace mdfeed::obs
