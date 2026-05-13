# ITCH 5.0 wire layouts (subset implemented in v1)

All multi-byte fields are big-endian. Each message on the wire is preceded by
a 2-byte big-endian length prefix that covers everything from the 1-byte type
code through the end of the body. The total framed size is therefore
`2 + length`.

Field abbreviations: `u8`, `u16`, `u32`, `u64` are unsigned integers of the
given byte width. `u48` is a 6-byte big-endian unsigned integer used for
ITCH's nanoseconds-since-midnight timestamp; the parser widens it to a 64-bit
host-endian value. `sym[8]` is an 8-byte stock symbol, right-padded with
spaces.

Header fields shared by every message:

| Offset (from type) | Field            | Type  |
|--------------------|------------------|-------|
| 0                  | type             | u8    |
| 1                  | stock_locate     | u16   |
| 3                  | tracking_number  | u16   |
| 5                  | ts (ns-of-day)   | u48   |

## S - System Event (length 12)

| Offset | Field      | Type | Notes                          |
|--------|------------|------|--------------------------------|
| 11     | event_code | u8   | 'O' start of messages, 'C' end |

## R - Stock Directory (length 39)

| Offset | Field            | Type    |
|--------|------------------|---------|
| 11     | symbol           | sym[8]  |
| 19     | market_category  | u8      |
| 20     | financial_status | u8      |
| 21     | round_lot_size   | u32     |

## A - Add Order (length 36)

| Offset | Field        | Type    |
|--------|--------------|---------|
| 11     | order_id     | u64     |
| 19     | side         | u8 ('B' or 'S') |
| 20     | shares       | u32     |
| 24     | symbol       | sym[8]  |
| 32     | price        | u32     |

## F - Add Order with MPID (length 40)

Same as A plus 4-byte attribution identifier appended.

| Offset | Field        | Type    |
|--------|--------------|---------|
| 11..35 | (as A)       |         |
| 36     | attribution  | u8[4]   |

## E - Order Executed (length 31)

| Offset | Field           | Type |
|--------|-----------------|------|
| 11     | order_id        | u64  |
| 19     | executed_shares | u32  |
| 23     | match_number    | u64  |

## C - Order Executed With Price (length 36)

| Offset | Field           | Type |
|--------|-----------------|------|
| 11..30 | (as E)          |      |
| 31     | printable       | u8   |
| 32     | execution_price | u32  |

## X - Order Cancel (length 23)

| Offset | Field           | Type |
|--------|-----------------|------|
| 11     | order_id        | u64  |
| 19     | canceled_shares | u32  |

## D - Order Delete (length 19)

| Offset | Field    | Type |
|--------|----------|------|
| 11     | order_id | u64  |

## U - Order Replace (length 35)

| Offset | Field             | Type |
|--------|-------------------|------|
| 11     | original_order_id | u64  |
| 19     | new_order_id      | u64  |
| 27     | shares            | u32  |
| 31     | price             | u32  |

## P - Trade (non-cross, length 44)

| Offset | Field        | Type    |
|--------|--------------|---------|
| 11     | order_id     | u64     |
| 19     | side         | u8      |
| 20     | shares       | u32     |
| 24     | symbol       | sym[8]  |
| 32     | price        | u32     |
| 36     | match_number | u64     |

## Transport framing (multicast payload)

Each multicast datagram body wraps a single ITCH frame with a per-stock-locate
sequence number:

| Offset | Field        | Type | Notes                              |
|--------|--------------|------|------------------------------------|
| 0      | stock_locate | u16  | matches the ITCH message header    |
| 2      | sequence     | u64  | 1-based per locate; 0 = bootstrap  |
| 10     | length       | u16  | ITCH length prefix (see above)     |
| 12     | type+body    | u8 + N bytes |                            |
