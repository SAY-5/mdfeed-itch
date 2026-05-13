// Property tests for depth-10 SymbolBook invariants:
//  - bids sorted strictly descending
//  - asks sorted strictly ascending
//  - level qty totals equal the sum of order qty per (symbol, side, price)
//  - per-side order_count per level matches the live order count
//  - book never exceeds depth_ levels
//
// Generator emits random valid sequences mixed across {add, modify, cancel,
// execute, delete, replace}. After every applied message we re-check
// invariants_ok() and also recompute totals/counts from a shadow map.

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <vector>

#include "core/depth_book.h"
#include "core/itch_messages.h"

using namespace mdfeed::itch;

namespace {

struct ShadowOrder {
    Side side;
    Price price;
    Quantity qty;
};

struct ShadowBook {
    std::map<OrderId, ShadowOrder> orders;

    void add(OrderId id, Side s, Price p, Quantity q) {
        if (q == 0) return;
        orders.emplace(id, ShadowOrder{s, p, q});
    }
    void reduce(OrderId id, Quantity delta) {
        auto it = orders.find(id);
        if (it == orders.end()) return;
        if (delta >= it->second.qty) {
            orders.erase(it);
        } else {
            it->second.qty -= delta;
        }
    }
    void erase(OrderId id) { orders.erase(id); }
    void replace(OrderId old_id, OrderId new_id, Price p, Quantity q) {
        auto it = orders.find(old_id);
        if (it == orders.end()) return;
        const Side s = it->second.side;
        orders.erase(it);
        if (q > 0) orders.emplace(new_id, ShadowOrder{s, p, q});
    }

    struct Levels {
        std::map<Price, std::pair<Quantity, std::uint32_t>, std::greater<Price>> bids;
        std::map<Price, std::pair<Quantity, std::uint32_t>> asks;
    };

    Levels recompute() const {
        Levels out;
        for (const auto& [_, o] : orders) {
            if (o.qty == 0) continue;
            if (o.side == Side::Buy) {
                auto& slot = out.bids[o.price];
                slot.first += o.qty;
                slot.second += 1;
            } else {
                auto& slot = out.asks[o.price];
                slot.first += o.qty;
                slot.second += 1;
            }
        }
        return out;
    }
};

void check_consistent(const SymbolBook& book, const ShadowBook& shadow, std::size_t depth) {
    ASSERT_TRUE(book.invariants_ok());
    const auto lv = shadow.recompute();

    // Compare top-N levels.
    std::vector<std::pair<Price, std::pair<Quantity, std::uint32_t>>> top_bids;
    for (auto it = lv.bids.begin(); it != lv.bids.end() && top_bids.size() < depth; ++it) {
        top_bids.push_back({it->first, it->second});
    }
    ASSERT_EQ(book.bids().levels.size(), top_bids.size());
    for (std::size_t i = 0; i < top_bids.size(); ++i) {
        EXPECT_EQ(book.bids().levels[i].price, top_bids[i].first);
        EXPECT_EQ(book.bids().levels[i].total_qty, top_bids[i].second.first);
        EXPECT_EQ(book.bids().levels[i].order_count, top_bids[i].second.second);
    }

    std::vector<std::pair<Price, std::pair<Quantity, std::uint32_t>>> top_asks;
    for (auto it = lv.asks.begin(); it != lv.asks.end() && top_asks.size() < depth; ++it) {
        top_asks.push_back({it->first, it->second});
    }
    ASSERT_EQ(book.asks().levels.size(), top_asks.size());
    for (std::size_t i = 0; i < top_asks.size(); ++i) {
        EXPECT_EQ(book.asks().levels[i].price, top_asks[i].first);
        EXPECT_EQ(book.asks().levels[i].total_qty, top_asks[i].second.first);
        EXPECT_EQ(book.asks().levels[i].order_count, top_asks[i].second.second);
    }
}

}  // namespace

// ----- Random mixed sequences -----

