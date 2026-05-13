# Gap detection and snapshot+gap-fill recovery

## Sequence model

The transport frame carries `(stock_locate, sequence)` on every multicast
datagram. Sequence numbers are 1-based per stock locate. `sequence == 0` is
reserved for bootstrap messages (stock directory) and bypasses gap checking.

The `GapDetector` tracks, per locate, the next expected sequence number.

| Observation                          | Result                                         |
|--------------------------------------|------------------------------------------------|
| `seq == expected`                    | OK, advance `expected` to `seq + 1`            |
| `seq < expected`                     | Stale, drop silently                           |
| `seq > expected`                     | Gap detected, do NOT advance `expected`        |
| Locate not seen before               | Bootstrap, set `expected = seq + 1`            |

`expected` is not advanced on a detected gap. The recovery path is responsible
for advancing it after applying a snapshot.

## Recovery flow

```
1. multicast datagram arrives at FeedHandler::on_datagram
2. GapDetector.observe(loc, seq) returns is_gap == true
3. FeedHandler.requester_(req, resp) is invoked
   - req.stock_locate = loc
   - req.from_seq = expected
4. The requester opens a TCP connection to the snapshot server,
   sends an encoded SnapshotRequest, and waits for a SnapshotResponse.
5. FeedHandler.apply_snapshot(resp) replaces the book state for the
   snapshot's symbol with the price levels in the response and sets
   GapDetector.expected[loc] = resp.last_applied_seq + 1.
6. Control returns to on_datagram. If the current message's seq is
   already covered by the snapshot (seq <= last_applied_seq), it is
   skipped. Otherwise it is decoded and applied.
```

## Wire protocol (TCP)

Length-prefixed frames: `[u32 length][op][body]`.

- `op == 'Q'`: SnapshotRequest, body = `[u16 stock_locate][u64 from_seq]`
- `op == 'S'`: SnapshotResponse, body =
  `[u16 stock_locate][u64 last_applied_seq][u8[8] symbol]
   [u32 bid_count][repeated PriceLevel][u32 ask_count][repeated PriceLevel]`
  with `PriceLevel = [u32 price][u32 total_qty][u32 order_count]`.
- `op == 'F'`: SnapshotFault, body = `[u16 stock_locate][u8 code]`.

The same code path serves both reactive gap-fill (triggered by a detected gap)
and on-demand resync (a caller can request a snapshot for `from_seq == 0`).

## Ordering during catch-up

Because the snapshot is taken atomically at the server, the response carries
the consistent state of the book at `last_applied_seq`. After
`apply_snapshot()`, the handler discards any subsequent multicast packet whose
sequence number is `<= last_applied_seq` and applies the rest in order.

## Failure modes

| Failure                                | Handling                              |
|----------------------------------------|---------------------------------------|
| `requester_` returns false             | Gap remains; next datagram retries    |
| TCP connect timeout                    | Treated as requester failure          |
| Snapshot for unknown locate            | Empty response, book left unchanged   |
| Multiple gaps before recovery returns  | Each new gap re-enters the recovery path |

## Tests

`tests/integration/gap_fill_test.cpp` drives the full flow:

1. Stand up a UDP multicast group on `239.0.0.2:<test-port>` and a TCP
   snapshot server on `127.0.0.1:<ephemeral>`.
2. Publisher emits 1500 ITCH messages across three symbols.
3. The test deliberately drops every 100th packet received from the multicast
   socket.
4. The handler observes gaps, requests snapshots, applies them.
5. After publishing finishes, a final per-symbol snapshot reconciles the
   handler's book to the shadow book.
6. Both books are compared level by level; quantities and counts must match.
