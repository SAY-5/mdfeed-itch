#include "core/parser.h"

#include <cstring>

namespace mdfeed::itch {

namespace {

inline std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

inline std::uint32_t read_u32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline std::uint64_t read_u48(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(p[0]) << 40) | (static_cast<std::uint64_t>(p[1]) << 32) |
           (static_cast<std::uint64_t>(p[2]) << 24) | (static_cast<std::uint64_t>(p[3]) << 16) |
           (static_cast<std::uint64_t>(p[4]) << 8) | static_cast<std::uint64_t>(p[5]);
}

inline std::uint64_t read_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    }
    return v;
}

inline void write_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

inline void write_u32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

inline void write_u48(std::uint8_t* p, std::uint64_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 40);
    p[1] = static_cast<std::uint8_t>(v >> 32);
    p[2] = static_cast<std::uint8_t>(v >> 24);
    p[3] = static_cast<std::uint8_t>(v >> 16);
    p[4] = static_cast<std::uint8_t>(v >> 8);
    p[5] = static_cast<std::uint8_t>(v);
}

inline void write_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (56 - i * 8));
    }
}

inline void read_symbol(const std::uint8_t* p, Symbol& out) {
    std::memcpy(out.data(), p, kSymbolLen);
}
inline void write_symbol(std::uint8_t* p, const Symbol& s) {
    std::memcpy(p, s.data(), kSymbolLen);
}

}  // namespace

