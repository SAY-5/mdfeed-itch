#pragma once

#include <cstdint>
#include <variant>

#include "core/types.h"

namespace mdfeed::itch {

// ITCH 5.0 message type codes (subset implemented in v1).
enum class MessageType : char {
    SystemEvent = 'S',
    StockDirectory = 'R',
    AddOrder = 'A',
    AddOrderMPID = 'F',
    OrderExecuted = 'E',
    OrderExecutedWithPrice = 'C',
    OrderCancel = 'X',
    OrderDelete = 'D',
    OrderReplace = 'U',
    Trade = 'P',
};

// Decoded forms. Network-byte-order conversion happens in the parser; these
// are host-endian native types ready for consumption.

struct SystemEventMsg {
    StockLocate stock_locate;
    TrackingNumber tracking_number;
    Timestamp ts;
    char event_code;  // 'O' start of messages, 'S' start of system hours, etc.
};

struct StockDirectoryMsg {
    StockLocate stock_locate;
    TrackingNumber tracking_number;
    Timestamp ts;
    Symbol symbol;
    char market_category;
    char financial_status;
    std::uint32_t round_lot_size;
};

struct AddOrderMsg {
    StockLocate stock_locate;
    TrackingNumber tracking_number;
    Timestamp ts;
    OrderId order_id;
    Side side;
    Quantity shares;
    Symbol symbol;
    Price price;
};

struct AddOrderMPIDMsg {
    AddOrderMsg base;
    std::array<char, 4> attribution;
};

struct OrderExecutedMsg {
    StockLocate stock_locate;
    TrackingNumber tracking_number;
    Timestamp ts;
    OrderId order_id;
    Quantity executed_shares;
    std::uint64_t match_number;
};

struct OrderExecutedWithPriceMsg {
    OrderExecutedMsg base;
    char printable;  // 'Y'/'N'
    Price execution_price;
};

struct OrderCancelMsg {
    StockLocate stock_locate;
    TrackingNumber tracking_number;
    Timestamp ts;
    OrderId order_id;
    Quantity canceled_shares;
};

struct OrderDeleteMsg {
    StockLocate stock_locate;
    TrackingNumber tracking_number;
    Timestamp ts;
    OrderId order_id;
};

struct OrderReplaceMsg {
    StockLocate stock_locate;
    TrackingNumber tracking_number;
    Timestamp ts;
    OrderId original_order_id;
    OrderId new_order_id;
    Quantity shares;
    Price price;
};

struct TradeMsg {
    StockLocate stock_locate;
    TrackingNumber tracking_number;
    Timestamp ts;
    OrderId order_id;  // always 0 for non-cross trade but kept for parity
    Side side;
    Quantity shares;
    Symbol symbol;
    Price price;
    std::uint64_t match_number;
};

using AnyMessage = std::variant<SystemEventMsg, StockDirectoryMsg, AddOrderMsg, AddOrderMPIDMsg,
                                OrderExecutedMsg, OrderExecutedWithPriceMsg, OrderCancelMsg,
                                OrderDeleteMsg, OrderReplaceMsg, TradeMsg>;

// Wire sizes including the 1-byte type code. The outer transport wraps each
// message with a 2-byte big-endian length prefix.
constexpr std::size_t kWireSize_S = 12;
constexpr std::size_t kWireSize_R = 39;
constexpr std::size_t kWireSize_A = 36;
constexpr std::size_t kWireSize_F = 40;
constexpr std::size_t kWireSize_E = 31;
constexpr std::size_t kWireSize_C = 36;
constexpr std::size_t kWireSize_X = 23;
constexpr std::size_t kWireSize_D = 19;
constexpr std::size_t kWireSize_U = 35;
constexpr std::size_t kWireSize_P = 44;

}  // namespace mdfeed::itch
