#include "core/depth_book.h"

#include <algorithm>
#include <utility>

#include "core/itch_messages.h"

namespace mdfeed::itch {

const OrderState* SymbolBook::find_order(OrderId id) const {
    auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
}

void SymbolBook::apply_level_delta(Side side, Price price, std::int64_t qty_delta,
                                   std::int64_t count_delta) {
    if (side == Side::Buy) {
        auto it = bid_levels_.find(price);
        if (it == bid_levels_.end()) {
            if (qty_delta <= 0) return;
            PriceLevel pl{};
            pl.price = price;
            pl.total_qty = static_cast<Quantity>(qty_delta);
            pl.order_count = static_cast<std::uint32_t>(count_delta);
            bid_levels_.emplace(price, pl);
        } else {
            auto& pl = it->second;
            const std::int64_t nq = static_cast<std::int64_t>(pl.total_qty) + qty_delta;
            const std::int64_t nc = static_cast<std::int64_t>(pl.order_count) + count_delta;
            if (nq <= 0 || nc <= 0) {
                bid_levels_.erase(it);
            } else {
                pl.total_qty = static_cast<Quantity>(nq);
                pl.order_count = static_cast<std::uint32_t>(nc);
            }
        }
        refresh_top_bids();
    } else {
        auto it = ask_levels_.find(price);
        if (it == ask_levels_.end()) {
            if (qty_delta <= 0) return;
            PriceLevel pl{};
            pl.price = price;
            pl.total_qty = static_cast<Quantity>(qty_delta);
            pl.order_count = static_cast<std::uint32_t>(count_delta);
            ask_levels_.emplace(price, pl);
        } else {
            auto& pl = it->second;
            const std::int64_t nq = static_cast<std::int64_t>(pl.total_qty) + qty_delta;
            const std::int64_t nc = static_cast<std::int64_t>(pl.order_count) + count_delta;
            if (nq <= 0 || nc <= 0) {
                ask_levels_.erase(it);
            } else {
                pl.total_qty = static_cast<Quantity>(nq);
                pl.order_count = static_cast<std::uint32_t>(nc);
            }
        }
        refresh_top_asks();
    }
}

void SymbolBook::refresh_top_bids() {
    top_bids_.levels.clear();
    top_bids_.levels.reserve(depth_);
    std::size_t i = 0;
    for (const auto& [_, pl] : bid_levels_) {
        if (i++ >= depth_) break;
        top_bids_.levels.push_back(pl);
    }
}

void SymbolBook::refresh_top_asks() {
    top_asks_.levels.clear();
    top_asks_.levels.reserve(depth_);
    std::size_t i = 0;
    for (const auto& [_, pl] : ask_levels_) {
        if (i++ >= depth_) break;
        top_asks_.levels.push_back(pl);
    }
}

void SymbolBook::add_order(OrderId id, Side side, Price price, Quantity qty, Timestamp ts) {
    if (qty == 0) return;
    auto [it, inserted] = orders_.emplace(id, OrderState{});
    if (!inserted) return;  // duplicate id, ignore
    it->second.side = side;
    it->second.price = price;
    it->second.qty = qty;
    it->second.ts = ts;
    last_ts_ = ts;
    apply_level_delta(side, price, static_cast<std::int64_t>(qty), 1);
}

bool SymbolBook::modify_order(OrderId id, Quantity new_qty, Timestamp ts) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    const std::int64_t delta =
        static_cast<std::int64_t>(new_qty) - static_cast<std::int64_t>(it->second.qty);
    if (delta == 0) {
        it->second.ts = ts;
        last_ts_ = ts;
        return true;
    }
    if (new_qty == 0) {
        const auto side = it->second.side;
        const auto price = it->second.price;
        const auto qty = it->second.qty;
        orders_.erase(it);
        apply_level_delta(side, price, -static_cast<std::int64_t>(qty), -1);
    } else {
        const auto side = it->second.side;
        const auto price = it->second.price;
        it->second.qty = new_qty;
        it->second.ts = ts;
        apply_level_delta(side, price, delta, 0);
    }
    last_ts_ = ts;
    return true;
}

