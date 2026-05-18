#include "core/feed_handler.h"

#include <cstring>
#include <variant>

#include "sim/publisher.h"

namespace mdfeed::core {

namespace {

inline std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint64_t read_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    return v;
}

void tally_type(PerTypeCounters& c, const itch::AnyMessage& msg) {
    std::visit(
        [&](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, itch::AddOrderMsg> ||
                          std::is_same_v<T, itch::AddOrderMPIDMsg>) {
                c.add += 1;
            } else if constexpr (std::is_same_v<T, itch::OrderExecutedMsg> ||
                                 std::is_same_v<T, itch::OrderExecutedWithPriceMsg>) {
                c.execute += 1;
            } else if constexpr (std::is_same_v<T, itch::OrderCancelMsg>) {
                c.cancel += 1;
            } else if constexpr (std::is_same_v<T, itch::OrderDeleteMsg>) {
                c.delete_ += 1;
            } else if constexpr (std::is_same_v<T, itch::OrderReplaceMsg>) {
                c.replace += 1;
            } else {
                c.other += 1;
            }
        },
        msg);
}

}  // namespace

bool FeedHandler::on_datagram(const std::uint8_t* data, std::size_t len) {
    stats_.bytes_in += len;
    stats_.datagrams_in += 1;

    if (len < sim::kTransportHeaderSize + 2) {
        stats_.parse_errors += 1;
        return false;
    }
    const itch::StockLocate loc = read_u16(data);
    const itch::SequenceNumber seq = read_u64(data + 2);

    // Sequence 0 messages are bootstrap (stock directory). Apply without
    // gap-checking.
    if (seq != 0) {
        const auto obs = gap_.observe(loc, seq);
        if (obs.is_stale) {
            stats_.stale_msgs += 1;
            return false;
        }
        if (obs.is_gap) {
            stats_.gaps_detected += 1;
            if (requester_) {
                itch::SnapshotRequest req{};
                req.stock_locate = loc;
                req.from_seq = obs.gap.expected;
                itch::SnapshotResponse resp{};
                stats_.snapshots_requested += 1;
                if (requester_(req, resp)) {
                    apply_snapshot(resp);
                    // After applying the snapshot, the next expected sequence
                    // is whatever the snapshot reported + 1, but the message
                    // we just observed (seq) might be at or beyond that point.
                    if (seq >= gap_.expected(loc)) {
                        // Apply the current message and bump expected.
                        // Fall through to normal application below by
                        // continuing past this branch.
                    } else {
                        // Snapshot already covers this message; mark observed.
                        return true;
                    }
                } else {
                    return false;
                }
            } else {
                return false;
            }
        }
        // After a successful recovery, manually advance expected so the same
        // sequence is not flagged twice on the apply path below.
        if (gap_.expected(loc) <= seq) {
            gap_.set_expected(loc, seq + 1);
        }
    }

    // Decode the ITCH frame.
    const std::uint8_t* body = data + sim::kTransportHeaderSize;
    const std::size_t body_len = len - sim::kTransportHeaderSize;
    auto pr = itch::decode_frame(std::span<const std::uint8_t>(body, body_len));
    if (pr.status != itch::ParseStatus::Ok || !pr.message.has_value()) {
        stats_.parse_errors += 1;
        return false;
    }
    if (book_.apply(*pr.message)) {
        stats_.messages_applied += 1;
        tally_type(stats_.by_type, *pr.message);
    }
    return true;
}

void FeedHandler::apply_snapshot(const itch::SnapshotResponse& snap) {
    // Replace the book state for the given symbol with the snapshot. The
    // snapshot is authoritative for the price ladders but does not carry
    // per-order ids. The order_id index is therefore reset for this symbol.
    auto& sym_book = book_.book_for(snap.symbol);

    // The simplest authoritative way to install a snapshot without exposing
    // private state on SymbolBook is to wipe and rebuild using synthetic
    // orders, one per price level, with deterministic synthesised ids that
    // do not collide with the publisher's ids (publisher ids start at 1 and
    // increment; snapshot synthetic ids start at 1ULL << 60).
    // We delete all existing orders for the symbol first.
    // This requires walking the SymbolBook's order set; we expose has_order
    // / delete_order / find_order, so we iterate by scanning a copy of ids.
    // For test sanity the snapshot is the source of truth.

    // Build a list of order ids belonging to this symbol via the book's
    // public API. We do not have direct iteration on SymbolBook, so we use
    // a fresh approach: we replace the SymbolBook entirely via reassignment.
    sym_book =
        itch::SymbolBook{snap.bids.size() > snap.asks.size() ? snap.bids.size() : snap.asks.size()};
    // Synthesise one order per level so the ladder rebuilds with correct
    // counts and totals.
    itch::OrderId synthetic = 1ULL << 60;
    for (const auto& lvl : snap.bids) {
        // Multiple orders at this level to preserve order_count.
        const std::uint32_t cnt = std::max<std::uint32_t>(1u, lvl.order_count);
        const itch::Quantity per = lvl.total_qty / cnt;
        const itch::Quantity rem = lvl.total_qty - per * cnt;
        for (std::uint32_t i = 0; i < cnt; ++i) {
            const itch::Quantity q = per + (i == 0 ? rem : 0);
            if (q == 0) continue;
            sym_book.add_order(synthetic++, itch::Side::Buy, lvl.price, q, 0);
        }
    }
    for (const auto& lvl : snap.asks) {
        const std::uint32_t cnt = std::max<std::uint32_t>(1u, lvl.order_count);
        const itch::Quantity per = lvl.total_qty / cnt;
        const itch::Quantity rem = lvl.total_qty - per * cnt;
        for (std::uint32_t i = 0; i < cnt; ++i) {
            const itch::Quantity q = per + (i == 0 ? rem : 0);
            if (q == 0) continue;
            sym_book.add_order(synthetic++, itch::Side::Sell, lvl.price, q, 0);
        }
    }
    gap_.set_expected(snap.stock_locate, snap.last_applied_seq + 1);
    stats_.snapshots_applied += 1;
}

}  // namespace mdfeed::core
