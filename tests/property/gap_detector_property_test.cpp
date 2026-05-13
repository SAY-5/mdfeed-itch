// Property test: for random sequences with random drop rates, every gap is
// detected by GapDetector::observe(). We construct an in-order sequence,
// then drop a random subset and feed the surviving messages to the detector.
// We then verify that the set of gaps reported (after each recovery the
// detector is advanced via set_expected) covers exactly the dropped seqs.

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "core/types.h"
#include "recovery/gap_detector.h"

using namespace mdfeed::itch;

TEST(PropertyGap, RandomDropsAllDetected) {
    constexpr int kSeeds = 40;
    constexpr int kSeqLen = 500;
    for (int seed = 0; seed < kSeeds; ++seed) {
        std::mt19937_64 rng(static_cast<std::uint64_t>(seed) + 0xBADC0FFEEULL);
        std::uniform_real_distribution<double> drop_rate(0.0, 0.5);
        const double rate = drop_rate(rng);

        std::vector<SequenceNumber> surviving;
        std::set<SequenceNumber> dropped;
        for (SequenceNumber s = 1; s <= kSeqLen; ++s) {
            if (std::generate_canonical<double, 32>(rng) < rate) {
                dropped.insert(s);
            } else {
                surviving.push_back(s);
            }
        }

        GapDetector g;
        const StockLocate loc = 100;
        g.start(loc, 1);

        std::set<SequenceNumber> reported_missing;
        for (SequenceNumber seq : surviving) {
            const auto r = g.observe(loc, seq);
            if (r.is_gap) {
                // Every sequence in [expected, received) is missing.
                for (SequenceNumber m = r.gap.expected; m < r.gap.received; ++m) {
                    reported_missing.insert(m);
                }
                // Simulate recovery: advance to received+1 (we just observed received).
                g.set_expected(loc, r.gap.received + 1);
            } else if (r.is_stale) {
                FAIL() << "in-order survivor cannot be stale: seq=" << seq;
            } else {
                EXPECT_TRUE(r.ok);
            }
        }

        // Every dropped sequence (except a trailing tail past the last observed
        // sequence) should have been reported.
        SequenceNumber last_seen = surviving.empty() ? 0 : surviving.back();
        std::set<SequenceNumber> expected_missing;
        for (auto d : dropped) {
            if (d <= last_seen) expected_missing.insert(d);
        }
        EXPECT_EQ(reported_missing, expected_missing)
            << "seed=" << seed << " drop_rate=" << rate << " survivors=" << surviving.size();
    }
}

TEST(PropertyGap, MultiStreamIndependent) {
    constexpr int kSeeds = 20;
    for (int seed = 0; seed < kSeeds; ++seed) {
        std::mt19937_64 rng(static_cast<std::uint64_t>(seed) * 7919ULL + 11ULL);
        const int n_streams = 4;
        std::vector<SequenceNumber> next_seq(static_cast<std::size_t>(n_streams), 1);
        GapDetector g;
        for (int i = 0; i < n_streams; ++i) g.start(static_cast<StockLocate>(100 + i), 1);

        // Round-robin observe in-order across streams. Every step the
        // stream advances by exactly 1 so no gap should ever be detected.
        for (int step = 0; step < 400; ++step) {
            const std::size_t s =
                static_cast<std::size_t>(std::uniform_int_distribution<int>(0, n_streams - 1)(rng));
            const auto r =
                g.observe(static_cast<StockLocate>(100 + static_cast<int>(s)), next_seq[s]);
            EXPECT_TRUE(r.ok) << "stream=" << s << " seq=" << next_seq[s];
            EXPECT_FALSE(r.is_gap);
            next_seq[s]++;
        }
        for (int i = 0; i < n_streams; ++i) {
            EXPECT_EQ(g.expected(static_cast<StockLocate>(100 + i)),
                      next_seq[static_cast<std::size_t>(i)]);
        }
    }
}
