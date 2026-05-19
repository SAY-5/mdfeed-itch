#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/itch_messages.h"
#include "core/types.h"

namespace mdfeed::itch {

constexpr std::size_t kDefaultDepth = 10;

struct PriceLevel {
    Price price{0};
    Quantity total_qty{0};
    std::uint32_t order_count{0};
};
// Hot scan record. The packed 12-byte layout is intentional: see
// docs/cache-padding-study.md for the measured comparison against padded and
// cache-line-aligned variants.
static_assert(sizeof(PriceLevel) == 12, "PriceLevel layout changed; revisit cache-padding-study");

struct DepthSide {
    // Sorted: bids descending by price, asks ascending by price. Capped at
    // capacity().
    std::vector<PriceLevel> levels;
};

struct DepthSnapshot {
    Symbol symbol{};
    DepthSide bids;
    DepthSide asks;
    Timestamp ts{0};
};

struct OrderState {
    Symbol symbol{};
    Side side{Side::Buy};
    Price price{0};
    Quantity qty{0};
    Timestamp ts{0};
};
// Power-of-two so two records fit one cache line exactly; see
// docs/cache-padding-study.md.
static_assert(sizeof(OrderState) == 32, "OrderState layout changed; revisit cache-padding-study");

using SymbolKey = std::uint64_t;

inline SymbolKey symbol_key(const Symbol& s) {
    SymbolKey k = 0;
    for (std::size_t i = 0; i < kSymbolLen; ++i) {
        k = (k << 8) | static_cast<std::uint8_t>(s[i]);
    }
    return k;
}

// Per-symbol depth-N book. The internal representation keeps a full price
// ladder map so updates beyond the top-N still adjust correctly when an inner
// level is removed and a deeper level pops to the top.
class SymbolBook {
  public:
    explicit SymbolBook(std::size_t depth = kDefaultDepth) : depth_(depth) {}

    void add_order(OrderId id, Side side, Price price, Quantity qty, Timestamp ts);
    bool modify_order(OrderId id, Quantity new_qty, Timestamp ts);
    bool reduce_order(OrderId id, Quantity delta, Timestamp ts);
    bool delete_order(OrderId id, Timestamp ts);
    bool replace_order(OrderId old_id, OrderId new_id, Price new_price, Quantity new_qty,
                       Timestamp ts);

    const DepthSide& bids() const { return top_bids_; }
    const DepthSide& asks() const { return top_asks_; }
    Timestamp last_ts() const { return last_ts_; }

    std::size_t order_count() const { return orders_.size(); }
    std::size_t depth() const { return depth_; }

    bool has_order(OrderId id) const { return orders_.find(id) != orders_.end(); }
    const OrderState* find_order(OrderId id) const;

    bool invariants_ok() const;

  private:
    void apply_level_delta(Side side, Price price, std::int64_t qty_delta,
                           std::int64_t count_delta);
    void refresh_top_bids();
    void refresh_top_asks();

    std::size_t depth_;
    std::unordered_map<OrderId, OrderState> orders_;
    // Full price ladder per side. std::map keeps the ladder sorted so the
    // top-N can be sliced in O(N) without rebuilding everything on each
    // update. bids use std::greater so iteration order is descending.
    std::map<Price, PriceLevel, std::greater<Price>> bid_levels_;
    std::map<Price, PriceLevel> ask_levels_;
    DepthSide top_bids_;
    DepthSide top_asks_;
    Timestamp last_ts_{0};
};

// Owns a SymbolBook per symbol and exposes a callback for depth snapshots.
class DepthBook {
  public:
    using SnapshotCallback = std::function<void(const DepthSnapshot&)>;

    explicit DepthBook(std::size_t depth = kDefaultDepth) : depth_(depth) {}

    void set_snapshot_callback(SnapshotCallback cb) { cb_ = std::move(cb); }

    SymbolBook& book_for(const Symbol& sym);
    const SymbolBook* find(const Symbol& sym) const;

    // Look up the cached symbol for an order id. Returns nullopt if unknown.
    std::optional<Symbol> symbol_for_order(OrderId id) const;

    // Apply an ITCH message to the book and emit snapshots. Returns true if
    // the message produced any state change.
    bool apply(const AnyMessage& msg);

    // Number of distinct symbols ever seen.
    std::size_t symbol_count() const { return books_.size(); }

    // Every symbol the book currently tracks, in unspecified order. Used by
    // the snapshot publisher to take a consistent multi-symbol copy.
    std::vector<Symbol> all_symbols() const;

    // Total order count across all symbols.
    std::size_t total_orders() const;

    DepthSnapshot snapshot(const Symbol& sym) const;

  private:
    void emit_snapshot(const Symbol& sym, Timestamp ts);

    std::size_t depth_;
    std::unordered_map<SymbolKey, SymbolBook> books_;
    std::unordered_map<SymbolKey, Symbol> symbol_index_;
    // Order id -> symbol-key to route Executed/Cancel/Delete/Replace by id.
    std::unordered_map<OrderId, SymbolKey> order_to_symbol_;
    SnapshotCallback cb_;
};

}  // namespace mdfeed::itch
