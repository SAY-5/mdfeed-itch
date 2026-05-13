# Depth-N order book

## Goal

Maintain, per symbol, the top-N price levels for bids and asks. v1 defaults to
N = 10. Each price level reports:

- `price` (u32, ITCH-scaled)
- `total_qty` (u32, sum of shares of all live orders at this price)
- `order_count` (u32, number of live orders at this price)

## Data structures

Each `SymbolBook` owns:

- `unordered_map<OrderId, OrderState>` — every live order this book knows
  about. `OrderState` carries side, price, qty, timestamp. This is the index
  used by Execute / Cancel / Delete / Replace messages, all of which reference
  an order id rather than a price.
- `map<Price, PriceLevel, greater<Price>>` for bids and `map<Price, PriceLevel>`
  for asks. Sorted ladders so the top-N slice is the first N iterators; no
  rebuild on each update.
- `DepthSide top_bids_` and `top_asks_` — materialized top-N vectors refreshed
  after every level change.

## Invariants

- Bid prices in `top_bids_.levels` are strictly descending.
- Ask prices in `top_asks_.levels` are strictly ascending.
- `top_bids_.levels.size() <= depth_` and likewise for asks.
- Every visible level has `total_qty > 0` and `order_count > 0`.

`SymbolBook::invariants_ok()` checks all of the above and is used in the unit
tests after every mutation.

## Message handling

| ITCH message | Book operation                                                            |
|--------------|---------------------------------------------------------------------------|
| A / F        | insert order, increment level qty + count                                 |
| E / C        | reduce order qty by `executed_shares`; remove if it reaches zero          |
| X            | reduce order qty by `canceled_shares`; remove if it reaches zero          |
| D            | remove order entirely                                                     |
| U            | remove `original_order_id`; insert `new_order_id` at the replacement price |
| S / R        | no book impact (system event / symbol directory)                          |
| P            | non-cross trade; does not alter the visible order book                    |

## Snapshot emission

On every book mutation that changes state, `DepthBook` emits a `DepthSnapshot`
event via its configured callback. The snapshot carries the symbol, the bids
and asks (top-N), and the timestamp from the originating ITCH message.

Consumers downstream of the handler can subscribe to these events without
having to walk the internal ladder.

## Complexity

| Operation         | Cost                            |
|-------------------|---------------------------------|
| Add order         | `O(log L)` ladder insert + `O(N)` top-N refresh |
| Cancel / execute  | `O(1)` order lookup + `O(log L)` ladder update + `O(N)` refresh |
| Delete            | same as Cancel                  |
| Replace           | one delete + one add            |

Here `L` is the number of distinct price levels for the symbol; `N` is the
configured depth (10 by default).
