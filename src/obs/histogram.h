#pragma once

#include <cstdint>
#include <vector>

namespace mdfeed::obs {

// Log-linear histogram in nanoseconds. Bucket layout:
//   - 64 sub-buckets per power of 2
//   - Range 1ns up to ~1s. Values above the top bucket are clamped.
//
// Optimized for the hot ingest path: record() is branch-light and O(1).
class Histogram {
public:
    Histogram();

    void record(std::uint64_t value_ns);

    std::uint64_t count() const { return count_; }
    std::uint64_t total() const { return total_; }
    std::uint64_t min() const { return min_; }
    std::uint64_t max() const { return max_; }
    double mean() const { return count_ ? static_cast<double>(total_) / count_ : 0.0; }

    // Approximate percentile in ns. p is in [0, 1].
    std::uint64_t percentile(double p) const;

    void merge(const Histogram& other);
    void reset();

private:
    static constexpr int kSubBucketBits = 6;  // 64
    static constexpr int kSubBuckets = 1 << kSubBucketBits;
    static constexpr int kBuckets = 32;
    std::vector<std::uint64_t> counts_;  // size = kBuckets * kSubBuckets
    std::uint64_t count_{0};
    std::uint64_t total_{0};
    std::uint64_t min_{UINT64_MAX};
    std::uint64_t max_{0};

    static int bucket_index(std::uint64_t value, int& sub);
    static std::uint64_t bucket_lower_bound(int bucket, int sub);
};

}  // namespace mdfeed::obs
