# Book-snapshot wire format

The `BookSnapshotPublisher` pushes a binary snapshot of every tracked symbol
to connected subscribers. This document is the normative byte layout; the
authoritative source is `src/sim/book_snapshot.h`.

All integers are big-endian. A frame is a 4-byte length prefix followed by a
body. The body begins with a 4-byte magic and ends with a CRC-32 covering the
body from the magic byte up to (but not including) the CRC itself.

## Frame

| Field         | Type    | Notes                                            |
|---------------|---------|--------------------------------------------------|
| body_length   | u32     | byte count of everything after this field        |
| magic         | u8[4]   | ASCII `BSN1`                                      |
| sequence      | u64     | monotonic publish counter, starts at 1            |
| symbol_count  | u32     | number of `SymbolRecord` entries                  |
| symbols       | repeated `SymbolRecord` |                                  |
| crc32         | u32     | CRC-32 of `magic .. last SymbolRecord`            |

## SymbolRecord

| Field            | Type      | Notes                                       |
|------------------|-----------|---------------------------------------------|
| symbol           | u8[8]     | space-padded ITCH symbol                     |
| bid_level_count  | u8        | 0..10                                        |
| bids             | repeated `WireLevel` | `bid_level_count` entries, best bid first |
| ask_level_count  | u8        | 0..10                                        |
| asks             | repeated `WireLevel` | `ask_level_count` entries, best ask first |

## WireLevel

| Field        | Type | Notes                          |
|--------------|------|--------------------------------|
| price        | u32  | ITCH price, scaled by 10000    |
| total_qty    | u32  | summed quantity at the level   |
| order_count  | u32  | resting orders at the level    |

## Length and CRC guards

Two independent guards protect a decoder:

- **Length prefix.** The 4-byte `body_length` lets a reader pull exactly one
  frame off a stream socket. `tcp_recv_frame` rejects a zero length or one
  above 16 MiB.
- **CRC-32.** The trailing `crc32` (IEEE 802.3, reflected, polynomial
  `0xEDB88320`) is verified over the whole body before any field is trusted.
  A flipped byte, a truncated record, or a bad magic all fail the decode and
  `decode_book_snapshot` returns `ok = false`.

Per-record bounds are also checked while walking the body: each level count
must be `<= 10` and the running cursor must never pass the CRC offset.

## Reconstruction

A subscriber treats each frame as authoritative for the symbols it carries.
`BookSnapshotSubscriber::receive_one` replaces the local depth-10 state for
every symbol in the frame, so after applying a frame taken at the publisher's
terminal book state the subscriber's reconstruction is byte-equal to the
publisher's reference for all symbols. The integration test
`book_snapshot_pubsub_test` verifies this over a loopback TCP connection
after a multicast feed run.
