#include "obs/clock.h"

#include <chrono>

namespace mdfeed::obs {

// std::chrono::steady_clock uses mach_continuous_time on Darwin and
// clock_gettime(CLOCK_MONOTONIC) elsewhere; on both platforms the inner
// implementation is vDSO/usermode-fast, well below the cost of a syscall.
std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

}  // namespace mdfeed::obs
