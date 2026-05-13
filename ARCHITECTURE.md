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

## Build matrix

| Job in CI       | Toolchain             | Purpose                              |
|-----------------|-----------------------|--------------------------------------|
| format-check    | clang-format          | Style enforcement                    |
| build-gcc       | g++ + libstdc++       | Default Linux build, all tests       |
| build-clang     | clang++ + libstdc++   | Clang build, all tests               |
| asan-ubsan      | clang++ + ASan + UBSan| Sanitized run of every test          |
| fuzz-smoke      | clang++ + libFuzzer   | 5,000 iters of `parser_fuzz`         |
| bench-smoke     | g++                   | 100,000-message bench, smoke         |
| docker          | buildx                | amd64 + arm64 multi-platform image   |
