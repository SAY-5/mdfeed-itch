// Property tests: random valid-shape ITCH messages of all 10 types
// round-trip through encode_frame() -> decode_frame() byte-equal.
//
// Each message type runs N random trials. The random generator is seeded
// deterministically so failures reproduce.

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "core/itch_messages.h"
#include "core/parser.h"

using namespace mdfeed::itch;

namespace {

constexpr int kTrialsPerType = 2000;

struct R {
    std::mt19937_64 rng;
    explicit R(std::uint64_t seed) : rng(seed) {}
    std::uint16_t u16() {
        return static_cast<std::uint16_t>(
            std::uniform_int_distribution<std::uint32_t>(0, 0xFFFF)(rng));
    }
    std::uint32_t u32() {
        return std::uniform_int_distribution<std::uint32_t>(0, 0xFFFFFFFFu)(rng);
    }
    std::uint64_t u48() {
        return std::uniform_int_distribution<std::uint64_t>(0, (1ULL << 48) - 1)(rng);
    }
    std::uint64_t u64() { return std::uniform_int_distribution<std::uint64_t>(0, ~0ULL)(rng); }
    char ascii_letter() {
        return static_cast<char>('A' + std::uniform_int_distribution<int>(0, 25)(rng));
    }
    Side side() { return std::uniform_int_distribution<int>(0, 1)(rng) ? Side::Buy : Side::Sell; }
    Symbol symbol() {
        Symbol s{};
        const int len = std::uniform_int_distribution<int>(1, 8)(rng);
        for (int i = 0; i < kSymbolLen; ++i) {
            s[static_cast<std::size_t>(i)] = i < len ? ascii_letter() : ' ';
        }
        return s;
    }
};

// Helper: encode then decode, return the decoded variant on success.
template <typename Msg>
Msg round_trip_one(const Msg& in) {
    AnyMessage am = in;
    std::vector<std::uint8_t> buf(framed_size(am));
    const std::size_t n = encode_frame(am, buf);
    EXPECT_EQ(n, buf.size());
    auto pr = decode_frame(std::span<const std::uint8_t>(buf.data(), buf.size()));
    EXPECT_EQ(pr.status, ParseStatus::Ok);
    EXPECT_EQ(pr.consumed, n);
    EXPECT_TRUE(pr.message.has_value());
    // Also: encoding the decoded form must produce the same bytes.
    std::vector<std::uint8_t> buf2(framed_size(*pr.message));
    const std::size_t n2 = encode_frame(*pr.message, buf2);
    EXPECT_EQ(n2, buf2.size());
    EXPECT_EQ(buf, buf2);
    return std::get<Msg>(*pr.message);
}

}  // namespace

