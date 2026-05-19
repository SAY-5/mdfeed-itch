#include "sim/book_snapshot.h"

#include <array>
#include <cstring>

namespace mdfeed::sim {

namespace {

void put_u8(std::vector<std::uint8_t>& v, std::uint8_t x) {
    v.push_back(x);
}
void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}
void put_u64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>(x >> (56 - i * 8)));
}

std::uint32_t take_u32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
std::uint64_t take_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    return v;
}

void put_level(std::vector<std::uint8_t>& v, const itch::PriceLevel& l) {
    put_u32(v, l.price);
    put_u32(v, l.total_qty);
    put_u32(v, l.order_count);
}

// Build the CRC-32 lookup table once.
const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> tbl = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return tbl;
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t len) {
    const auto& tbl = crc_table();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        c = tbl[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

BookSnapshot capture_book_snapshot(const itch::DepthBook& book, std::uint64_t sequence) {
    BookSnapshot snap;
    snap.sequence = sequence;
    for (const auto& sym : book.all_symbols()) {
        const itch::SymbolBook* sb = book.find(sym);
        if (sb == nullptr) continue;
        BookSnapshotSymbol rec;
        rec.symbol = sym;
        const auto& bids = sb->bids().levels;
        const auto& asks = sb->asks().levels;
        const std::size_t nb = bids.size() < kSnapshotMaxLevels ? bids.size() : kSnapshotMaxLevels;
        const std::size_t na = asks.size() < kSnapshotMaxLevels ? asks.size() : kSnapshotMaxLevels;
        rec.bids.assign(bids.begin(), bids.begin() + static_cast<std::ptrdiff_t>(nb));
        rec.asks.assign(asks.begin(), asks.begin() + static_cast<std::ptrdiff_t>(na));
        snap.symbols.push_back(std::move(rec));
    }
    return snap;
}

std::vector<std::uint8_t> encode_book_snapshot(const BookSnapshot& snap) {
    // Body: magic .. last record, then CRC. The 4-byte length prefix wraps it.
    std::vector<std::uint8_t> body;
    body.insert(body.end(), kSnapshotMagic, kSnapshotMagic + 4);
    put_u64(body, snap.sequence);
    put_u32(body, static_cast<std::uint32_t>(snap.symbols.size()));
    for (const auto& rec : snap.symbols) {
        body.insert(body.end(), rec.symbol.begin(), rec.symbol.end());
        put_u8(body, static_cast<std::uint8_t>(rec.bids.size()));
        for (const auto& l : rec.bids) put_level(body, l);
        put_u8(body, static_cast<std::uint8_t>(rec.asks.size()));
        for (const auto& l : rec.asks) put_level(body, l);
    }
    const std::uint32_t crc = crc32(body.data(), body.size());
    put_u32(body, crc);

    std::vector<std::uint8_t> frame;
    frame.reserve(4 + body.size());
    put_u32(frame, static_cast<std::uint32_t>(body.size()));
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

BookSnapshot decode_book_snapshot(const std::vector<std::uint8_t>& bytes, bool& ok) {
    ok = false;
    BookSnapshot snap;
    if (bytes.size() < 4) return snap;
    const std::uint32_t body_len = take_u32(bytes.data());
    if (bytes.size() < 4 + static_cast<std::size_t>(body_len)) return snap;
    const std::uint8_t* body = bytes.data() + 4;
    // Minimum body: magic(4) + seq(8) + count(4) + crc(4).
    if (body_len < 20) return snap;
    if (std::memcmp(body, kSnapshotMagic, 4) != 0) return snap;

    // Verify the CRC over magic .. last record before trusting any field.
    const std::uint32_t stored_crc = take_u32(body + body_len - 4);
    const std::uint32_t calc_crc = crc32(body, body_len - 4);
    if (stored_crc != calc_crc) return snap;

    const std::uint8_t* p = body + 4;
    const std::uint8_t* crc_start = body + body_len - 4;
    snap.sequence = take_u64(p);
    p += 8;
    const std::uint32_t sym_count = take_u32(p);
    p += 4;

    snap.symbols.reserve(sym_count);
    for (std::uint32_t s = 0; s < sym_count; ++s) {
        if (crc_start - p < static_cast<std::ptrdiff_t>(itch::kSymbolLen) + 1) return snap;
        BookSnapshotSymbol rec;
        std::memcpy(rec.symbol.data(), p, itch::kSymbolLen);
        p += itch::kSymbolLen;
        const std::uint8_t nb = *p++;
        if (nb > kSnapshotMaxLevels) return snap;
        if (crc_start - p < static_cast<std::ptrdiff_t>(nb) * 12 + 1) return snap;
        rec.bids.reserve(nb);
        for (std::uint8_t i = 0; i < nb; ++i) {
            itch::PriceLevel l{};
            l.price = take_u32(p);
            l.total_qty = take_u32(p + 4);
            l.order_count = take_u32(p + 8);
            p += 12;
            rec.bids.push_back(l);
        }
        const std::uint8_t na = *p++;
        if (na > kSnapshotMaxLevels) return snap;
        if (crc_start - p < static_cast<std::ptrdiff_t>(na) * 12) return snap;
        rec.asks.reserve(na);
        for (std::uint8_t i = 0; i < na; ++i) {
            itch::PriceLevel l{};
            l.price = take_u32(p);
            l.total_qty = take_u32(p + 4);
            l.order_count = take_u32(p + 8);
            p += 12;
            rec.asks.push_back(l);
        }
        snap.symbols.push_back(std::move(rec));
    }
    if (p != crc_start) return snap;
    ok = true;
    return snap;
}

}  // namespace mdfeed::sim
