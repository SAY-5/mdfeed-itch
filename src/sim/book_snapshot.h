#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/depth_book.h"
#include "core/types.h"

namespace mdfeed::sim {

// Book-snapshot wire format (v4). A snapshot frame carries the full depth-10
// state of every symbol the publisher tracks at one consistent point in time.
// All integers are big-endian. The byte layout is documented in
// docs/snapshot-wire.md; this header is the authoritative source.
//
// Frame:
//   u32  body_length          (bytes after this field, including the CRC)
//   u8   magic[4] = "BSN1"
//   u64  sequence             (monotonic publish counter, starts at 1)
//   u32  symbol_count
//   repeated SymbolRecord
//   u32  crc32                (CRC-32 of every byte from magic .. last record)
//
// SymbolRecord:
//   u8   symbol[8]
//   u8   bid_level_count      (0..10)
//   repeated WireLevel        (bid_level_count entries, best bid first)
//   u8   ask_level_count      (0..10)
//   repeated WireLevel        (ask_level_count entries, best ask first)
//
// WireLevel:
//   u32  price
//   u32  total_qty
//   u32  order_count

constexpr std::uint8_t kSnapshotMagic[4] = {'B', 'S', 'N', '1'};
constexpr std::size_t kSnapshotMaxLevels = 10;

// One symbol's depth-10 state as carried on the wire.
struct BookSnapshotSymbol {
    itch::Symbol symbol{};
    std::vector<itch::PriceLevel> bids;
    std::vector<itch::PriceLevel> asks;
};

// A complete cross-symbol snapshot at one publish point.
struct BookSnapshot {
    std::uint64_t sequence{0};
    std::vector<BookSnapshotSymbol> symbols;
};

// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) over a byte range.
std::uint32_t crc32(const std::uint8_t* data, std::size_t len);

// Take a consistent copy of every symbol in the book. Pure read of the book;
// the caller is responsible for any locking. The returned BookSnapshot owns
// its data so it can be serialised without further book access.
BookSnapshot capture_book_snapshot(const itch::DepthBook& book, std::uint64_t sequence);

// Serialise a snapshot to a length-prefixed frame ready for TCP write.
std::vector<std::uint8_t> encode_book_snapshot(const BookSnapshot& snap);

// Decode one frame from the front of bytes. ok is set false on a truncated
// frame, bad magic, or a CRC mismatch.
BookSnapshot decode_book_snapshot(const std::vector<std::uint8_t>& bytes, bool& ok);

}  // namespace mdfeed::sim
