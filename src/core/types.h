#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace mdfeed::itch {

using OrderId = std::uint64_t;
using Price = std::uint32_t;   // ITCH price is 4 bytes, scaled by 10000
using Quantity = std::uint32_t;
using Timestamp = std::uint64_t; // ITCH timestamp is 6 bytes nanoseconds since midnight; we widen to 8
using StockLocate = std::uint16_t;
using TrackingNumber = std::uint16_t;
using SequenceNumber = std::uint64_t;

constexpr std::size_t kSymbolLen = 8;
using Symbol = std::array<char, kSymbolLen>;

inline Symbol make_symbol(std::string_view s) {
    Symbol out{};
    out.fill(' ');
    for (std::size_t i = 0; i < s.size() && i < kSymbolLen; ++i) {
        out[i] = s[i];
    }
    return out;
}

inline std::string symbol_to_string(const Symbol& s) {
    std::string out(s.data(), kSymbolLen);
    auto end = out.find_last_not_of(' ');
    if (end == std::string::npos) {
        out.clear();
    } else {
        out.resize(end + 1);
    }
    return out;
}

enum class Side : std::uint8_t {
    Buy = 'B',
    Sell = 'S',
};

}  // namespace mdfeed::itch