ParseResult decode_frame(std::span<const std::uint8_t> bytes) {
    ParseResult r;
    if (bytes.size() < 2) {
        r.status = ParseStatus::ShortBuffer;
        return r;
    }
    const std::uint16_t length = read_u16(bytes.data());
    const std::size_t total = 2u + length;
    if (bytes.size() < total) {
        r.status = ParseStatus::ShortBuffer;
        return r;
    }
    if (length < 1) {
        r.status = ParseStatus::LengthMismatch;
        r.consumed = total;
        return r;
    }
    const std::uint8_t* body = bytes.data() + 2;
    const char type = static_cast<char>(body[0]);
    const std::uint8_t* p = body + 1;
    const std::size_t body_len = length;

    auto check = [&](std::size_t expected) { return body_len == expected; };

    switch (static_cast<MessageType>(type)) {
        case MessageType::SystemEvent: {
            if (!check(kWireSize_S)) {
                r.status = ParseStatus::LengthMismatch;
                r.consumed = total;
                return r;
            }
            SystemEventMsg m{};
            m.stock_locate = read_u16(p);
            p += 2;
            m.tracking_number = read_u16(p);
            p += 2;
            m.ts = read_u48(p);
            p += 6;
            m.event_code = static_cast<char>(*p);
            r.message = m;
            break;
        }
        case MessageType::StockDirectory: {
            if (!check(kWireSize_R)) {
                r.status = ParseStatus::LengthMismatch;
                r.consumed = total;
                return r;
            }
            StockDirectoryMsg m{};
            m.stock_locate = read_u16(p);
            p += 2;
            m.tracking_number = read_u16(p);
            p += 2;
            m.ts = read_u48(p);
            p += 6;
            read_symbol(p, m.symbol);
            p += kSymbolLen;
            m.market_category = static_cast<char>(*p++);
            m.financial_status = static_cast<char>(*p++);
            m.round_lot_size = read_u32(p);
            r.message = m;
            break;
        }
        case MessageType::AddOrder: {
            if (!check(kWireSize_A)) {
                r.status = ParseStatus::LengthMismatch;
                r.consumed = total;
                return r;
            }
            AddOrderMsg m{};
            m.stock_locate = read_u16(p);
            p += 2;
            m.tracking_number = read_u16(p);
            p += 2;
            m.ts = read_u48(p);
            p += 6;
            m.order_id = read_u64(p);
            p += 8;
            m.side = static_cast<Side>(*p++);
            m.shares = read_u32(p);
            p += 4;
            read_symbol(p, m.symbol);
            p += kSymbolLen;
            m.price = read_u32(p);
            r.message = m;
            break;
        }
        case MessageType::AddOrderMPID: {
            if (!check(kWireSize_F)) {
                r.status = ParseStatus::LengthMismatch;
                r.consumed = total;
                return r;
            }
            AddOrderMPIDMsg m{};
            m.base.stock_locate = read_u16(p);
            p += 2;
            m.base.tracking_number = read_u16(p);
            p += 2;
            m.base.ts = read_u48(p);
            p += 6;
            m.base.order_id = read_u64(p);
            p += 8;
            m.base.side = static_cast<Side>(*p++);
            m.base.shares = read_u32(p);
            p += 4;
            read_symbol(p, m.base.symbol);
            p += kSymbolLen;
            m.base.price = read_u32(p);
            p += 4;
            std::memcpy(m.attribution.data(), p, 4);
            r.message = m;
            break;
        }
        case MessageType::OrderExecuted: {
            if (!check(kWireSize_E)) {
                r.status = ParseStatus::LengthMismatch;
                r.consumed = total;
                return r;
            }
            OrderExecutedMsg m{};
            m.stock_locate = read_u16(p);
            p += 2;
            m.tracking_number = read_u16(p);
            p += 2;
            m.ts = read_u48(p);
            p += 6;
            m.order_id = read_u64(p);
            p += 8;
            m.executed_shares = read_u32(p);
            p += 4;
            m.match_number = read_u64(p);
            r.message = m;
            break;
        }
        case MessageType::OrderExecutedWithPrice: {
            if (!check(kWireSize_C)) {
                r.status = ParseStatus::LengthMismatch;
                r.consumed = total;
                return r;
            }
            OrderExecutedWithPriceMsg m{};
            m.base.stock_locate = read_u16(p);
            p += 2;
            m.base.tracking_number = read_u16(p);
            p += 2;
            m.base.ts = read_u48(p);
            p += 6;
            m.base.order_id = read_u64(p);
            p += 8;
            m.base.executed_shares = read_u32(p);
            p += 4;
            m.base.match_number = read_u64(p);
            p += 8;
            m.printable = static_cast<char>(*p++);
            m.execution_price = read_u32(p);
            r.message = m;
            break;
        }
        case MessageType::OrderCancel: {
            if (!check(kWireSize_X)) {
                r.status = ParseStatus::LengthMismatch;
                r.consumed = total;
                return r;
            }
            OrderCancelMsg m{};
            m.stock_locate = read_u16(p);
            p += 2;
            m.tracking_number = read_u16(p);
            p += 2;
            m.ts = read_u48(p);
            p += 6;
            m.order_id = read_u64(p);
            p += 8;
            m.canceled_shares = read_u32(p);
            r.message = m;
            break;
        }
        case MessageType::OrderDelete: {
            if (!check(kWireSize_D)) {
                r.status = ParseStatus::LengthMismatch;
                r.consumed = total;
                return r;
            }
            OrderDeleteMsg m{};
            m.stock_locate = read_u16(p);
            p += 2;
            m.tracking_number = read_u16(p);
            p += 2;
            m.ts = read_u48(p);
            p += 6;
            m.order_id = read_u64(p);
            r.message = m;
            break;
        }
        case MessageType::OrderReplace: {
            if (!check(kWireSize_U)) {
                r.status = ParseStatus::LengthMismatch;
                r.consumed = total;
                return r;
            }
            OrderReplaceMsg m{};
            m.stock_locate = read_u16(p);
            p += 2;
            m.tracking_number = read_u16(p);
            p += 2;
            m.ts = read_u48(p);
            p += 6;
            m.original_order_id = read_u64(p);
            p += 8;
            m.new_order_id = read_u64(p);
            p += 8;
            m.shares = read_u32(p);
            p += 4;
            m.price = read_u32(p);
            r.message = m;
            break;
        }
        case MessageType::Trade: {
            if (!check(kWireSize_P)) {
                r.status = ParseStatus::LengthMismatch;
                r.consumed = total;
                return r;
            }
            TradeMsg m{};
            m.stock_locate = read_u16(p);
            p += 2;
            m.tracking_number = read_u16(p);
            p += 2;
            m.ts = read_u48(p);
            p += 6;
            m.order_id = read_u64(p);
            p += 8;
            m.side = static_cast<Side>(*p++);
            m.shares = read_u32(p);
            p += 4;
            read_symbol(p, m.symbol);
            p += kSymbolLen;
            m.price = read_u32(p);
            p += 4;
            m.match_number = read_u64(p);
            r.message = m;
            break;
        }
        default:
            r.status = ParseStatus::UnknownType;
            r.consumed = total;
            return r;
    }

    r.status = ParseStatus::Ok;
    r.consumed = total;
    return r;
}

std::size_t framed_size(const AnyMessage& msg) {
    return std::visit(
        [](auto&& m) -> std::size_t {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, SystemEventMsg>)
                return 2 + kWireSize_S;
            else if constexpr (std::is_same_v<T, StockDirectoryMsg>)
                return 2 + kWireSize_R;
            else if constexpr (std::is_same_v<T, AddOrderMsg>)
                return 2 + kWireSize_A;
            else if constexpr (std::is_same_v<T, AddOrderMPIDMsg>)
                return 2 + kWireSize_F;
            else if constexpr (std::is_same_v<T, OrderExecutedMsg>)
                return 2 + kWireSize_E;
            else if constexpr (std::is_same_v<T, OrderExecutedWithPriceMsg>)
                return 2 + kWireSize_C;
            else if constexpr (std::is_same_v<T, OrderCancelMsg>)
                return 2 + kWireSize_X;
            else if constexpr (std::is_same_v<T, OrderDeleteMsg>)
                return 2 + kWireSize_D;
            else if constexpr (std::is_same_v<T, OrderReplaceMsg>)
                return 2 + kWireSize_U;
            else if constexpr (std::is_same_v<T, TradeMsg>)
                return 2 + kWireSize_P;
            else
                return 0;
        },
        msg);
}

