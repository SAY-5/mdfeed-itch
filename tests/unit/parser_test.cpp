#include "core/parser.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "core/itch_messages.h"

using namespace mdfeed::itch;

namespace {

AnyMessage round_trip(const AnyMessage& in) {
    std::vector<std::uint8_t> buf(framed_size(in));
    const std::size_t n = encode_frame(in, buf);
    EXPECT_EQ(n, buf.size());
    auto pr = decode_frame(std::span<const std::uint8_t>(buf.data(), buf.size()));
    EXPECT_EQ(pr.status, ParseStatus::Ok);
    EXPECT_EQ(pr.consumed, n);
    EXPECT_TRUE(pr.message.has_value());
    return *pr.message;
}

}  // namespace

TEST(Parser, SystemEvent) {
    SystemEventMsg m{};
    m.stock_locate = 1;
    m.tracking_number = 2;
    m.ts = 1234567890ULL;
    m.event_code = 'O';
    const auto rt = std::get<SystemEventMsg>(round_trip(m));
    EXPECT_EQ(rt.stock_locate, m.stock_locate);
    EXPECT_EQ(rt.tracking_number, m.tracking_number);
    EXPECT_EQ(rt.ts, m.ts);
    EXPECT_EQ(rt.event_code, m.event_code);
}

TEST(Parser, StockDirectory) {
    StockDirectoryMsg m{};
    m.stock_locate = 7;
    m.symbol = make_symbol("AAPL");
    m.market_category = 'Q';
    m.financial_status = 'N';
    m.round_lot_size = 100;
    const auto rt = std::get<StockDirectoryMsg>(round_trip(m));
    EXPECT_EQ(rt.stock_locate, m.stock_locate);
    EXPECT_EQ(symbol_to_string(rt.symbol), "AAPL");
    EXPECT_EQ(rt.market_category, 'Q');
    EXPECT_EQ(rt.round_lot_size, 100u);
}

TEST(Parser, AddOrder) {
    AddOrderMsg m{};
    m.stock_locate = 11;
    m.tracking_number = 3;
    m.ts = 9999;
    m.order_id = 0xDEADBEEFCAFEBABEULL;
    m.side = Side::Buy;
    m.shares = 250;
    m.symbol = make_symbol("MSFT");
    m.price = 1500000;
    const auto rt = std::get<AddOrderMsg>(round_trip(m));
    EXPECT_EQ(rt.order_id, m.order_id);
    EXPECT_EQ(static_cast<char>(rt.side), 'B');
    EXPECT_EQ(rt.shares, m.shares);
    EXPECT_EQ(symbol_to_string(rt.symbol), "MSFT");
    EXPECT_EQ(rt.price, m.price);
}

TEST(Parser, AddOrderMPID) {
    AddOrderMPIDMsg m{};
    m.base.stock_locate = 12;
    m.base.order_id = 42;
    m.base.side = Side::Sell;
    m.base.shares = 500;
    m.base.symbol = make_symbol("GOOG");
    m.base.price = 2750000;
    m.attribution = {'B', 'A', 'R', 'X'};
    const auto rt = std::get<AddOrderMPIDMsg>(round_trip(m));
    EXPECT_EQ(rt.base.order_id, 42u);
    EXPECT_EQ(rt.base.shares, 500u);
    EXPECT_EQ(std::string(rt.attribution.data(), 4), "BARX");
}

TEST(Parser, OrderExecuted) {
    OrderExecutedMsg m{};
    m.stock_locate = 13;
    m.order_id = 99;
    m.executed_shares = 100;
    m.match_number = 0xAABBCCDD;
    const auto rt = std::get<OrderExecutedMsg>(round_trip(m));
    EXPECT_EQ(rt.order_id, 99u);
    EXPECT_EQ(rt.executed_shares, 100u);
    EXPECT_EQ(rt.match_number, 0xAABBCCDDu);
}

