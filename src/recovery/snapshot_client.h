#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/depth_book.h"
#include "core/types.h"

namespace mdfeed::itch {

// Snapshot+gap-fill protocol (frames, both directions are length-prefixed by
// 4-byte big-endian length + 1-byte op + body):
//
//   client -> server: 'Q' SnapshotRequest { u16 stock_locate, u64 from_seq }
//   server -> client: 'S' SnapshotResponse {
//       u16 stock_locate,
//       u64 last_applied_seq,
//       Symbol symbol,
//       u32 bid_levels_count,
//       repeated PriceLevel,
//       u32 ask_levels_count,
//       repeated PriceLevel
//   }
//   server -> client: 'F' SnapshotFault { u16 stock_locate, u8 code }
//
// PriceLevel on the wire: u32 price, u32 total_qty, u32 order_count

struct SnapshotRequest {
    StockLocate stock_locate;
    SequenceNumber from_seq;
};

struct SnapshotFault {
    StockLocate stock_locate;
    std::uint8_t code;
};

struct SnapshotResponse {
    StockLocate stock_locate;
    SequenceNumber last_applied_seq;
    Symbol symbol;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};

// Wire helpers; pure functions (no sockets) so they can be unit-tested.
std::vector<std::uint8_t> encode_request(const SnapshotRequest& req);
std::vector<std::uint8_t> encode_response(const SnapshotResponse& resp);
std::vector<std::uint8_t> encode_fault(const SnapshotFault& f);

struct DecodedFrame {
    enum class Kind { None, Request, Response, Fault };
    Kind kind{Kind::None};
    SnapshotRequest request{};
    SnapshotResponse response{};
    SnapshotFault fault{};
};

// Decode one frame from the front of bytes; consumed is set to bytes used.
DecodedFrame decode_frame(const std::vector<std::uint8_t>& bytes, std::size_t& consumed);

}  // namespace mdfeed::itch
