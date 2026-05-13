#include "sim/publisher.h"

#include <algorithm>
#include <cstring>

#include "core/parser.h"

namespace mdfeed::sim {

namespace {

void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

void put_u64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>(x >> (56 - i * 8)));
}

}  // namespace

MessageGenerator::MessageGenerator(std::uint64_t seed, std::size_t num_symbols) : rng_(seed) {
    static const char* names[] = {"AAPL", "MSFT", "GOOG", "AMZN", "NVDA",
                                  "META", "TSLA", "AMD",  "INTC", "ORCL"};
    const std::size_t n = std::min(num_symbols, sizeof(names) / sizeof(names[0]));
    symbols_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) symbols_.push_back(itch::make_symbol(names[i]));
}

std::vector<std::vector<std::uint8_t>> MessageGenerator::directory_messages() {
    std::vector<std::vector<std::uint8_t>> out;
    out.reserve(symbols_.size());
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
        itch::StockDirectoryMsg m{};
        m.stock_locate = static_cast<itch::StockLocate>(base_locate_ + i);
        m.tracking_number = 0;
        m.ts = 0;
        m.symbol = symbols_[i];
        m.market_category = 'Q';
        m.financial_status = 'N';
        m.round_lot_size = 100;
        std::vector<std::uint8_t> payload;
        payload.resize(kTransportHeaderSize + 2 + itch::kWireSize_R);
        // transport header
        payload[0] = static_cast<std::uint8_t>(m.stock_locate >> 8);
        payload[1] = static_cast<std::uint8_t>(m.stock_locate);
        const itch::SequenceNumber s = 0;
        for (std::size_t j = 0; j < 8; ++j)
            payload[2 + j] = static_cast<std::uint8_t>(s >> (56 - j * 8));
        std::span<std::uint8_t> body(payload.data() + kTransportHeaderSize,
                                     payload.size() - kTransportHeaderSize);
        const std::size_t n = itch::encode_frame(m, body);
        if (n == 0) continue;
        payload.resize(kTransportHeaderSize + n);
        out.push_back(std::move(payload));
    }
    return out;
}

