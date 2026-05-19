# Architecture

## Component map

```
publisher (sim) ─── multicast UDP (239.0.0.x) ───► MulticastReceiver
                                                       │
                                                       ▼
                                                  FeedHandler
                                              ┌────────┴────────┐
                                              │                 │
                                              ▼                 ▼
                                         GapDetector          Parser
                                              │                 │
                            (on gap)──────────┘                 ▼
                                              ▼            DepthBook
                                       SnapshotClient          │
                                              │                ▼
                                              ▼          DepthSnapshot
                                          TcpClient        callback
                                              │
                                              ▼
                                       SnapshotServer (sim)
```

## Modules

| Path                       | Responsibility                                   |
|----------------------------|--------------------------------------------------|
| `src/core/types.h`         | Primitive types (OrderId, Price, Symbol, Side)   |
| `src/core/itch_messages.h` | Decoded message structs and `AnyMessage` variant |
| `src/core/parser.cpp`      | Encode and decode ITCH wire frames               |
| `src/core/depth_book.cpp`  | Per-symbol depth-N order book                    |
| `src/core/feed_handler.cpp`| Transport → gap detect → parse → apply           |
| `src/net/multicast.cpp`    | UDP multicast receive / send (POSIX)             |
| `src/net/tcp_recovery.cpp` | TCP client / server primitives                   |
| `src/recovery/gap_detector.cpp` | Per-locate sequence tracking                |
| `src/recovery/snapshot_client.cpp` | Snapshot frame encode/decode             |
| `src/sim/publisher.cpp`    | Deterministic ITCH message generator             |
| `src/sim/snapshot_server.cpp` | In-test snapshot server                       |
| `src/sim/book_snapshot.cpp` | Book-snapshot wire format encode/decode + CRC   |
| `src/sim/book_snapshot_publisher.cpp` | TCP service pushing book snapshots    |
| `src/sim/book_snapshot_subscriber.cpp` | TCP client rebuilding a local book   |
| `src/pcap/pcap_io.cpp`     | libpcap-format offline capture reader / writer   |
| `src/pcap_replay.cpp`      | Offline pcap replay tool                         |
| `src/pcap_gen.cpp`         | Generator for the committed pcap test captures   |
| `src/obs/clock.cpp`        | `steady_clock`-based monotonic nanosecond clock  |
| `src/obs/histogram.cpp`    | Log-linear latency histogram                     |

## ITCH wire layouts

See `docs/itch-wire.md` for the exact byte layouts of the 10 message types
implemented in v1.

## Depth-10 book invariants

See `docs/depth-book.md`. The key invariants enforced after every mutation:

- Bid prices descending, ask prices ascending.
- Each visible level has nonzero qty and order_count.
- Visible level count is capped at the configured depth.

## Gap detection and snapshot+gap-fill recovery

See `docs/gap-fill-recovery.md` for the algorithm, the wire protocol of the
TCP control channel, and the ordering rules during catch-up.

## Multicast loopback in CI

GitHub Actions Ubuntu runners support multicast over the `lo` interface. The
relevant setsockopt calls are:

- Sender: `IP_MULTICAST_LOOP = 1`, `IP_MULTICAST_IF = 127.0.0.1`, TTL 1.
- Receiver: `SO_REUSEADDR`, bind `0.0.0.0:<port>` (not the group address),
  `IP_ADD_MEMBERSHIP` for `(group, 127.0.0.1)`.

Tests pick deterministic ports keyed off the PID to avoid collisions when
multiple test binaries run in parallel.

## Offline pcap replay

`pcap_replay` reads a libpcap capture of ITCH multicast packets and replays
the datagrams through the same `FeedHandler` as the live path. The capture is
an Ethernet / IPv4 / UDP framing of the transport datagram; the reader strips
the fixed 42-byte header prefix. See `docs/pcap-replay.md` for the CLI, the
capture format, and the committed test inputs.

## Book snapshot publishing

`BookSnapshotPublisher` is a TCP service that pushes a binary snapshot of the
full depth-10 book, every symbol, to connected subscribers. A snapshot is
triggered by either a time interval or an applied-message stride
(`BookSnapshotPolicy`); setting a trigger to 0 disables it. A subscriber
(`BookSnapshotSubscriber`) connects, reads length-prefixed snapshot frames,
verifies the CRC, and rebuilds its own depth-10 book per symbol. The wire
layout is in `docs/snapshot-wire.md`.

### Hot-path isolation

The multicast receive thread must never block on a slow subscriber. The
publisher enforces this in two phases:

1. **Capture under lock.** The publisher thread acquires the same mutex the
   feed uses to guard book mutations, copies out a `BookSnapshot` value, and
   releases the lock. This window holds nothing but a memory copy; no socket
   calls happen inside it.
2. **Serialise and write outside the lock.** Encoding the frame and writing
   it to every subscriber socket happens entirely after the lock is released.
   A stalled subscriber whose TCP buffer is full can delay the publisher's
   own loop but can never back-pressure the feed thread.

The feed hot path only calls `note_message()`, which is a single relaxed
atomic increment. The integration test `book_snapshot_pubsub_test` asserts
both properties: the reconstructed book is byte-equal to the publisher's
reference across all symbols, and a deliberately stalled subscriber does not
prevent the publisher from publishing or block the lock-acquiring feed
thread.

## Build matrix

| Job in CI       | Toolchain             | Purpose                              |
|-----------------|-----------------------|--------------------------------------|
| format-check    | clang-format          | Style enforcement                    |
| build-gcc       | g++ + libstdc++       | Default Linux build, all tests, pcap replay smoke |
| build-clang     | clang++ + libstdc++   | Clang build, all tests               |
| asan-ubsan      | clang++ + ASan + UBSan| Sanitized run of every test          |
| fuzz-smoke      | clang++ + libFuzzer   | 10,000 iters of `parser_fuzz`        |
| bench-smoke     | g++                   | 100,000-message bench + cache study  |
| bench-regress   | g++                   | 1M-message throughput regression gate |
| docker          | buildx                | amd64 + arm64 multi-platform image   |

All build jobs install `libpcap-dev`, which the offline replay path links.
