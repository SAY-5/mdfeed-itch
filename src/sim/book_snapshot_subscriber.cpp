#include "sim/book_snapshot_subscriber.h"

namespace mdfeed::sim {

bool BookSnapshotSubscriber::connect(std::uint16_t port, int timeout_ms, std::string* err) {
    return client_.connect("127.0.0.1", port, timeout_ms, err);
}

bool BookSnapshotSubscriber::receive_one() {
    std::vector<std::uint8_t> frame;
    if (!client_.recv_frame(frame)) return false;
    bool ok = false;
    const BookSnapshot snap = decode_book_snapshot(frame, ok);
    if (!ok) return false;

    // A snapshot frame is authoritative for the symbols it carries: replace
    // the local depth-10 state wholesale so the rebuild matches the
    // publisher's reference exactly.
    for (const auto& rec : snap.symbols) {
        const itch::SymbolKey key = itch::symbol_key(rec.symbol);
        LocalBook& lb = books_[key];
        lb.bids = rec.bids;
        lb.asks = rec.asks;
        index_[key] = rec.symbol;
    }
    last_seq_ = snap.sequence;
    ++frames_applied_;
    return true;
}

const std::vector<itch::PriceLevel>* BookSnapshotSubscriber::bids(const itch::Symbol& sym) const {
    auto it = books_.find(itch::symbol_key(sym));
    return it == books_.end() ? nullptr : &it->second.bids;
}

const std::vector<itch::PriceLevel>* BookSnapshotSubscriber::asks(const itch::Symbol& sym) const {
    auto it = books_.find(itch::symbol_key(sym));
    return it == books_.end() ? nullptr : &it->second.asks;
}

std::vector<itch::Symbol> BookSnapshotSubscriber::symbols() const {
    std::vector<itch::Symbol> out;
    out.reserve(index_.size());
    for (const auto& [_, sym] : index_) out.push_back(sym);
    return out;
}

}  // namespace mdfeed::sim