bool SymbolBook::reduce_order(OrderId id, Quantity delta, Timestamp ts) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    const Quantity cur = it->second.qty;
    const Quantity new_qty = (delta >= cur) ? 0 : cur - delta;
    return modify_order(id, new_qty, ts);
}

bool SymbolBook::delete_order(OrderId id, Timestamp ts) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    const auto side = it->second.side;
    const auto price = it->second.price;
    const auto qty = it->second.qty;
    orders_.erase(it);
    apply_level_delta(side, price, -static_cast<std::int64_t>(qty), -1);
    last_ts_ = ts;
    return true;
}

bool SymbolBook::replace_order(OrderId old_id, OrderId new_id, Price new_price, Quantity new_qty,
                               Timestamp ts) {
    auto it = orders_.find(old_id);
    if (it == orders_.end()) return false;
    const Side side = it->second.side;
    const Symbol sym{};
    (void)sym;
    delete_order(old_id, ts);
    add_order(new_id, side, new_price, new_qty, ts);
    return true;
}

bool SymbolBook::invariants_ok() const {
    if (top_bids_.levels.size() > depth_) return false;
    if (top_asks_.levels.size() > depth_) return false;
    for (std::size_t i = 1; i < top_bids_.levels.size(); ++i) {
        if (!(top_bids_.levels[i - 1].price > top_bids_.levels[i].price)) return false;
    }
    for (std::size_t i = 1; i < top_asks_.levels.size(); ++i) {
        if (!(top_asks_.levels[i - 1].price < top_asks_.levels[i].price)) return false;
    }
    for (const auto& lvl : top_bids_.levels) {
        if (lvl.order_count == 0 || lvl.total_qty == 0) return false;
    }
    for (const auto& lvl : top_asks_.levels) {
        if (lvl.order_count == 0 || lvl.total_qty == 0) return false;
    }
    return true;
}

SymbolBook& DepthBook::book_for(const Symbol& sym) {
    const auto key = symbol_key(sym);
    auto it = books_.find(key);
    if (it == books_.end()) {
        it = books_.emplace(key, SymbolBook{depth_}).first;
        symbol_index_.emplace(key, sym);
    }
    return it->second;
}

const SymbolBook* DepthBook::find(const Symbol& sym) const {
    const auto key = symbol_key(sym);
    auto it = books_.find(key);
    return it == books_.end() ? nullptr : &it->second;
}

std::optional<Symbol> DepthBook::symbol_for_order(OrderId id) const {
    auto it = order_to_symbol_.find(id);
    if (it == order_to_symbol_.end()) return std::nullopt;
    auto sit = symbol_index_.find(it->second);
    if (sit == symbol_index_.end()) return std::nullopt;
    return sit->second;
}

std::size_t DepthBook::total_orders() const {
    std::size_t n = 0;
    for (const auto& [_, b] : books_) n += b.order_count();
    return n;
}

DepthSnapshot DepthBook::snapshot(const Symbol& sym) const {
    DepthSnapshot snap{};
    snap.symbol = sym;
    auto it = books_.find(symbol_key(sym));
    if (it == books_.end()) return snap;
    snap.bids = it->second.bids();
    snap.asks = it->second.asks();
    snap.ts = it->second.last_ts();
    return snap;
}

void DepthBook::emit_snapshot(const Symbol& sym, Timestamp ts) {
    if (!cb_) return;
    auto it = books_.find(symbol_key(sym));
    if (it == books_.end()) return;
    DepthSnapshot snap{};
    snap.symbol = sym;
    snap.bids = it->second.bids();
    snap.asks = it->second.asks();
    snap.ts = ts;
    cb_(snap);
}

