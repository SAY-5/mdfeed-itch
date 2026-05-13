#include "obs/histogram.h"

#include <algorithm>
#include <climits>

namespace mdfeed::obs {

namespace {
int leading_zeros_64(std::uint64_t v) {
    if (v == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(v);
#else
    int n = 0;
    while ((v & (1ULL << 63)) == 0) {
        v <<= 1;
        ++n;
    }
    return n;
#endif
}
}  // namespace

Histogram::Histogram() : counts_(static_cast<std::size_t>(kBuckets) * kSubBuckets, 0) {}

int Histogram::bucket_index(std::uint64_t value, int& sub) {
    if (value <= 1) {
        sub = 0;
        return 0;
    }
    const int top_bit = 63 - leading_zeros_64(value);
    int bucket = top_bit - kSubBucketBits;
    if (bucket < 0) {
        sub = static_cast<int>(value) - 1;
        return 0;
    }
    if (bucket >= kBuckets) {
        sub = kSubBuckets - 1;
        return kBuckets - 1;
    }
    // sub-bucket from bits below the top.
    sub = static_cast<int>((value >> bucket) & (kSubBuckets - 1));
    return bucket;
}

std::uint64_t Histogram::bucket_lower_bound(int bucket, int sub) {
    if (bucket == 0) return static_cast<std::uint64_t>(sub) + 1;
    return (static_cast<std::uint64_t>(kSubBuckets) + static_cast<std::uint64_t>(sub))
           << bucket;
}

void Histogram::record(std::uint64_t value_ns) {
    int sub = 0;
    const int bucket = bucket_index(value_ns, sub);
    counts_[static_cast<std::size_t>(bucket) * kSubBuckets + static_cast<std::size_t>(sub)] += 1;
    ++count_;
    total_ += value_ns;
    if (value_ns < min_) min_ = value_ns;
    if (value_ns > max_) max_ = value_ns;
}

void Histogram::merge(const Histogram& other) {
    for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += other.counts_[i];
    count_ += other.count_;
    total_ += other.total_;
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
}

void Histogram::reset() {
    std::fill(counts_.begin(), counts_.end(), 0u);
    count_ = 0;
    total_ = 0;
    min_ = UINT64_MAX;
    max_ = 0;
}

std::uint64_t Histogram::percentile(double p) const {
    if (count_ == 0) return 0;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    const std::uint64_t target = static_cast<std::uint64_t>(p * static_cast<double>(count_));
    std::uint64_t running = 0;
    for (int b = 0; b < kBuckets; ++b) {
        for (int s = 0; s < kSubBuckets; ++s) {
            running += counts_[static_cast<std::size_t>(b) * kSubBuckets +
                               static_cast<std::size_t>(s)];
            if (running >= target) return bucket_lower_bound(b, s);
        }
    }
    return max_;
}

}  // namespace mdfeed::obs
