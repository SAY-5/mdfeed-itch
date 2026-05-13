#include "core/depth_book.h"

#include <gtest/gtest.h>

using namespace mdfeed::itch;

namespace {

AddOrderMsg add(OrderId id, Side side, Price p, Quantity q, const char* sym = "AAPL",
                Timestamp ts = 0) {
    AddOrderMsg m{};
    m.stock_locate = 1;
    m.order_id = id;
    m.side = side;
    m.price = p;
    m.shares = q;
    m.symbol = make_symbol(sym);
    m.ts = ts;
    return m;
}

}  // namespace

TEST(DepthBook, BidsSortedDescending) {
    DepthBook db(10);
    db.apply(add(1, Side::Buy, 100, 200));
    db.apply(add(2, Side::Buy, 105, 300));
    db.apply(add(3, Side::Buy, 110, 100));
    db.apply(add(4, Side::Buy, 102, 400));
    const auto* sb = db.find(make_symbol("AAPL"));
    ASSERT_NE(sb, nullptr);
    ASSERT_EQ(sb->bids().levels.size(), 4u);
    EXPECT_EQ(sb->bids().levels[0].price, 110u);
    EXPECT_EQ(sb->bids().levels[1].price, 105u);
    EXPECT_EQ(sb->bids().levels[2].price, 102u);
    EXPECT_EQ(sb->bids().levels[3].price, 100u);
}

TEST(DepthBook, AsksSortedAscending) {
    DepthBook db(10);
    db.apply(add(1, Side::Sell, 110, 200));
    db.apply(add(2, Side::Sell, 105, 300));
    db.apply(add(3, Side::Sell, 100, 100));
    const auto* sb = db.find(make_symbol("AAPL"));
    ASSERT_NE(sb, nullptr);
    ASSERT_EQ(sb->asks().levels.size(), 3u);
    EXPECT_EQ(sb->asks().levels[0].price, 100u);
    EXPECT_EQ(sb->asks().levels[2].price, 110u);
}

TEST(DepthBook, LevelTotalsAndCounts) {
    DepthBook db(10);
    db.apply(add(1, Side::Buy, 100, 200));
    db.apply(add(2, Side::Buy, 100, 300));
    db.apply(add(3, Side::Buy, 100, 100));
    const auto* sb = db.find(make_symbol("AAPL"));
    ASSERT_NE(sb, nullptr);
    ASSERT_EQ(sb->bids().levels.size(), 1u);
    EXPECT_EQ(sb->bids().levels[0].total_qty, 600u);
    EXPECT_EQ(sb->bids().levels[0].order_count, 3u);
}

TEST(DepthBook, DepthCappedAtN) {
    DepthBook db(3);
    for (int i = 0; i < 10; ++i) {
        db.apply(add(static_cast<OrderId>(i + 1), Side::Buy, static_cast<Price>(100 + i), 100));
    }
    const auto* sb = db.find(make_symbol("AAPL"));
    ASSERT_NE(sb, nullptr);
    ASSERT_EQ(sb->bids().levels.size(), 3u);
    EXPECT_EQ(sb->bids().levels[0].price, 109u);
    EXPECT_EQ(sb->bids().levels[1].price, 108u);
    EXPECT_EQ(sb->bids().levels[2].price, 107u);
    EXPECT_TRUE(sb->invariants_ok());
}

TEST(DepthBook, ExecutePartialThenFull) {
    DepthBook db(10);
    db.apply(add(1, Side::Buy, 100, 500));
    OrderExecutedMsg ex{};
    ex.stock_locate = 1;
    ex.order_id = 1;
    ex.executed_shares = 200;
    db.apply(ex);
    const auto* sb = db.find(make_symbol("AAPL"));
    ASSERT_EQ(sb->bids().levels.size(), 1u);
    EXPECT_EQ(sb->bids().levels[0].total_qty, 300u);

    ex.executed_shares = 300;
    db.apply(ex);
    EXPECT_EQ(sb->bids().levels.size(), 0u);
}

TEST(DepthBook, CancelDecrementsQty) {
    DepthBook db(10);
    db.apply(add(1, Side::Sell, 100, 1000));
    OrderCancelMsg c{};
    c.stock_locate = 1;
    c.order_id = 1;
    c.canceled_shares = 400;
    db.apply(c);
    const auto* sb = db.find(make_symbol("AAPL"));
    EXPECT_EQ(sb->asks().levels[0].total_qty, 600u);
}

TEST(DepthBook, DeleteRemovesOrder) {
    DepthBook db(10);
    db.apply(add(1, Side::Buy, 100, 100));
    db.apply(add(2, Side::Buy, 100, 200));
    OrderDeleteMsg d{};
    d.stock_locate = 1;
    d.order_id = 1;
    db.apply(d);
    const auto* sb = db.find(make_symbol("AAPL"));
    ASSERT_EQ(sb->bids().levels.size(), 1u);
    EXPECT_EQ(sb->bids().levels[0].order_count, 1u);
    EXPECT_EQ(sb->bids().levels[0].total_qty, 200u);
}

TEST(DepthBook, ReplaceMovesOrder) {
    DepthBook db(10);
    db.apply(add(1, Side::Buy, 100, 500));
    OrderReplaceMsg r{};
    r.stock_locate = 1;
    r.original_order_id = 1;
    r.new_order_id = 2;
    r.price = 105;
    r.shares = 600;
    db.apply(r);
    const auto* sb = db.find(make_symbol("AAPL"));
    ASSERT_EQ(sb->bids().levels.size(), 1u);
    EXPECT_EQ(sb->bids().levels[0].price, 105u);
    EXPECT_EQ(sb->bids().levels[0].total_qty, 600u);
}

TEST(DepthBook, DeepLevelPopsUpAfterDelete) {
    DepthBook db(3);
    db.apply(add(1, Side::Buy, 100, 100));
    db.apply(add(2, Side::Buy, 101, 100));
    db.apply(add(3, Side::Buy, 102, 100));
    db.apply(add(4, Side::Buy, 99, 100));  // outside top-3
    OrderDeleteMsg d{};
    d.stock_locate = 1;
    d.order_id = 3;  // delete the best bid
    db.apply(d);
    const auto* sb = db.find(make_symbol("AAPL"));
    ASSERT_EQ(sb->bids().levels.size(), 3u);
    EXPECT_EQ(sb->bids().levels[0].price, 101u);
    EXPECT_EQ(sb->bids().levels[1].price, 100u);
    EXPECT_EQ(sb->bids().levels[2].price, 99u);
}

TEST(DepthBook, SnapshotCallbackFiresOnChange) {
    DepthBook db(5);
    int n = 0;
    DepthSnapshot last{};
    db.set_snapshot_callback([&](const DepthSnapshot& s) {
        ++n;
        last = s;
    });
    db.apply(add(1, Side::Buy, 100, 100));
    EXPECT_EQ(n, 1);
    EXPECT_EQ(last.bids.levels.size(), 1u);
}

TEST(DepthBook, MultipleSymbolsIsolated) {
    DepthBook db(10);
    db.apply(add(1, Side::Buy, 100, 100, "AAPL"));
    db.apply(add(2, Side::Buy, 200, 200, "MSFT"));
    EXPECT_EQ(db.find(make_symbol("AAPL"))->bids().levels[0].price, 100u);
    EXPECT_EQ(db.find(make_symbol("MSFT"))->bids().levels[0].price, 200u);
    EXPECT_EQ(db.symbol_count(), 2u);
}