bool DepthBook::apply(const AnyMessage& msg) {
    bool changed = false;
    std::optional<Symbol> sym;
    Timestamp ts = 0;

    std::visit(
        [&](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, AddOrderMsg>) {
                auto& b = book_for(m.symbol);
                b.add_order(m.order_id, m.side, m.price, m.shares, m.ts);
                order_to_symbol_[m.order_id] = symbol_key(m.symbol);
                sym = m.symbol;
                ts = m.ts;
                changed = true;
            } else if constexpr (std::is_same_v<T, AddOrderMPIDMsg>) {
                auto& b = book_for(m.base.symbol);
                b.add_order(m.base.order_id, m.base.side, m.base.price, m.base.shares, m.base.ts);
                order_to_symbol_[m.base.order_id] = symbol_key(m.base.symbol);
                sym = m.base.symbol;
                ts = m.base.ts;
                changed = true;
            } else if constexpr (std::is_same_v<T, OrderExecutedMsg>) {
                auto it = order_to_symbol_.find(m.order_id);
                if (it == order_to_symbol_.end()) return;
                auto sit = symbol_index_.find(it->second);
                if (sit == symbol_index_.end()) return;
                auto& b = books_.find(it->second)->second;
                if (b.reduce_order(m.order_id, m.executed_shares, m.ts)) {
                    if (!b.has_order(m.order_id)) order_to_symbol_.erase(m.order_id);
                    sym = sit->second;
                    ts = m.ts;
                    changed = true;
                }
            } else if constexpr (std::is_same_v<T, OrderExecutedWithPriceMsg>) {
                auto it = order_to_symbol_.find(m.base.order_id);
                if (it == order_to_symbol_.end()) return;
                auto sit = symbol_index_.find(it->second);
                if (sit == symbol_index_.end()) return;
                auto& b = books_.find(it->second)->second;
                if (b.reduce_order(m.base.order_id, m.base.executed_shares, m.base.ts)) {
                    if (!b.has_order(m.base.order_id)) order_to_symbol_.erase(m.base.order_id);
                    sym = sit->second;
                    ts = m.base.ts;
                    changed = true;
                }
            } else if constexpr (std::is_same_v<T, OrderCancelMsg>) {
                auto it = order_to_symbol_.find(m.order_id);
                if (it == order_to_symbol_.end()) return;
                auto sit = symbol_index_.find(it->second);
                if (sit == symbol_index_.end()) return;
                auto& b = books_.find(it->second)->second;
                if (b.reduce_order(m.order_id, m.canceled_shares, m.ts)) {
                    if (!b.has_order(m.order_id)) order_to_symbol_.erase(m.order_id);
                    sym = sit->second;
                    ts = m.ts;
                    changed = true;
                }
            } else if constexpr (std::is_same_v<T, OrderDeleteMsg>) {
                auto it = order_to_symbol_.find(m.order_id);
                if (it == order_to_symbol_.end()) return;
                auto sit = symbol_index_.find(it->second);
                if (sit == symbol_index_.end()) return;
                auto& b = books_.find(it->second)->second;
                if (b.delete_order(m.order_id, m.ts)) {
                    order_to_symbol_.erase(m.order_id);
                    sym = sit->second;
                    ts = m.ts;
                    changed = true;
                }
            } else if constexpr (std::is_same_v<T, OrderReplaceMsg>) {
                auto it = order_to_symbol_.find(m.original_order_id);
                if (it == order_to_symbol_.end()) return;
                auto sit = symbol_index_.find(it->second);
                if (sit == symbol_index_.end()) return;
                const auto key = it->second;
                auto& b = books_.find(key)->second;
                const auto* prev = b.find_order(m.original_order_id);
                if (!prev) return;
                const Side side = prev->side;
                b.delete_order(m.original_order_id, m.ts);
                order_to_symbol_.erase(m.original_order_id);
                b.add_order(m.new_order_id, side, m.price, m.shares, m.ts);
                order_to_symbol_[m.new_order_id] = key;
                sym = sit->second;
                ts = m.ts;
                changed = true;
            } else if constexpr (std::is_same_v<T, TradeMsg>) {
                // Non-cross trade messages do not alter the visible book.
                (void)m;
            } else {
                // SystemEvent, StockDirectory: no book impact.
                (void)m;
            }
        },
        msg);

    if (changed && sym) emit_snapshot(*sym, ts);
    return changed;
}

}  // namespace mdfeed::itch