std::size_t MessageGenerator::next(std::vector<std::uint8_t>& out, TransportHeader& hdr_out) {
    // Choose a symbol and an action.
    std::uniform_int_distribution<std::size_t> pick_sym(0, symbols_.empty() ? 0 : symbols_.size() - 1);
    std::uniform_int_distribution<int> action_dist(0, 99);
    const std::size_t si = pick_sym(rng_);
    const itch::Symbol& sym = symbols_[si];
    const itch::StockLocate loc = static_cast<itch::StockLocate>(base_locate_ + si);

    int a = action_dist(rng_);
    // Distribution biased toward Add to keep the book populated.
    //   0..49 Add, 50..69 Cancel partial, 70..84 Execute, 85..94 Delete, 95..99 Replace
    itch::AnyMessage msg;
    if (live_orders_.empty() || a < 50) {
        itch::AddOrderMsg m{};
        m.stock_locate = loc;
        m.tracking_number = 0;
        m.ts = next_order_id_;  // monotone for determinism
        m.order_id = next_order_id_++;
        std::uniform_int_distribution<int> side_dist(0, 1);
        m.side = side_dist(rng_) == 0 ? itch::Side::Buy : itch::Side::Sell;
        std::uniform_int_distribution<itch::Quantity> qty_dist(100, 1000);
        m.shares = qty_dist(rng_);
        std::uniform_int_distribution<itch::Price> price_dist(100000, 200000);
        m.price = price_dist(rng_);
        m.symbol = sym;
        live_orders_.push_back(m.order_id);
        live_prices_.push_back(m.price);
        live_qtys_.push_back(m.shares);
        live_sides_.push_back(m.side);
        live_locs_.push_back(m.stock_locate);
        msg = m;
    } else {
        std::uniform_int_distribution<std::size_t> pick_ord(0, live_orders_.size() - 1);
        const std::size_t oi = pick_ord(rng_);
        const itch::OrderId oid = live_orders_[oi];
        if (a < 70) {
            itch::OrderCancelMsg m{};
            m.stock_locate = live_locs_[oi];
            m.ts = next_order_id_++;
            m.order_id = oid;
            std::uniform_int_distribution<itch::Quantity> cd(1, std::max<itch::Quantity>(1, live_qtys_[oi]));
            m.canceled_shares = cd(rng_);
            if (m.canceled_shares >= live_qtys_[oi]) {
                live_qtys_[oi] = 0;
                live_orders_.erase(live_orders_.begin() + static_cast<std::ptrdiff_t>(oi));
                live_prices_.erase(live_prices_.begin() + static_cast<std::ptrdiff_t>(oi));
                live_qtys_.erase(live_qtys_.begin() + static_cast<std::ptrdiff_t>(oi));
                live_sides_.erase(live_sides_.begin() + static_cast<std::ptrdiff_t>(oi));
                live_locs_.erase(live_locs_.begin() + static_cast<std::ptrdiff_t>(oi));
            } else {
                live_qtys_[oi] -= m.canceled_shares;
            }
            msg = m;
        } else if (a < 85) {
            itch::OrderExecutedMsg m{};
            m.stock_locate = live_locs_[oi];
            m.ts = next_order_id_++;
            m.order_id = oid;
            std::uniform_int_distribution<itch::Quantity> ex(1, std::max<itch::Quantity>(1, live_qtys_[oi]));
            m.executed_shares = ex(rng_);
            m.match_number = next_order_id_;
            if (m.executed_shares >= live_qtys_[oi]) {
                live_orders_.erase(live_orders_.begin() + static_cast<std::ptrdiff_t>(oi));
                live_prices_.erase(live_prices_.begin() + static_cast<std::ptrdiff_t>(oi));
                live_qtys_.erase(live_qtys_.begin() + static_cast<std::ptrdiff_t>(oi));
                live_sides_.erase(live_sides_.begin() + static_cast<std::ptrdiff_t>(oi));
                live_locs_.erase(live_locs_.begin() + static_cast<std::ptrdiff_t>(oi));
            } else {
                live_qtys_[oi] -= m.executed_shares;
            }
            msg = m;
        } else if (a < 95) {
            itch::OrderDeleteMsg m{};
            m.stock_locate = live_locs_[oi];
            m.ts = next_order_id_++;
            m.order_id = oid;
            live_orders_.erase(live_orders_.begin() + static_cast<std::ptrdiff_t>(oi));
            live_prices_.erase(live_prices_.begin() + static_cast<std::ptrdiff_t>(oi));
            live_qtys_.erase(live_qtys_.begin() + static_cast<std::ptrdiff_t>(oi));
            live_sides_.erase(live_sides_.begin() + static_cast<std::ptrdiff_t>(oi));
            live_locs_.erase(live_locs_.begin() + static_cast<std::ptrdiff_t>(oi));
            msg = m;
        } else {
            itch::OrderReplaceMsg m{};
            m.stock_locate = live_locs_[oi];
            m.ts = next_order_id_++;
            m.original_order_id = oid;
            m.new_order_id = next_order_id_++;
            std::uniform_int_distribution<itch::Quantity> q(100, 1000);
            m.shares = q(rng_);
            std::uniform_int_distribution<itch::Price> p(100000, 200000);
            m.price = p(rng_);
            live_orders_[oi] = m.new_order_id;
            live_prices_[oi] = m.price;
            live_qtys_[oi] = m.shares;
            msg = m;
        }
    }

    hdr_out.stock_locate = loc;
    auto sit = next_seq_.find(loc);
    if (sit == next_seq_.end()) {
        next_seq_[loc] = 2;
        hdr_out.sequence = 1;
    } else {
        hdr_out.sequence = sit->second++;
    }

    out.clear();
    put_u16(out, hdr_out.stock_locate);
    put_u64(out, hdr_out.sequence);
    const std::size_t prefix = out.size();
    out.resize(prefix + itch::framed_size(msg));
    std::span<std::uint8_t> body(out.data() + prefix, out.size() - prefix);
    const std::size_t n = itch::encode_frame(msg, body);
    if (n == 0) {
        out.clear();
        return 0;
    }
    out.resize(prefix + n);
    return out.size();
}

}  // namespace mdfeed::sim