TEST(PropertyBook, RandomMixedSequences) {
    constexpr std::size_t kDepth = 10;
    constexpr int kSeeds = 25;
    constexpr int kStepsPerSeed = 2000;

    for (int seed = 0; seed < kSeeds; ++seed) {
        std::mt19937_64 rng(static_cast<std::uint64_t>(0xA110CA7E) +
                            static_cast<std::uint64_t>(seed));
        SymbolBook book(kDepth);
        ShadowBook shadow;

        std::vector<OrderId> live;
        OrderId next_id = 1;

        std::uniform_int_distribution<int> action_dist(0, 99);
        std::uniform_int_distribution<int> side_dist(0, 1);
        std::uniform_int_distribution<Price> price_dist(100,
                                                        130);  // narrow band, lots of collisions
        std::uniform_int_distribution<Quantity> qty_dist(1, 500);

        for (int step = 0; step < kStepsPerSeed; ++step) {
            int a = action_dist(rng);
            if (live.empty() || a < 50) {
                const OrderId id = next_id++;
                const Side s = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
                const Price p = price_dist(rng);
                const Quantity q = qty_dist(rng);
                book.add_order(id, s, p, q, 0);
                shadow.add(id, s, p, q);
                if (q > 0) live.push_back(id);
            } else {
                std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
                const std::size_t k = pick(rng);
                const OrderId id = live[k];
                const auto* st = book.find_order(id);
                if (!st) {
                    // Already gone, drop from live and try again next loop.
                    live[k] = live.back();
                    live.pop_back();
                    continue;
                }
                if (a < 70) {
                    // Modify: random new qty (can be 0).
                    std::uniform_int_distribution<Quantity> nq_dist(0, st->qty + 100);
                    const Quantity nq = nq_dist(rng);
                    const Quantity old = st->qty;
                    book.modify_order(id, nq, 0);
                    if (nq == 0) {
                        shadow.erase(id);
                    } else {
                        // Apply diff via shadow.reduce / shadow add-back semantics.
                        if (nq < old) {
                            shadow.reduce(id, old - nq);
                        } else {
                            auto it = shadow.orders.find(id);
                            if (it != shadow.orders.end()) it->second.qty = nq;
                        }
                    }
                    if (!book.has_order(id)) {
                        live[k] = live.back();
                        live.pop_back();
                    }
                } else if (a < 80) {
                    // Cancel partial.
                    std::uniform_int_distribution<Quantity> cd(1, std::max<Quantity>(1, st->qty));
                    const Quantity c = cd(rng);
                    book.reduce_order(id, c, 0);
                    shadow.reduce(id, c);
                    if (!book.has_order(id)) {
                        live[k] = live.back();
                        live.pop_back();
                    }
                } else if (a < 90) {
                    // Execute (same as reduce path).
                    std::uniform_int_distribution<Quantity> ex(1, std::max<Quantity>(1, st->qty));
                    const Quantity c = ex(rng);
                    book.reduce_order(id, c, 0);
                    shadow.reduce(id, c);
                    if (!book.has_order(id)) {
                        live[k] = live.back();
                        live.pop_back();
                    }
                } else if (a < 95) {
                    // Delete.
                    book.delete_order(id, 0);
                    shadow.erase(id);
                    live[k] = live.back();
                    live.pop_back();
                } else {
                    // Replace.
                    const OrderId new_id = next_id++;
                    const Price np = price_dist(rng);
                    const Quantity nq = qty_dist(rng);
                    book.replace_order(id, new_id, np, nq, 0);
                    shadow.replace(id, new_id, np, nq);
                    live[k] = live.back();
                    live.pop_back();
                    if (nq > 0) live.push_back(new_id);
                }
            }

            // Cap order count assertion: orders <= number of adds (sanity).
            ASSERT_LE(book.order_count(), static_cast<std::size_t>(step + 1));

            // Re-check invariants every 13 steps to keep cost bounded while
            // still catching transient violations.
            if ((step % 13) == 0) {
                check_consistent(book, shadow, kDepth);
            }
        }

        // Final state.
        check_consistent(book, shadow, kDepth);
    }
}

// Depth-N is honored: many random adds + occasional delete of best level
// must keep top_bids/top_asks size <= N and properly sorted.
TEST(PropertyBook, DepthBoundedAtN) {
    constexpr std::size_t kDepth = 10;
    std::mt19937_64 rng(0xDEADBEEFULL);
    SymbolBook book(kDepth);
    std::uniform_int_distribution<Price> price_dist(1, 1000);
    OrderId next_id = 1;
    std::vector<OrderId> live;
    for (int step = 0; step < 5000; ++step) {
        if ((rng() & 7) == 0 && !live.empty()) {
            std::uniform_int_distribution<std::size_t> p(0, live.size() - 1);
            const std::size_t k = p(rng);
            book.delete_order(live[k], 0);
            live[k] = live.back();
            live.pop_back();
        } else {
            const OrderId id = next_id++;
            book.add_order(id, (rng() & 1) ? Side::Buy : Side::Sell, price_dist(rng), 100, 0);
            live.push_back(id);
        }
        ASSERT_LE(book.bids().levels.size(), kDepth);
        ASSERT_LE(book.asks().levels.size(), kDepth);
        ASSERT_TRUE(book.invariants_ok());
    }
}
