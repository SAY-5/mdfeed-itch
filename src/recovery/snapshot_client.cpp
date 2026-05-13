#include "recovery/snapshot_client.h"

#include <cstring>

namespace mdfeed::itch {

namespace {

void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
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

void put_symbol(std::vector<std::uint8_t>& v, const Symbol& s) {
    for (std::size_t i = 0; i < kSymbolLen; ++i) v.push_back(static_cast<std::uint8_t>(s[i]));
}

void put_level(std::vector<std::uint8_t>& v, const PriceLevel& lvl) {
    put_u32(v, lvl.price);
    put_u32(v, lvl.total_qty);
    put_u32(v, lvl.order_count);
}

std::uint16_t take_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
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

// Prepend a u32 length header for the body of an op-coded frame.
std::vector<std::uint8_t> frame_with_header(char op, const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> out;
    out.reserve(5 + body.size());
    const std::uint32_t len = static_cast<std::uint32_t>(1 + body.size());
    put_u32(out, len);
    out.push_back(static_cast<std::uint8_t>(op));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

std::vector<std::uint8_t> encode_request(const SnapshotRequest& req) {
    std::vector<std::uint8_t> body;
    put_u16(body, req.stock_locate);
    put_u64(body, req.from_seq);
    return frame_with_header('Q', body);
}

std::vector<std::uint8_t> encode_response(const SnapshotResponse& resp) {
    std::vector<std::uint8_t> body;
    put_u16(body, resp.stock_locate);
    put_u64(body, resp.last_applied_seq);
    put_symbol(body, resp.symbol);
    put_u32(body, static_cast<std::uint32_t>(resp.bids.size()));
    for (const auto& l : resp.bids) put_level(body, l);
    put_u32(body, static_cast<std::uint32_t>(resp.asks.size()));
    for (const auto& l : resp.asks) put_level(body, l);
    return frame_with_header('S', body);
}

std::vector<std::uint8_t> encode_fault(const SnapshotFault& f) {
    std::vector<std::uint8_t> body;
    put_u16(body, f.stock_locate);
    body.push_back(f.code);
    return frame_with_header('F', body);
}

DecodedFrame decode_frame(const std::vector<std::uint8_t>& bytes, std::size_t& consumed) {
    consumed = 0;
    DecodedFrame out;
    if (bytes.size() < 5) return out;
    const std::uint32_t len = take_u32(bytes.data());
    if (bytes.size() < 4 + len) return out;
    const std::uint8_t* p = bytes.data() + 4;
    const std::uint8_t* end = p + len;
    const char op = static_cast<char>(*p++);
    switch (op) {
        case 'Q': {
            if (end - p < 10) return out;
            out.kind = DecodedFrame::Kind::Request;
            out.request.stock_locate = take_u16(p);
            p += 2;
            out.request.from_seq = take_u64(p);
            break;
        }
        case 'F': {
            if (end - p < 3) return out;
            out.kind = DecodedFrame::Kind::Fault;
            out.fault.stock_locate = take_u16(p);
            p += 2;
            out.fault.code = *p;
            break;
        }
        case 'S': {
            if (end - p < 2 + 8 + static_cast<std::ptrdiff_t>(kSymbolLen) + 4) return out;
            out.kind = DecodedFrame::Kind::Response;
            out.response.stock_locate = take_u16(p);
            p += 2;
            out.response.last_applied_seq = take_u64(p);
            p += 8;
            std::memcpy(out.response.symbol.data(), p, kSymbolLen);
            p += kSymbolLen;
            const std::uint32_t bn = take_u32(p);
            p += 4;
            if (end - p < static_cast<std::ptrdiff_t>(bn) * 12 + 4) return out;
            out.response.bids.reserve(bn);
            for (std::uint32_t i = 0; i < bn; ++i) {
                PriceLevel lvl{};
                lvl.price = take_u32(p);
                p += 4;
                lvl.total_qty = take_u32(p);
                p += 4;
                lvl.order_count = take_u32(p);
                p += 4;
                out.response.bids.push_back(lvl);
            }
            const std::uint32_t an = take_u32(p);
            p += 4;
            if (end - p < static_cast<std::ptrdiff_t>(an) * 12) return out;
            out.response.asks.reserve(an);
            for (std::uint32_t i = 0; i < an; ++i) {
                PriceLevel lvl{};
                lvl.price = take_u32(p);
                p += 4;
                lvl.total_qty = take_u32(p);
                p += 4;
                lvl.order_count = take_u32(p);
                p += 4;
                out.response.asks.push_back(lvl);
            }
            break;
        }
        default:
            return out;
    }
    consumed = 4 + len;
    return out;
}

}  // namespace mdfeed::itch
