// Cache-line padding study.
//
// The depth book's hot read path is a linear scan of a per-side level vector
// (top_bids_ / top_asks_, each a std::vector<PriceLevel>). PriceLevel is three
// u32 fields = 12 bytes. This study measures whether widening the level record
// to a power-of-two size, or aligning the per-side block to a 64-byte cache
// line, changes scan throughput.
//
// Three layouts are compared over an identical depth-10 top-of-book scan:
//   packed12  : 12-byte record, the v1 layout
//   padded16  : 16-byte record (one u32 of explicit pad), naturally aligned
//   aligned64 : 16-byte record whose backing array starts on a 64-byte line
//
// The workload sums total_qty across every visible level for a large number
// of synthetic books, which is the shape of a quote-aggregation consumer.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "obs/clock.h"

namespace {

struct LevelPacked {
    std::uint32_t price;
    std::uint32_t total_qty;
    std::uint32_t order_count;
};

struct LevelPadded {
    std::uint32_t price;
    std::uint32_t total_qty;
    std::uint32_t order_count;
    std::uint32_t pad;
};

constexpr std::size_t kDepth = 10;

template <typename Level>
std::uint64_t scan(const std::vector<Level>& levels, std::uint64_t iters) {
    std::uint64_t acc = 0;
    for (std::uint64_t it = 0; it < iters; ++it) {
        for (std::size_t i = 0; i < levels.size(); ++i) {
            acc += levels[i].total_qty;
        }
    }
    return acc;
}

template <typename Level>
double measure(std::size_t books, std::uint64_t iters, bool align64) {
    // One contiguous block of books * kDepth levels.
    const std::size_t total = books * kDepth;
    Level* base = nullptr;
    void* raw = nullptr;
    if (align64) {
        raw = ::operator new[](total * sizeof(Level), std::align_val_t(64));
        base = static_cast<Level*>(raw);
    } else {
        raw = ::operator new[](total * sizeof(Level));
        base = static_cast<Level*>(raw);
    }
    for (std::size_t i = 0; i < total; ++i) {
        base[i].price = static_cast<std::uint32_t>(100000 + i);
        base[i].total_qty = static_cast<std::uint32_t>(100 + (i % 900));
        base[i].order_count = 1;
    }
    std::uint64_t acc = 0;
    const auto t0 = mdfeed::obs::now_ns();
    for (std::uint64_t it = 0; it < iters; ++it) {
        for (std::size_t b = 0; b < books; ++b) {
            const Level* lv = base + b * kDepth;
            for (std::size_t i = 0; i < kDepth; ++i) acc += lv[i].total_qty;
        }
    }
    const auto t1 = mdfeed::obs::now_ns();
    if (align64) {
        ::operator delete[](raw, std::align_val_t(64));
    } else {
        ::operator delete[](raw);
    }
    // Defeat dead-code elimination.
    std::fprintf(stderr, "# checksum=%llu\n", (unsigned long long)acc);
    const double scans = static_cast<double>(iters) * static_cast<double>(books) * kDepth;
    const double sec = static_cast<double>(t1 - t0) / 1e9;
    return sec > 0 ? scans / sec : 0.0;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t books = 50'000;
    std::uint64_t iters = 200;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--books") && i + 1 < argc) {
            books = static_cast<std::size_t>(std::atoll(argv[++i]));
        } else if (!std::strcmp(argv[i], "--iters") && i + 1 < argc) {
            iters = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        }
    }

    const double packed = measure<LevelPacked>(books, iters, false);
    const double padded = measure<LevelPadded>(books, iters, false);
    const double aligned = measure<LevelPadded>(books, iters, true);

    std::printf(
        "{\n"
        "  \"books\": %zu,\n"
        "  \"iters\": %llu,\n"
        "  \"sizeof_packed12\": %zu,\n"
        "  \"sizeof_padded16\": %zu,\n"
        "  \"levels_per_cacheline_packed\": %.2f,\n"
        "  \"levels_per_cacheline_padded\": %.2f,\n"
        "  \"scan_levels_per_sec\": {\n"
        "    \"packed12\": %.0f,\n"
        "    \"padded16\": %.0f,\n"
        "    \"aligned64\": %.0f\n"
        "  }\n"
        "}\n",
        books, (unsigned long long)iters, sizeof(LevelPacked), sizeof(LevelPadded),
        64.0 / sizeof(LevelPacked), 64.0 / sizeof(LevelPadded), packed, padded, aligned);
    (void)scan<LevelPacked>;
    return 0;
}