TEST(Parser, OrderExecutedWithPrice) {
    OrderExecutedWithPriceMsg m{};
    m.base.stock_locate = 14;
    m.base.order_id = 100;
    m.base.executed_shares = 50;
    m.base.match_number = 7;
    m.printable = 'Y';
    m.execution_price = 200000;
    const auto rt = std::get<OrderExecutedWithPriceMsg>(round_trip(m));
    EXPECT_EQ(rt.printable, 'Y');
    EXPECT_EQ(rt.execution_price, 200000u);
}

TEST(Parser, OrderCancel) {
    OrderCancelMsg m{};
    m.stock_locate = 15;
    m.order_id = 101;
    m.canceled_shares = 25;
    const auto rt = std::get<OrderCancelMsg>(round_trip(m));
    EXPECT_EQ(rt.order_id, 101u);
    EXPECT_EQ(rt.canceled_shares, 25u);
}

TEST(Parser, OrderDelete) {
    OrderDeleteMsg m{};
    m.stock_locate = 16;
    m.order_id = 102;
    const auto rt = std::get<OrderDeleteMsg>(round_trip(m));
    EXPECT_EQ(rt.order_id, 102u);
}

TEST(Parser, OrderReplace) {
    OrderReplaceMsg m{};
    m.stock_locate = 17;
    m.original_order_id = 200;
    m.new_order_id = 201;
    m.shares = 400;
    m.price = 1500000;
    const auto rt = std::get<OrderReplaceMsg>(round_trip(m));
    EXPECT_EQ(rt.original_order_id, 200u);
    EXPECT_EQ(rt.new_order_id, 201u);
    EXPECT_EQ(rt.shares, 400u);
}

TEST(Parser, Trade) {
    TradeMsg m{};
    m.stock_locate = 18;
    m.order_id = 0;
    m.side = Side::Buy;
    m.shares = 1000;
    m.symbol = make_symbol("NVDA");
    m.price = 900000;
    m.match_number = 0xABCDEF;
    const auto rt = std::get<TradeMsg>(round_trip(m));
    EXPECT_EQ(rt.shares, 1000u);
    EXPECT_EQ(symbol_to_string(rt.symbol), "NVDA");
    EXPECT_EQ(rt.match_number, 0xABCDEFu);
}

TEST(Parser, ShortBuffer) {
    std::vector<std::uint8_t> buf = {0x00};  // only 1 byte
    auto pr = decode_frame(std::span<const std::uint8_t>(buf.data(), buf.size()));
    EXPECT_EQ(pr.status, ParseStatus::ShortBuffer);
}

TEST(Parser, UnknownType) {
    std::vector<std::uint8_t> buf;
    buf.push_back(0x00);
    buf.push_back(0x05);  // length 5
    buf.push_back('Z');   // unknown type
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    auto pr = decode_frame(std::span<const std::uint8_t>(buf.data(), buf.size()));
    EXPECT_EQ(pr.status, ParseStatus::UnknownType);
    EXPECT_EQ(pr.consumed, 7u);
}

TEST(Parser, LengthMismatch) {
    // Encode an AddOrder then truncate the wire length to 35 (correct is 36).
    AddOrderMsg m{};
    m.stock_locate = 1;
    m.order_id = 1;
    m.side = Side::Buy;
    m.shares = 100;
    m.symbol = make_symbol("AAPL");
    m.price = 100;
    std::vector<std::uint8_t> buf(framed_size(m));
    encode_frame(m, buf);
    buf[0] = 0;
    buf[1] = 35;
    auto pr = decode_frame(std::span<const std::uint8_t>(buf.data(), buf.size() - 1));
    EXPECT_EQ(pr.status, ParseStatus::LengthMismatch);
}

TEST(Parser, RandomGarbageDoesNotCrash) {
    std::vector<std::uint8_t> buf(256);
    for (std::size_t seed = 0; seed < 1000; ++seed) {
        for (std::size_t i = 0; i < buf.size(); ++i) {
            buf[i] = static_cast<std::uint8_t>((seed * 31 + i * 7) & 0xff);
        }
        auto pr = decode_frame(std::span<const std::uint8_t>(buf.data(), buf.size()));
        (void)pr;
    }
}
