#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/itch_messages.h"
#include "core/types.h"

namespace mdfeed::sim {

// Deterministic ITCH message generator. Each generated message is wrapped in
// a small transport header carrying a per-stock-locate sequence number, so
// the receiver can detect gaps.
//
// Transport-layer frame on the wire (multicast payload), big-endian:
//   [u16 stock_locate][u64 sequence][u16 itch_length][1 byte type][N bytes body]
//
// The itch_length+type+body region is exactly what the parser decodes.

struct TransportHeader {
    itch::StockLocate stock_locate;
    itch::SequenceNumber sequence;
};

constexpr std::size_t kTransportHeaderSize = 2 + 8;

class MessageGenerator {
  public:
    explicit MessageGenerator(std::uint64_t seed = 0xC0FFEEULL, std::size_t num_symbols = 8);

    // Build one transport-framed datagram payload and place it in out.
    // Returns the bytes written, or 0 on encode failure.
    std::size_t next(std::vector<std::uint8_t>& out, TransportHeader& hdr_out);

    // Generate a stock directory message for every configured symbol. Used to
    // bootstrap the receiver.
    std::vector<std::vector<std::uint8_t>> directory_messages();

    const std::vector<itch::Symbol>& symbols() const { return symbols_; }

  private:
    std::mt19937_64 rng_;
    std::vector<itch::Symbol> symbols_;
    std::vector<itch::OrderId> live_orders_;
    std::vector<itch::Price> live_prices_;
    std::vector<itch::Quantity> live_qtys_;
    std::vector<itch::Side> live_sides_;
    std::vector<itch::StockLocate> live_locs_;
    itch::OrderId next_order_id_{1};
    std::unordered_map<itch::StockLocate, itch::SequenceNumber> next_seq_;
    itch::StockLocate base_locate_{100};
};

}  // namespace mdfeed::sim
