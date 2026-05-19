#include "sim/book_snapshot.h"

#include <gtest/gtest.h>

#include "core/types.h"

using namespace mdfeed;

namespace {

itch::PriceLevel lvl(itch::Price p, itch::Quantity q, std::uint32_t c) {
    return itch::PriceLevel{p, q, c};
}

sim::BookSnapshot sample_snapshot() {
    sim::BookSnapshot snap;
    snap.sequence = 7;
    sim::BookSnapshotSymbol a;
    a.symbol = itch::make_symbol("AAPL");
    a.bids = {lvl(1500000, 300, 3), lvl(1499900, 100, 1)};
    a.asks = {lvl(1500100, 200, 2)};
    sim::BookSnapshotSymbol b;
    b.symbol = itch::make_symbol("MSFT");
    b.bids = {lvl(4200000, 50, 1)};
    b.asks = {};
    snap.symbols = {a, b};
    return snap;
}

}  // namespace

TEST(BookSnapshotWire, RoundTrip) {
    const auto snap = sample_snapshot();
    const auto frame = sim::encode_book_snapshot(snap);

    bool ok = false;
    const auto out = sim::decode_book_snapshot(frame, ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(out.sequence, snap.sequence);
    ASSERT_EQ(out.symbols.size(), 2u);

    EXPECT_EQ(out.symbols[0].symbol, snap.symbols[0].symbol);
    ASSERT_EQ(out.symbols[0].bids.size(), 2u);
    EXPECT_EQ(out.symbols[0].bids[0].price, 1500000u);
    EXPECT_EQ(out.symbols[0].bids[0].total_qty, 300u);
    EXPECT_EQ(out.symbols[0].bids[0].order_count, 3u);
    ASSERT_EQ(out.symbols[0].asks.size(), 1u);
    EXPECT_EQ(out.symbols[0].asks[0].price, 1500100u);

    EXPECT_EQ(out.symbols[1].symbol, snap.symbols[1].symbol);
    ASSERT_EQ(out.symbols[1].bids.size(), 1u);
    EXPECT_TRUE(out.symbols[1].asks.empty());
}

TEST(BookSnapshotWire, EmptyBook) {
    sim::BookSnapshot snap;
    snap.sequence = 1;
    const auto frame = sim::encode_book_snapshot(snap);
    bool ok = false;
    const auto out = sim::decode_book_snapshot(frame, ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(out.sequence, 1u);
    EXPECT_TRUE(out.symbols.empty());
}

TEST(BookSnapshotWire, CrcMismatchRejected) {
    auto frame = sim::encode_book_snapshot(sample_snapshot());
    ASSERT_GT(frame.size(), 10u);
    // Flip a byte inside the body; CRC must catch it.
    frame[10] ^= 0xFF;
    bool ok = true;
    sim::decode_book_snapshot(frame, ok);
    EXPECT_FALSE(ok);
}

TEST(BookSnapshotWire, TruncatedFrameRejected) {
    auto frame = sim::encode_book_snapshot(sample_snapshot());
    frame.resize(frame.size() - 3);
    bool ok = true;
    sim::decode_book_snapshot(frame, ok);
    EXPECT_FALSE(ok);
}

TEST(BookSnapshotWire, BadMagicRejected) {
    auto frame = sim::encode_book_snapshot(sample_snapshot());
    // Byte 4 is the first magic byte ('B').
    frame[4] = 'X';
    bool ok = true;
    sim::decode_book_snapshot(frame, ok);
    EXPECT_FALSE(ok);
}

TEST(BookSnapshotWire, Crc32KnownVector) {
    // CRC-32/IEEE of "123456789" is 0xCBF43926.
    const std::uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(sim::crc32(data, sizeof(data)), 0xCBF43926u);
}
