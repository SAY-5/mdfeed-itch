#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/itch_messages.h"

namespace mdfeed::itch {

enum class ParseStatus {
    Ok,
    ShortBuffer,
    UnknownType,
    LengthMismatch,
};

struct ParseResult {
    ParseStatus status{ParseStatus::ShortBuffer};
    std::size_t consumed{0};
    std::optional<AnyMessage> message;
};

// Decode a single framed message from a buffer. The frame layout is:
//   [u16 big-endian length N][1 byte type code][N-1 bytes body]
//
// Returns the number of bytes consumed in result.consumed, regardless of
// whether the decode succeeded; for ShortBuffer the consumed count is 0.
ParseResult decode_frame(std::span<const std::uint8_t> bytes);

// Encode a message for the wire. Writes the 2-byte length prefix + type + body.
// Returns total bytes written.
std::size_t encode_frame(const AnyMessage& msg, std::span<std::uint8_t> out);

// Best-case wire size for a given variant alternative including the length
// prefix. Useful for pre-sizing buffers.
std::size_t framed_size(const AnyMessage& msg);

}  // namespace mdfeed::itch