namespace {

template <typename Body>
std::size_t write_header(std::span<std::uint8_t> out, std::size_t wire_size, char type,
                         const Body& fields) {
    if (out.size() < 2 + wire_size) return 0;
    write_u16(out.data(), static_cast<std::uint16_t>(wire_size));
    out[2] = static_cast<std::uint8_t>(type);
    write_u16(out.data() + 3, fields.stock_locate);
    write_u16(out.data() + 5, fields.tracking_number);
    write_u48(out.data() + 7, fields.ts);
    return 2 + wire_size;
}

}  // namespace

std::size_t encode_frame(const AnyMessage& msg, std::span<std::uint8_t> out) {
    return std::visit(
        [&](auto&& m) -> std::size_t {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, SystemEventMsg>) {
                if (auto n = write_header(out, kWireSize_S, 'S', m); n == 0) return 0;
                out[13] = static_cast<std::uint8_t>(m.event_code);
                return 2 + kWireSize_S;
            } else if constexpr (std::is_same_v<T, StockDirectoryMsg>) {
                if (auto n = write_header(out, kWireSize_R, 'R', m); n == 0) return 0;
                write_symbol(out.data() + 13, m.symbol);
                out[21] = static_cast<std::uint8_t>(m.market_category);
                out[22] = static_cast<std::uint8_t>(m.financial_status);
                write_u32(out.data() + 23, m.round_lot_size);
                return 2 + kWireSize_R;
            } else if constexpr (std::is_same_v<T, AddOrderMsg>) {
                if (auto n = write_header(out, kWireSize_A, 'A', m); n == 0) return 0;
                write_u64(out.data() + 13, m.order_id);
                out[21] = static_cast<std::uint8_t>(m.side);
                write_u32(out.data() + 22, m.shares);
                write_symbol(out.data() + 26, m.symbol);
                write_u32(out.data() + 34, m.price);
                return 2 + kWireSize_A;
            } else if constexpr (std::is_same_v<T, AddOrderMPIDMsg>) {
                if (auto n = write_header(out, kWireSize_F, 'F', m.base); n == 0) return 0;
                write_u64(out.data() + 13, m.base.order_id);
                out[21] = static_cast<std::uint8_t>(m.base.side);
                write_u32(out.data() + 22, m.base.shares);
                write_symbol(out.data() + 26, m.base.symbol);
                write_u32(out.data() + 34, m.base.price);
                std::memcpy(out.data() + 38, m.attribution.data(), 4);
                return 2 + kWireSize_F;
            } else if constexpr (std::is_same_v<T, OrderExecutedMsg>) {
                if (auto n = write_header(out, kWireSize_E, 'E', m); n == 0) return 0;
                write_u64(out.data() + 13, m.order_id);
                write_u32(out.data() + 21, m.executed_shares);
                write_u64(out.data() + 25, m.match_number);
                return 2 + kWireSize_E;
            } else if constexpr (std::is_same_v<T, OrderExecutedWithPriceMsg>) {
                if (auto n = write_header(out, kWireSize_C, 'C', m.base); n == 0) return 0;
                write_u64(out.data() + 13, m.base.order_id);
                write_u32(out.data() + 21, m.base.executed_shares);
                write_u64(out.data() + 25, m.base.match_number);
                out[33] = static_cast<std::uint8_t>(m.printable);
                write_u32(out.data() + 34, m.execution_price);
                return 2 + kWireSize_C;
            } else if constexpr (std::is_same_v<T, OrderCancelMsg>) {
                if (auto n = write_header(out, kWireSize_X, 'X', m); n == 0) return 0;
                write_u64(out.data() + 13, m.order_id);
                write_u32(out.data() + 21, m.canceled_shares);
                return 2 + kWireSize_X;
            } else if constexpr (std::is_same_v<T, OrderDeleteMsg>) {
                if (auto n = write_header(out, kWireSize_D, 'D', m); n == 0) return 0;
                write_u64(out.data() + 13, m.order_id);
                return 2 + kWireSize_D;
            } else if constexpr (std::is_same_v<T, OrderReplaceMsg>) {
                if (auto n = write_header(out, kWireSize_U, 'U', m); n == 0) return 0;
                write_u64(out.data() + 13, m.original_order_id);
                write_u64(out.data() + 21, m.new_order_id);
                write_u32(out.data() + 29, m.shares);
                write_u32(out.data() + 33, m.price);
                return 2 + kWireSize_U;
            } else if constexpr (std::is_same_v<T, TradeMsg>) {
                if (auto n = write_header(out, kWireSize_P, 'P', m); n == 0) return 0;
                write_u64(out.data() + 13, m.order_id);
                out[21] = static_cast<std::uint8_t>(m.side);
                write_u32(out.data() + 22, m.shares);
                write_symbol(out.data() + 26, m.symbol);
                write_u32(out.data() + 34, m.price);
                write_u64(out.data() + 38, m.match_number);
                return 2 + kWireSize_P;
            } else {
                return 0;
            }
        },
        msg);
}

}  // namespace mdfeed::itch