TEST(PropertyParser, SystemEvent_RoundTrip) {
    R r(0x5'5555ULL);
    for (int i = 0; i < kTrialsPerType; ++i) {
        SystemEventMsg m{};
        m.stock_locate = r.u16();
        m.tracking_number = r.u16();
        m.ts = r.u48();
        m.event_code = r.ascii_letter();
        const auto out = round_trip_one(m);
        EXPECT_EQ(out.stock_locate, m.stock_locate);
        EXPECT_EQ(out.tracking_number, m.tracking_number);
        EXPECT_EQ(out.ts, m.ts);
        EXPECT_EQ(out.event_code, m.event_code);
    }
}

TEST(PropertyParser, StockDirectory_RoundTrip) {
    R r(0x5'5556ULL);
    for (int i = 0; i < kTrialsPerType; ++i) {
        StockDirectoryMsg m{};
        m.stock_locate = r.u16();
        m.tracking_number = r.u16();
        m.ts = r.u48();
        m.symbol = r.symbol();
        m.market_category = r.ascii_letter();
        m.financial_status = r.ascii_letter();
        m.round_lot_size = r.u32();
        const auto out = round_trip_one(m);
        EXPECT_EQ(out.stock_locate, m.stock_locate);
        EXPECT_EQ(out.tracking_number, m.tracking_number);
        EXPECT_EQ(out.ts, m.ts);
        EXPECT_EQ(out.symbol, m.symbol);
        EXPECT_EQ(out.market_category, m.market_category);
        EXPECT_EQ(out.financial_status, m.financial_status);
        EXPECT_EQ(out.round_lot_size, m.round_lot_size);
    }
}

TEST(PropertyParser, AddOrder_RoundTrip) {
    R r(0x5'5557ULL);
    for (int i = 0; i < kTrialsPerType; ++i) {
        AddOrderMsg m{};
        m.stock_locate = r.u16();
        m.tracking_number = r.u16();
        m.ts = r.u48();
        m.order_id = r.u64();
        m.side = r.side();
        m.shares = r.u32();
        m.symbol = r.symbol();
        m.price = r.u32();
        const auto out = round_trip_one(m);
        EXPECT_EQ(out.order_id, m.order_id);
        EXPECT_EQ(out.side, m.side);
        EXPECT_EQ(out.shares, m.shares);
        EXPECT_EQ(out.symbol, m.symbol);
        EXPECT_EQ(out.price, m.price);
    }
}

TEST(PropertyParser, AddOrderMPID_RoundTrip) {
    R r(0x5'5558ULL);
    for (int i = 0; i < kTrialsPerType; ++i) {
        AddOrderMPIDMsg m{};
        m.base.stock_locate = r.u16();
        m.base.tracking_number = r.u16();
        m.base.ts = r.u48();
        m.base.order_id = r.u64();
        m.base.side = r.side();
        m.base.shares = r.u32();
        m.base.symbol = r.symbol();
        m.base.price = r.u32();
        for (int k = 0; k < 4; ++k) m.attribution[static_cast<std::size_t>(k)] = r.ascii_letter();
        const auto out = round_trip_one(m);
        EXPECT_EQ(out.base.order_id, m.base.order_id);
        EXPECT_EQ(out.base.symbol, m.base.symbol);
        EXPECT_EQ(out.attribution, m.attribution);
    }
}

TEST(PropertyParser, OrderExecuted_RoundTrip) {
    R r(0x5'5559ULL);
    for (int i = 0; i < kTrialsPerType; ++i) {
        OrderExecutedMsg m{};
        m.stock_locate = r.u16();
        m.tracking_number = r.u16();
        m.ts = r.u48();
        m.order_id = r.u64();
        m.executed_shares = r.u32();
        m.match_number = r.u64();
        const auto out = round_trip_one(m);
        EXPECT_EQ(out.order_id, m.order_id);
        EXPECT_EQ(out.executed_shares, m.executed_shares);
        EXPECT_EQ(out.match_number, m.match_number);
    }
}

TEST(PropertyParser, OrderExecutedWithPrice_RoundTrip) {
    R r(0x5'555AULL);
    for (int i = 0; i < kTrialsPerType; ++i) {
        OrderExecutedWithPriceMsg m{};
        m.base.stock_locate = r.u16();
        m.base.tracking_number = r.u16();
        m.base.ts = r.u48();
        m.base.order_id = r.u64();
        m.base.executed_shares = r.u32();
        m.base.match_number = r.u64();
        m.printable = (r.u16() & 1) ? 'Y' : 'N';
        m.execution_price = r.u32();
        const auto out = round_trip_one(m);
        EXPECT_EQ(out.base.order_id, m.base.order_id);
        EXPECT_EQ(out.printable, m.printable);
        EXPECT_EQ(out.execution_price, m.execution_price);
    }
}

TEST(PropertyParser, OrderCancel_RoundTrip) {
    R r(0x5'555BULL);
    for (int i = 0; i < kTrialsPerType; ++i) {
        OrderCancelMsg m{};
        m.stock_locate = r.u16();
        m.tracking_number = r.u16();
        m.ts = r.u48();
        m.order_id = r.u64();
        m.canceled_shares = r.u32();
        const auto out = round_trip_one(m);
        EXPECT_EQ(out.order_id, m.order_id);
        EXPECT_EQ(out.canceled_shares, m.canceled_shares);
    }
}

TEST(PropertyParser, OrderDelete_RoundTrip) {
    R r(0x5'555CULL);
    for (int i = 0; i < kTrialsPerType; ++i) {
        OrderDeleteMsg m{};
        m.stock_locate = r.u16();
        m.tracking_number = r.u16();
        m.ts = r.u48();
        m.order_id = r.u64();
        const auto out = round_trip_one(m);
        EXPECT_EQ(out.order_id, m.order_id);
    }
}

TEST(PropertyParser, OrderReplace_RoundTrip) {
    R r(0x5'555DULL);
    for (int i = 0; i < kTrialsPerType; ++i) {
        OrderReplaceMsg m{};
        m.stock_locate = r.u16();
        m.tracking_number = r.u16();
        m.ts = r.u48();
        m.original_order_id = r.u64();
        m.new_order_id = r.u64();
        m.shares = r.u32();
        m.price = r.u32();
        const auto out = round_trip_one(m);
        EXPECT_EQ(out.original_order_id, m.original_order_id);
        EXPECT_EQ(out.new_order_id, m.new_order_id);
        EXPECT_EQ(out.shares, m.shares);
        EXPECT_EQ(out.price, m.price);
    }
}

TEST(PropertyParser, Trade_RoundTrip) {
    R r(0x5'555EULL);
    for (int i = 0; i < kTrialsPerType; ++i) {
        TradeMsg m{};
        m.stock_locate = r.u16();
        m.tracking_number = r.u16();
        m.ts = r.u48();
        m.order_id = r.u64();
        m.side = r.side();
        m.shares = r.u32();
        m.symbol = r.symbol();
        m.price = r.u32();
        m.match_number = r.u64();
        const auto out = round_trip_one(m);
        EXPECT_EQ(out.order_id, m.order_id);
        EXPECT_EQ(out.side, m.side);
        EXPECT_EQ(out.shares, m.shares);
        EXPECT_EQ(out.symbol, m.symbol);
        EXPECT_EQ(out.price, m.price);
        EXPECT_EQ(out.match_number, m.match_number);
    }
}
