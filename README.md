# mdfeed-itch

C++20 NASDAQ TotalView-ITCH 5.0 multicast feed handler. Parses the ITCH wire
format off a UDP multicast group, maintains a depth-10 order book per symbol,
detects packet gaps via per-stock-locate sequence numbers, and recovers via
a snapshot+gap-fill request over a TCP control channel.

## What this studies

- **ITCH 5.0 wire decode.** Ten message types (`S R A F E C X D U P`) with
  big-endian, packed, variable-length frames over a 2-byte length-prefixed
  transport. Zero-copy parse into a tagged union.
- **POSIX UDP multicast.** Joining a group on `127.0.0.1` with
  `IP_ADD_MEMBERSHIP`, sending with `IP_MULTICAST_LOOP` on so the same host
  can receive its own packets.
- **Depth-N order books.** Per-symbol bid and ask ladders capped at the top
  10 levels, with full `OrderId → OrderState` tracking so cancel / execute /
  delete / replace messages mutate the ladder correctly.
- **Gap detection.** Per-stock-locate expected sequence numbers; stale
  packets dropped, gaps surfaced to the recovery layer.
- **Snapshot+gap-fill recovery.** A TCP control channel serves a consistent
  snapshot at `last_applied_seq`; the handler re-syncs and continues from the
  next multicast packet that advances the stream.

## How this differs from `SAY-5/mdfeed-handler`

| Axis              | `mdfeed-handler`                  | `mdfeed-itch` (this repo)               |
|-------------------|-----------------------------------|------------------------------------------|
| Wire protocol     | Custom binary + ASCII venues      | NASDAQ TotalView-ITCH 5.0 (subset)       |
| Transport         | UDP unicast on loopback           | UDP multicast (`239.0.0.x`) on loopback  |
| Book model        | Best bid / best offer (BBO)       | Depth-10 ladder per symbol               |
| Recovery          | None                              | Snapshot + gap-fill over TCP             |
| Sequence model    | Implicit / per-stream arrival     | Explicit per-stock-locate sequence       |

Long form: `docs/differs-from-mdfeed-handler.md`.

## Architecture

```
publisher (sim) ─── multicast UDP (239.0.0.x) ──► MulticastReceiver
                                                       │
                                                       ▼
                                                  FeedHandler
                                              ┌────────┴────────┐
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

Full module map: `ARCHITECTURE.md`.

## Bench numbers

From `bench/results/bench_local.json`, hardware: Apple M2 Pro (10 cores),
macOS 26.0.1, Apple clang 17.0.0, Release build (`-O3`). Single-threaded
end-to-end parse + book-apply over 1,000,000 generated ITCH messages, with a
sampled-latency pass (`obs::Histogram` over 200,000 messages):

| Metric                    | Value         |
|---------------------------|---------------|
| Sustained throughput      | 352,845 msgs/sec |
| Latency P50               | 288 ns        |
| Latency P95               | 704 ns        |
| Latency P99               | 1,280 ns      |
| Latency P99.9             | 2,496 ns      |
| Latency mean              | 520.2 ns      |
| Latency max               | 30,564,500 ns |

Gap-fill recovery test: 1,500 messages published over multicast loopback,
every 100th packet dropped at the receiver. The handler detects every gap,
requests a TCP snapshot, applies it, and converges to byte-equal book state
with the shadow.

To reproduce:

```
make build
./build/handler_bench --count 1000000 --out bench/results/bench_local.json
```

## What this is NOT

- **No production exchange connectivity.** Loopback multicast only. No
  real-network NASDAQ feed, no TLS, no MoldUDP64.
- **No UDP unicast.** That is `SAY-5/mdfeed-handler`.
- **No FIX.** Use `SAY-5/orderbook-fix` for order-entry over FIX.
- **No order entry at all.** This is read-side market data.
- **No full historical replay.** No `.itch` file ingestion. No PCAP loader.
- **No IGMPv3 source-specific multicast.** ASM only.
- **No kernel-bypass.** POSIX sockets only. No DPDK, no Solarflare onload,
  no AF_XDP.
- **No NIC offloads.** No hardware timestamping, no checksum offload tuning.

## Build and run

```
make build              # release build, all targets
make test               # ctest, all tests
make bench              # run the bench

# sanitizers
make asan               # ASan + UBSan, all tests

# fuzz
make fuzz               # libFuzzer build of parser_fuzz
./build-fuzz/parser_fuzz -runs=5000 -max_len=1024
```

Docker (multi-stage):

```
docker compose build
docker compose up
```

## Repository layout

```
mdfeed-itch/
├── CMakeLists.txt
├── Makefile
├── docker-compose.yml
├── Dockerfile
├── src/
│   ├── core/        # types, ITCH messages, parser, depth book, feed handler
│   ├── net/         # multicast and TCP socket helpers
│   ├── recovery/    # gap detector, snapshot client
│   ├── sim/         # publisher, snapshot server
│   ├── obs/         # clock, histogram
│   └── main.cpp
├── tests/
│   ├── unit/        # parser, depth book, gap detector, snapshot client
│   ├── fuzz/        # libFuzzer target for the parser
│   └── integration/ # multicast loopback, gap-fill recovery
├── bench/           # handler bench, results
├── docs/            # itch-wire, depth-book, gap-fill-recovery, differs
└── .github/workflows/ci.yml
```

3,635 LOC across C++ source, tests, and bench. 37 distinct test cases across
6 test executables. 100% test pass on Linux gcc, Linux clang, and the ASan +
UBSan sanitized build.

## License

MIT, see `LICENSE`.
