#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/depth_book.h"
#include "net/tcp_recovery.h"
#include "sim/book_snapshot.h"

namespace mdfeed::sim {

// Subscriber side of the book-snapshot protocol. Connects to a
// BookSnapshotPublisher, reads snapshot frames, and rebuilds a local
// depth-10 book per symbol from the wire levels. The reconstruction is a
// faithful copy: each frame fully replaces the local state for the symbols
// it carries.
class BookSnapshotSubscriber {
  public:
    BookSnapshotSubscriber() = default;

    // Connect to 127.0.0.1:<port>.
    bool connect(std::uint16_t port, int timeout_ms, std::string* err = nullptr);

    // Read exactly one snapshot frame and apply it. Returns false on a
    // transport error or a malformed/CRC-failed frame.
    bool receive_one();

    // Sequence number of the most recently applied snapshot (0 if none).
    std::uint64_t last_sequence() const { return last_seq_; }
    std::uint64_t frames_applied() const { return frames_applied_; }

    // The reconstructed depth-10 levels for a symbol, or nullptr if unseen.
    const std::vector<itch::PriceLevel>* bids(const itch::Symbol& sym) const;
    const std::vector<itch::PriceLevel>* asks(const itch::Symbol& sym) const;

    std::vector<itch::Symbol> symbols() const;

    // Unblock a receive_one() in flight on another thread. Call this, join
    // the reader thread, then call close().
    void shutdown() { client_.shutdown(); }
    void close() { client_.close(); }

  private:
    struct LocalBook {
        std::vector<itch::PriceLevel> bids;
        std::vector<itch::PriceLevel> asks;
    };

    net::TcpClient client_;
    std::unordered_map<itch::SymbolKey, LocalBook> books_;
    std::unordered_map<itch::SymbolKey, itch::Symbol> index_;
    std::uint64_t last_seq_{0};
    std::uint64_t frames_applied_{0};
};

}  // namespace mdfeed::sim
