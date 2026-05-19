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
macOS, Apple clang 17.0.0, Release build (`-O3`). Single-threaded end-to-end
parse + book-apply over 1,000,000 generated ITCH messages, with a
sampled-latency pass (`obs::Histogram` over 200,000 messages):

| Metric                    | Value             |
|---------------------------|-------------------|
| Sustained throughput      | 1,590,991 msgs/sec |
| Latency P50               | 250 ns            |
| Latency P95               | 500 ns            |
| Latency P99               | 664 ns            |
| Latency P99.9             | 4,288 ns          |
| Latency mean              | 293.0 ns          |

### Per-message-type throughput

Add / Execute / Cancel / Delete / Replace carry different book-mutation
costs. `handler_bench` groups the generated messages by ITCH type and times a
dedicated `DepthBook::apply` pass per type against a book pre-warmed with
every Add, isolating each type's cost:

| Type    | Cost shape                              | Throughput (msgs/sec) |
|---------|-----------------------------------------|-----------------------|
| Add     | insert order, maybe create price level  | ~1,978,000            |
| Execute | reduce qty on a live level              | ~1,091,000            |
| Cancel  | reduce qty on a live level              | ~1,020,000            |
| Delete  | erase order, maybe drop a price level   | ~788,000              |
| Replace | delete + add                            | ~505,000              |

Add is fastest because it never has to locate and may extend the ladder
in-place; Replace is slowest because it pays a delete and an add plus two
top-of-book refreshes.

### 1M-message multicast loopback

`multicast_bench` pushes the 1M-datagram feed through a real UDP multicast
group on `127.0.0.1` from a sender thread to a receiver thread. On the
reference machine the receiver delivered 1,000,008 of 1,000,008 datagrams
(100%, zero gaps detected) at ~144,000 datagrams/sec un-throttled. The
loopback datagram-delivery ceiling is well below the in-process parse+apply
rate above, so the network path, not the handler, is the scaling limit on a
single host. UDP is lossy by design; on a busier host the bench reports the
delivered fraction honestly and a `--pace-us` flag trades wall time for
delivery rate.

### Cache-line padding study

`bench/cache_study.cpp` compares the packed 12-byte `PriceLevel` against
16-byte padded and 64-byte-aligned variants over a top-of-book scan. The
packed layout is never slower; padding only enlarges the working set. Full
write-up and the reasoning: `docs/cache-padding-study.md`.

Gap-fill recovery test: 1,500 messages published over multicast loopback,
every 100th packet dropped at the receiver. The handler detects every gap,
requests a TCP snapshot, applies it, and converges to byte-equal book state
with the shadow.

To reproduce:

```
make build
./build/handler_bench --count 1000000 --out bench/results/bench_local.json
make bench-multicast            # 1M-message multicast loopback run
make cache-study                # cache-line padding comparison
make bench-regress              # 30% throughput-drift regression gate
```

### Regression gate

`make bench-regress` runs a 1M-message bench and fails if throughput drops
more than 30% below the committed baseline in
`bench/results/bench_ci_baseline.json`. The CI baseline is deliberately
conservative so the gate catches real regressions without flaking on
runner-to-runner variance; `bench/results/bench_local.json` carries the
developer-machine numbers. The gate runs in CI as the `bench-regress` job.

## pcap replay

`pcap_replay` reads a libpcap-format capture of ITCH multicast packets and
replays the datagrams through the same `FeedHandler` as the live multicast
path: identical decode, gap detection, and book maintenance, sourced from a
file. It uses libpcap (`pcap_open_offline` / `pcap_next_ex`); install
`libpcap-dev` (Debian/Ubuntu) or `libpcap` before building.

```
make build
./build/pcap_replay --pcap tests/pcap/itch_synthetic.pcap --speedup 0
./build/pcap_replay --pcap tests/pcap/itch_gap.pcap --speedup 0 --skip-gaps
```

CLI flags: `--pcap <path>`, `--speedup <N>` (rate multiplier, `0` = as fast as
possible), `--start-offset <secs>`, `--stop-after <count>`, `--skip-gaps`.

Two captures are committed under `tests/pcap/`: `itch_synthetic.pcap`
(1000 ITCH messages, contiguous sequences) and `itch_gap.pcap` (the same with
transport sequence 500..510 omitted). Replaying the gap capture surfaces
exactly one gap with missing range `[500, 510]`. Both come from `pcap_gen`;
regenerate with `make pcaps`. The replay test validates the handler against
an independent direct decode of the committed capture, so it is robust
regardless of which platform generated the pcap. Full write-up:
`docs/pcap-replay.md`.

## What this is NOT

- **No production exchange connectivity.** Loopback multicast only. No
  real-network NASDAQ feed, no TLS, no MoldUDP64.
- **No UDP unicast.** That is `SAY-5/mdfeed-handler`.
- **No FIX.** Use `SAY-5/orderbook-fix` for order-entry over FIX.
- **No order entry at all.** This is read-side market data.
- **No `.itch` flat-file ingestion.** The offline path is a libpcap replay
  (`pcap_replay`), not a raw NASDAQ `.itch` file loader.
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
│   ├── pcap/        # libpcap-format offline capture I/O
│   ├── obs/         # clock, histogram
│   ├── pcap_replay.cpp  # offline pcap replay tool
│   ├── pcap_gen.cpp     # test-pcap generator
│   └── main.cpp
├── tests/
│   ├── unit/        # parser, depth book, gap detector, snapshot client
│   ├── fuzz/        # libFuzzer target for the parser
│   ├── pcap/        # committed pcap-replay test captures
│   └── integration/ # multicast loopback, gap-fill recovery, pcap replay
├── bench/           # handler bench, multicast bench, cache study, results
├── docs/            # itch-wire, depth-book, gap-fill-recovery, pcap-replay, ...
└── .github/workflows/ci.yml
```

3,635 LOC across C++ source, tests, and bench. 37 distinct test cases across
6 test executables. 100% test pass on Linux gcc, Linux clang, and the ASan +
UBSan sanitized build.

## License

MIT, see `LICENSE`.
