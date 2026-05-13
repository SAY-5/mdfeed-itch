#include <gtest/gtest.h>

#include "recovery/gap_detector.h"

using namespace mdfeed::itch;

TEST(GapDetector, FirstMessageBootstraps) {
    GapDetector g;
    const auto r = g.observe(100, 1);
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.is_gap);
    EXPECT_EQ(g.expected(100), 2u);
}

TEST(GapDetector, InOrderAdvances) {
    GapDetector g;
    g.start(100, 1);
    for (SequenceNumber s = 1; s <= 10; ++s) {
        const auto r = g.observe(100, s);
        EXPECT_TRUE(r.ok);
    }
    EXPECT_EQ(g.expected(100), 11u);
}

TEST(GapDetector, GapDetected) {
    GapDetector g;
    g.start(100, 1);
    g.observe(100, 1);
    g.observe(100, 2);
    const auto r = g.observe(100, 5);
    EXPECT_TRUE(r.is_gap);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.gap.expected, 3u);
    EXPECT_EQ(r.gap.received, 5u);
    EXPECT_EQ(g.expected(100), 3u);  // not advanced until recovery
}

TEST(GapDetector, StaleDropped) {
    GapDetector g;
    g.start(100, 1);
    g.observe(100, 1);
    g.observe(100, 2);
    const auto r = g.observe(100, 1);
    EXPECT_TRUE(r.is_stale);
    EXPECT_FALSE(r.is_gap);
    EXPECT_FALSE(r.ok);
}

TEST(GapDetector, MultipleStreamsIndependent) {
    GapDetector g;
    g.observe(100, 1);
    g.observe(200, 1);
    g.observe(100, 2);
    EXPECT_EQ(g.expected(100), 3u);
    EXPECT_EQ(g.expected(200), 2u);
}

TEST(GapDetector, SetExpectedAfterRecovery) {
    GapDetector g;
    g.start(100, 1);
    g.observe(100, 1);
    const auto r = g.observe(100, 5);
    EXPECT_TRUE(r.is_gap);
    g.set_expected(100, 6);
    const auto r2 = g.observe(100, 6);
    EXPECT_TRUE(r2.ok);
}
