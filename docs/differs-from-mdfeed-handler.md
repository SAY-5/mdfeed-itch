# How mdfeed-itch differs from mdfeed-handler

Both repos study market-data ingest from low-level UDP, but they target
deliberately different problems.

| Axis                | `mdfeed-handler`                       | `mdfeed-itch` (this repo)              |
|---------------------|----------------------------------------|-----------------------------------------|
| Wire protocol       | Custom binary + ASCII venue formats    | NASDAQ TotalView-ITCH 5.0 (subset)      |
| Transport           | UDP unicast on loopback                | UDP multicast (`239.0.0.x`) on loopback |
| Book model          | Best bid / best offer (BBO) per symbol | Depth-10 per symbol, full price ladder  |
| Recovery            | None; lost packets are lost            | Snapshot + gap-fill over TCP            |
| Sequence model      | Implicit, per-stream                   | Explicit per-stock-locate sequence      |
| Latency target      | Sub-microsecond BBO update             | Sub-microsecond book apply, P99 < 5us   |

## Why both repos

ITCH and BBO solve different parts of the same pipeline:

- `mdfeed-handler` focuses on the *normalization* problem: ingesting multiple
  venues with different wire formats and reducing to a single internal BBO
  view. It does not address recovery.
- `mdfeed-itch` focuses on the *replication* problem: a single venue's full
  book is streamed over a lossy transport, and lost packets must be detected
  and recovered without resync.

A production system would generally use the patterns from both. The handler
in this repo could be paired with the venue-normalization layer in
`mdfeed-handler` to produce a unified ITCH-normalized BBO with gap-fill.

## Concretely shared

- C++20, CMake, GoogleTest via FetchContent.
- Hermetic CI: all multicast / unicast tests run on `127.0.0.1`.
- Latency histograms over `CLOCK_MONOTONIC`-class timers.
- Multi-stage Dockerfile producing a minimal runtime image.

## Concretely different

- This repo's `core::FeedHandler` is *stateful*: every order id is tracked so
  Execute / Cancel / Delete / Replace can mutate the ladder correctly.
- This repo emits *depth snapshots* on every state change rather than BBO
  ticks.
- This repo's `recovery::` layer is the entire reason it exists. The handler
  in `mdfeed-handler` has no equivalent.
- This repo's transport carries an explicit per-locate sequence number; the
  handler in `mdfeed-handler` relies on packet arrival order alone.
